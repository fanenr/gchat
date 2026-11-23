/* SPDX-FileCopyrightText: 2026 Arthur <arthur63@163.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "store.h"
#include "i18n.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <math.h>
#include <sys/file.h>
#include <unistd.h>

Chat *
chat_new (const gchar *id,
          const gchar *title)
{
  g_autoptr (Chat) chat = g_new0 (Chat, 1);
  chat->id = g_strdup (id);
  chat->title = g_strdup (title);
  chat->draft = g_strdup ("");
  chat->edit_text = g_strdup ("");
  chat->edit_from = G_MAXUINT;
  chat->dirty = TRUE;
  chat->created = g_get_real_time () / 1000;
  chat->messages = json_array_new ();
  return g_steal_pointer (&chat);
}

Chat *
chat_new_empty (void)
{
  g_autofree gchar *id = g_uuid_string_random ();
  return chat_new (id, "");
}

void
chat_free (Chat *chat)
{
  if (chat == NULL)
    return;
  g_free (chat->id);
  g_free (chat->title);
  g_free (chat->draft);
  g_free (chat->edit_text);
  g_clear_pointer (&chat->draft_images, json_array_unref);
  g_clear_pointer (&chat->edit_images, json_array_unref);
  json_array_unref (chat->messages);
  g_free (chat);
}

static gchar *
store_user_path (const gchar *xdg_dir,
                 const gchar *host_env,
                 const gchar *home_subdir,
                 const gchar *filename)
{
  /* Flatpak replaces the normal XDG paths with sandbox-private paths. */
  if (g_strcmp0 (g_getenv ("FLATPAK_ID"), GCHAT_APPLICATION_ID) == 0)
    {
      const gchar *host_dir = g_getenv (host_env);
      if (host_dir != NULL && g_path_is_absolute (host_dir))
        xdg_dir = host_dir;
      else
        return g_build_filename (g_get_home_dir (), home_subdir, "gchat",
                                 filename, NULL);
    }

  return g_build_filename (xdg_dir, "gchat", filename, NULL);
}

gchar *
store_session_path (void)
{
  return store_user_path (g_get_user_data_dir (), "HOST_XDG_DATA_HOME",
                          ".local/share", "session.db");
}

gint
store_lock (const gchar *path,
            GError     **error)
{
  g_autofree gchar *dir = g_path_get_dirname (path);
  g_autofree gchar *lock = g_strconcat (path, ".lock", NULL);
  gint fd = -1;
  if (g_mkdir_with_parents (dir, 0700) == 0)
    {
      fd = g_open (lock, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
      if (fd >= 0 && flock (fd, LOCK_EX | LOCK_NB) != 0)
        {
          gint saved_errno = errno;
          close (fd);
          fd = -1;
          errno = saved_errno;
        }
    }
  if (fd < 0)
    g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                 _ ("Cannot lock conversation file %s: %s"), path,
                 g_strerror (errno));
  return fd;
}

gchar *
store_config_path (void)
{
  return store_user_path (g_get_user_config_dir (), "HOST_XDG_CONFIG_HOME",
                          ".config", "config.json");
}

gchar *
store_image_dir (const gchar *path)
{
  g_autofree gchar *dir = g_path_get_dirname (path);
  /* Keep cleanup away from attachments owned by the old session format. */
  return g_build_filename (dir, "attachments", NULL);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (sqlite3_stmt, sqlite3_finalize)

static gboolean
db_error (sqlite3 *db,
          gint     rc,
          GError **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       db != NULL ? sqlite3_errmsg (db) : sqlite3_errstr (rc));
  return FALSE;
}

static gboolean
db_exec (sqlite3     *db,
         const gchar *sql,
         GError     **error)
{
  gint rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
  return rc == SQLITE_OK || db_error (db, rc, error);
}

static sqlite3_stmt *
db_prepare (sqlite3     *db,
            const gchar *sql,
            GError     **error)
{
  sqlite3_stmt *stmt = NULL;
  gint rc = sqlite3_prepare_v2 (db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK)
    db_error (db, rc, error);
  return stmt;
}

static gboolean
db_read_int (sqlite3     *db,
             const gchar *sql,
             gint        *value,
             GError     **error)
{
  g_autoptr (sqlite3_stmt) stmt = db_prepare (db, sql, error);
  if (stmt == NULL)
    return FALSE;
  gint rc = sqlite3_step (stmt);
  if (rc != SQLITE_ROW)
    return db_error (db, rc, error);
  *value = sqlite3_column_int (stmt, 0);
  return TRUE;
}

static gboolean
db_finish (sqlite3  *db,
           gboolean  ok,
           GError  **error)
{
  if (ok && db_exec (db, "COMMIT", error))
    return TRUE;
  if (!sqlite3_get_autocommit (db))
    sqlite3_exec (db, "ROLLBACK", NULL, NULL, NULL);
  return FALSE;
}

static gboolean
create_v1 (sqlite3 *db,
           GError **error)
{
  gint occupied;
  if (!db_read_int (db,
                    "SELECT EXISTS(SELECT 1 FROM sqlite_schema"
                    " WHERE name NOT GLOB 'sqlite_*')",
                    &occupied, error))
    return FALSE;
  if (occupied != 0)
    {
      g_set_error_literal (
          error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
          _ ("Unversioned conversation database is not empty"));
      return FALSE;
    }

  return db_exec (db,
                  "CREATE TABLE chats ("
                  " id TEXT PRIMARY KEY NOT NULL CHECK (id <> ''),"
                  " title TEXT NOT NULL,"
                  " created INTEGER NOT NULL CHECK (created >= 0),"
                  " pinned INTEGER NOT NULL CHECK (pinned IN (0, 1)),"
                  " draft TEXT NOT NULL, draft_images TEXT,"
                  " edit_from INTEGER, edit_text TEXT NOT NULL,"
                  " edit_images TEXT, messages TEXT NOT NULL"
                  ") STRICT;"
                  "CREATE TABLE session_state ("
                  " id INTEGER PRIMARY KEY CHECK (id = 1),"
                  " current_id TEXT REFERENCES chats(id),"
                  " model TEXT NOT NULL, effort TEXT NOT NULL"
                  ") STRICT;"
                  "INSERT INTO session_state VALUES (1, NULL, '', '')",
                  error);
}

static gboolean
upgrade_schema (sqlite3 *db,
                GError **error)
{
  /* The slot number is the version the step expects on entry. */
  static gboolean (*const upgrades[]) (sqlite3 *, GError **) = { create_v1 };
  const gint latest = G_N_ELEMENTS (upgrades);
  gint version;
  if (!db_read_int (db, "PRAGMA user_version", &version, error))
    return FALSE;
  if (version < 0 || version > latest)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   _ ("Unsupported conversation database version %d "
                      "(maximum supported: %d)"),
                   version, latest);
      return FALSE;
    }
  if (version == latest)
    return TRUE;

  for (; version < latest; version++)
    if (!upgrades[version] (db, error))
      return FALSE;
  g_autofree gchar *sql = g_strdup_printf ("PRAGMA user_version = %d", latest);
  return db_exec (db, sql, error);
}

sqlite3 *
store_open (const gchar *path,
            GError     **error)
{
  g_autoptr (sqlite3) db = NULL;
  g_autofree gchar *dir = g_path_get_dirname (path);
  if (g_mkdir_with_parents (dir, 0700) != 0)
    goto file_error;
  /* SQLite inherits the database mode for its rollback journal. */
  gint fd = g_open (path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
  if (fd >= 0)
    close (fd);
  else if (errno != EEXIST)
    goto file_error;

  gint rc = sqlite3_open_v2 (path, &db, SQLITE_OPEN_READWRITE, NULL);
  if (rc != SQLITE_OK)
    {
      db_error (db, rc, error);
      return NULL;
    }
  if (!db_exec (db, "PRAGMA foreign_keys = ON; BEGIN IMMEDIATE", error) ||
      !db_finish (db, upgrade_schema (db, error), error))
    return NULL;
  return g_steal_pointer (&db);

file_error:
  g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
               "%s: %s", path, g_strerror (errno));
  return NULL;
}

static gboolean
string_node_valid (JsonNode *node)
{
  return node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
         json_node_get_value_type (node) == G_TYPE_STRING;
}

static gboolean
string_member_valid (JsonObject  *obj,
                     const gchar *name)
{
  return string_node_valid (json_object_get_member (obj, name));
}

JsonArray *
image_array_member (JsonObject  *object,
                    const gchar *name)
{
  JsonNode *node = json_object_get_member (object, name);
  return node != NULL ? json_node_get_array (node) : NULL;
}

static gboolean
image_id_valid (JsonNode *node)
{
  return string_node_valid (node) &&
         g_uuid_string_is_valid (json_node_get_string (node));
}

static gboolean
number_member_valid (JsonObject  *obj,
                     const gchar *name)
{
  JsonNode *node = json_object_get_member (obj, name);
  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
    return FALSE;
  GType type = json_node_get_value_type (node);
  if (type != G_TYPE_INT64 && type != G_TYPE_DOUBLE)
    return FALSE;
  gdouble value = json_node_get_double (node);
  return isfinite (value) && value >= 0;
}

static gboolean
images_valid (JsonArray *images)
{
  guint count = json_array_get_length (images);
  for (guint i = 0; i < count; i++)
    if (!image_id_valid (json_array_get_element (images, i)))
      return FALSE;
  return count != 0;
}

static gboolean
messages_valid (JsonArray *messages)
{
  for (guint i = 0; i < json_array_get_length (messages); i++)
    {
      JsonNode *node = json_array_get_element (messages, i);
      if (!JSON_NODE_HOLDS_OBJECT (node))
        return FALSE;
      JsonObject *msg = json_node_get_object (node);
      if (!string_member_valid (msg, "role") ||
          !string_member_valid (msg, "content") ||
          (json_object_has_member (msg, "reasoning") &&
           !string_member_valid (msg, "reasoning")) ||
          (json_object_has_member (msg, "duration") &&
           !number_member_valid (msg, "duration")))
        return FALSE;

      const gchar *role = json_object_get_string_member (msg, "role");
      gboolean user = g_str_equal (role, "user");
      if (!user && !g_str_equal (role, "assistant"))
        return FALSE;
      JsonNode *images = json_object_get_member (msg, "images");
      if (images != NULL &&
          (!user || !JSON_NODE_HOLDS_ARRAY (images) ||
           !images_valid (json_node_get_array (images))))
        return FALSE;
    }
  return TRUE;
}

/* Return borrowed text, or NULL for a value that is not valid text. */
static const gchar *
column_text (sqlite3_stmt *stmt,
             gint          column)
{
  if (sqlite3_column_type (stmt, column) != SQLITE_TEXT)
    return NULL;
  const gchar *text = (const gchar *) sqlite3_column_text (stmt, column);
  gint length = sqlite3_column_bytes (stmt, column);
  return text != NULL && g_utf8_validate (text, length, NULL) ? text : NULL;
}

static JsonArray *
column_array (sqlite3_stmt *stmt,
              gint          column)
{
  const gchar *text = column_text (stmt, column);
  if (text == NULL)
    return NULL;
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, text, -1, NULL))
    return NULL;
  JsonNode *root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_ARRAY (root))
    return NULL;
  return json_array_ref (json_node_get_array (root));
}

static gboolean
read_images (sqlite3_stmt *stmt,
             gint          column,
             JsonArray   **images)
{
  if (sqlite3_column_type (stmt, column) == SQLITE_NULL)
    return TRUE;
  *images = column_array (stmt, column);
  return *images != NULL && images_valid (*images);
}

static Chat *
read_chat (sqlite3_stmt *stmt)
{
  const gchar *id = column_text (stmt, 0);
  const gchar *title = column_text (stmt, 1);
  const gchar *draft = column_text (stmt, 4);
  const gchar *edit_text = column_text (stmt, 7);
  if (id == NULL || !*id || title == NULL || draft == NULL ||
      edit_text == NULL || sqlite3_column_type (stmt, 2) != SQLITE_INTEGER ||
      sqlite3_column_type (stmt, 3) != SQLITE_INTEGER)
    return NULL;
  gint64 created = sqlite3_column_int64 (stmt, 2);
  gint64 pinned = sqlite3_column_int64 (stmt, 3);
  if (created < 0 || (pinned != 0 && pinned != 1))
    return NULL;

  g_autoptr (JsonArray) messages = column_array (stmt, 9);
  if (messages == NULL || !messages_valid (messages))
    return NULL;
  g_autoptr (Chat) chat = chat_new (id, title);
  if (!read_images (stmt, 5, &chat->draft_images) ||
      !read_images (stmt, 8, &chat->edit_images))
    return NULL;
  gint edit_type = sqlite3_column_type (stmt, 6);
  if (edit_type == SQLITE_INTEGER)
    {
      gint64 index = sqlite3_column_int64 (stmt, 6);
      if (index < 0 || index >= json_array_get_length (messages))
        return NULL;
      JsonObject *msg = json_array_get_object_element (messages, (guint) index);
      if (!g_str_equal (json_object_get_string_member (msg, "role"), "user"))
        return NULL;
      chat->edit_from = (guint) index;
    }
  else if (edit_type != SQLITE_NULL || *edit_text || chat->edit_images != NULL)
    return NULL;

  chat->created = created;
  chat->pinned = pinned != 0;
  g_free (chat->draft);
  chat->draft = g_strdup (draft);
  g_free (chat->edit_text);
  chat->edit_text = g_strdup (edit_text);
  json_array_unref (chat->messages);
  chat->messages = g_steal_pointer (&messages);
  chat->dirty = FALSE;
  return g_steal_pointer (&chat);
}

static gboolean
invalid_data (GError **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       _ ("Invalid conversation database data"));
  return FALSE;
}

GPtrArray *
store_load (sqlite3  *db,
            gchar   **current_id,
            gchar   **model,
            gchar   **effort,
            GError  **error)
{
  *current_id = NULL;
  *model = NULL;
  *effort = NULL;
  g_autoptr (sqlite3_stmt) rows = db_prepare (
      db, "SELECT id, title, created, pinned, draft, draft_images,"
          " edit_from, edit_text, edit_images, messages FROM chats"
          " ORDER BY rowid",
      error);
  if (rows == NULL)
    return NULL;
  g_autoptr (GPtrArray) chats =
      g_ptr_array_new_with_free_func ((GDestroyNotify) chat_free);
  gint rc;
  while ((rc = sqlite3_step (rows)) == SQLITE_ROW)
    {
      g_autoptr (Chat) chat = read_chat (rows);
      if (chat == NULL)
        {
          invalid_data (error);
          return NULL;
        }
      g_ptr_array_add (chats, g_steal_pointer (&chat));
    }
  if (rc != SQLITE_DONE)
    {
      db_error (db, rc, error);
      return NULL;
    }

  g_autoptr (sqlite3_stmt) state = db_prepare (
      db, "SELECT current_id, model, effort FROM session_state WHERE id = 1",
      error);
  if (state == NULL)
    return NULL;
  rc = sqlite3_step (state);
  if (rc != SQLITE_ROW)
    {
      if (rc == SQLITE_DONE)
        invalid_data (error);
      else
        db_error (db, rc, error);
      return NULL;
    }
  const gchar *current = column_text (state, 0);
  const gchar *saved_model = column_text (state, 1);
  const gchar *saved_effort = column_text (state, 2);
  gboolean found = chats->len == 0 &&
                   sqlite3_column_type (state, 0) == SQLITE_NULL;
  for (guint i = 0; !found && current != NULL && i < chats->len; i++)
    {
      Chat *chat = g_ptr_array_index (chats, i);
      found = g_str_equal (chat->id, current);
    }
  if (!found || saved_model == NULL || saved_effort == NULL)
    {
      invalid_data (error);
      return NULL;
    }

  *current_id = g_strdup (current);
  *model = g_strdup (saved_model);
  *effort = g_strdup (saved_effort);
  return g_steal_pointer (&chats);
}

static gchar *
array_to_text (JsonArray *array)
{
  if (array == NULL)
    return NULL;
  g_autoptr (JsonNode) node = json_node_alloc ();
  json_node_init (node, JSON_NODE_ARRAY);
  json_node_set_array (node, array);
  return json_to_string (node, FALSE);
}

static gint
bind_text (sqlite3_stmt *stmt,
           gint          column,
           const gchar  *value)
{
  return sqlite3_bind_text (stmt, column, value, -1, SQLITE_TRANSIENT);
}

static gboolean
write_row (sqlite3_stmt *stmt,
           GError      **error)
{
  gint rc = sqlite3_step (stmt);
  if (rc != SQLITE_DONE)
    return db_error (sqlite3_db_handle (stmt), rc, error);
  sqlite3_reset (stmt);
  return TRUE;
}

static gboolean
write_chat (sqlite3_stmt *stmt,
            Chat         *chat,
            GError      **error)
{
  g_autofree gchar *draft_images = array_to_text (chat->draft_images);
  g_autofree gchar *edit_images = array_to_text (chat->edit_images);
  g_autofree gchar *messages = array_to_text (chat->messages);
  gint rc;
  if ((rc = bind_text (stmt, 1, chat->id)) != SQLITE_OK ||
      (rc = bind_text (stmt, 2, chat->title)) != SQLITE_OK ||
      (rc = sqlite3_bind_int64 (stmt, 3, chat->created)) != SQLITE_OK ||
      (rc = sqlite3_bind_int (stmt, 4, chat->pinned)) != SQLITE_OK ||
      (rc = bind_text (stmt, 5, chat->draft)) != SQLITE_OK ||
      (rc = bind_text (stmt, 6, draft_images)) != SQLITE_OK ||
      (rc = chat->edit_from == G_MAXUINT
                ? sqlite3_bind_null (stmt, 7)
                : sqlite3_bind_int64 (stmt, 7, chat->edit_from)) != SQLITE_OK ||
      (rc = bind_text (stmt, 8, chat->edit_text)) != SQLITE_OK ||
      (rc = bind_text (stmt, 9, edit_images)) != SQLITE_OK ||
      (rc = bind_text (stmt, 10, messages)) != SQLITE_OK)
    return db_error (sqlite3_db_handle (stmt), rc, error);
  return write_row (stmt, error);
}

static gboolean
write_changes (sqlite3     *db,
               GPtrArray   *chats,
               GPtrArray   *deleted_ids,
               const gchar *current_id,
               const gchar *model,
               const gchar *effort,
               GError     **error)
{
  g_autoptr (sqlite3_stmt) upsert = NULL;
  for (guint i = 0; i < chats->len; i++)
    {
      Chat *chat = g_ptr_array_index (chats, i);
      if (!chat->dirty)
        continue;
      if (upsert == NULL)
        upsert = db_prepare (
            db, "INSERT INTO chats (id, title, created, pinned, draft,"
                " draft_images, edit_from, edit_text, edit_images, messages)"
                " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
                " ON CONFLICT(id) DO UPDATE SET title=excluded.title,"
                " created=excluded.created, pinned=excluded.pinned,"
                " draft=excluded.draft, draft_images=excluded.draft_images,"
                " edit_from=excluded.edit_from, edit_text=excluded.edit_text,"
                " edit_images=excluded.edit_images, messages=excluded.messages",
            error);
      if (upsert == NULL || !write_chat (upsert, chat, error))
        return FALSE;
    }

  /* Move the current-session reference before applying deletions. */
  g_autoptr (sqlite3_stmt) state = db_prepare (
      db, "UPDATE session_state SET current_id=?1, model=?2, effort=?3"
          " WHERE id=1 AND (current_id IS NOT ?1 OR model IS NOT ?2"
          " OR effort IS NOT ?3)",
      error);
  if (state == NULL)
    return FALSE;
  gint rc;
  if ((rc = bind_text (state, 1, current_id)) != SQLITE_OK ||
      (rc = bind_text (state, 2, model)) != SQLITE_OK ||
      (rc = bind_text (state, 3, effort)) != SQLITE_OK)
    return db_error (db, rc, error);
  if (!write_row (state, error))
    return FALSE;

  if (deleted_ids->len == 0)
    return TRUE;
  g_autoptr (sqlite3_stmt) remove = db_prepare (
      db, "DELETE FROM chats WHERE id=?", error);
  if (remove == NULL)
    return FALSE;
  for (guint i = 0; i < deleted_ids->len; i++)
    {
      rc = bind_text (remove, 1, g_ptr_array_index (deleted_ids, i));
      if (rc != SQLITE_OK)
        return db_error (db, rc, error);
      if (!write_row (remove, error))
        return FALSE;
    }
  return TRUE;
}

gboolean
store_save (sqlite3     *db,
            GPtrArray   *chats,
            GPtrArray   *deleted_ids,
            const gchar *current_id,
            const gchar *model,
            const gchar *effort,
            GError     **error)
{
  if ((current_id == NULL) != (chats->len == 0))
    return invalid_data (error);
  if (!db_exec (db, "BEGIN IMMEDIATE", error))
    return FALSE;
  gboolean ok = write_changes (db, chats, deleted_ids, current_id, model,
                               effort, error);
  if (!db_finish (db, ok, error))
    return FALSE;
  for (guint i = 0; i < chats->len; i++)
    {
      Chat *chat = g_ptr_array_index (chats, i);
      chat->dirty = FALSE;
    }
  g_ptr_array_set_size (deleted_ids, 0);
  return TRUE;
}

static void
collect_image_array (GHashTable *refs,
                     JsonArray  *images)
{
  if (images == NULL)
    return;
  for (guint i = 0; i < json_array_get_length (images); i++)
    {
      const gchar *id = json_array_get_string_element (images, i);
      g_hash_table_add (refs, (gpointer) id);
    }
}

void
store_cleanup_images (const gchar *path,
                      GPtrArray   *chats)
{
  g_autoptr (GHashTable) refs = g_hash_table_new (g_str_hash, g_str_equal);
  for (guint i = 0; i < chats->len; i++)
    {
      const Chat *chat = g_ptr_array_index (chats, i);
      collect_image_array (refs, chat->draft_images);
      collect_image_array (refs, chat->edit_images);
      for (guint j = 0; j < json_array_get_length (chat->messages); j++)
        {
          JsonObject *msg = json_array_get_object_element (chat->messages, j);
          collect_image_array (refs, image_array_member (msg, "images"));
        }
    }

  g_autofree gchar *dir = store_image_dir (path);
  g_autoptr (GDir) folder = g_dir_open (dir, 0, NULL);
  if (folder == NULL)
    return;
  const gchar *name;
  while ((name = g_dir_read_name (folder)) != NULL)
    if (g_uuid_string_is_valid (name) && !g_hash_table_contains (refs, name))
      {
        g_autofree gchar *file = g_build_filename (dir, name, NULL);
        if (g_unlink (file) != 0)
          g_warning ("cannot remove unused image %s: %s", file,
                     g_strerror (errno));
      }
}

/* Consume root and replace the configuration atomically. */
static gboolean
write_config (const gchar *path,
              JsonObject  *root)
{
  g_autoptr (JsonNode) node = json_node_alloc ();
  json_node_init (node, JSON_NODE_OBJECT);
  json_node_take_object (node, root);
  g_autoptr (JsonGenerator) gen = json_generator_new ();
  json_generator_set_root (gen, node);
  json_generator_set_pretty (gen, TRUE);
  gsize len;
  g_autofree gchar *data = json_generator_to_data (gen, &len);

  g_autofree gchar *dir = g_path_get_dirname (path);

  /* Replace the target file only after the temporary file is written. */
  g_autofree gchar *tmp = g_strconcat (path, ".tmp", NULL);
  g_autoptr (GError) error = NULL;
  /* Use 0600 because config.json holds the API key. */
  gboolean ok = g_mkdir_with_parents (dir, 0700) == 0;
  if (!ok)
    g_set_error (&error, G_FILE_ERROR, g_file_error_from_errno (errno), "%s",
                 g_strerror (errno));
  if (ok)
    ok = g_file_set_contents_full (
        tmp, data, (gssize) len, G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error);
  if (ok && g_rename (tmp, path) != 0)
    {
      ok = FALSE;
      g_set_error (&error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "%s", g_strerror (errno));
    }
  if (!ok)
    {
      g_warning ("cannot write %s: %s", path, error->message);
      g_unlink (tmp);
    }

  return ok;
}

gchar *
config_get (const gchar *name)
{
  g_autofree gchar *path = store_config_path ();
  g_autofree gchar *value = NULL;
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (json_parser_load_from_file (parser, path, NULL))
    {
      JsonNode *root = json_parser_get_root (parser);
      if (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
        {
          JsonNode *member =
              json_object_get_member (json_node_get_object (root), name);
          if (member != NULL && JSON_NODE_HOLDS_VALUE (member) &&
              json_node_get_value_type (member) == G_TYPE_STRING)
            {
              value = g_strdup (json_node_get_string (member));
            }
        }
    }
  if (value != NULL && !*value)
    return NULL;
  return g_steal_pointer (&value);
}

gboolean
config_set (const gchar *name,
            const gchar *value)
{
  g_autofree gchar *path = store_config_path ();
  g_autoptr (JsonObject) root = NULL;

  /* Refuse to write when the existing configuration does not parse, so the key
     and other settings are not lost. */
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GError) error = NULL;
  gboolean loaded = json_parser_load_from_file (parser, path, &error);
  JsonNode *node = loaded ? json_parser_get_root (parser) : NULL;
  gboolean valid = node != NULL && JSON_NODE_HOLDS_OBJECT (node);
  if (valid)
    {
      JsonObject *old = json_node_get_object (node);
      g_autoptr (GList) members = json_object_get_members (old);
      for (GList *it = members; it != NULL; it = it->next)
        {
          const gchar *key = it->data;
          if (!string_member_valid (old, key))
            {
              valid = FALSE;
              break;
            }
        }
      if (valid)
        root = json_object_ref (old);
    }
  gboolean missing = g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT);
  if (!valid && !missing)
    {
      g_warning ("cannot write %s: %s", path,
                 error != NULL ? error->message
                               : "invalid configuration structure");
      return FALSE;
    }
  if (missing)
    root = json_object_new ();

  json_object_set_string_member (root, name, value != NULL ? value : "");
  return write_config (path, g_steal_pointer (&root));
}
