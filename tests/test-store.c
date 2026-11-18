/* SPDX-FileCopyrightText: 2026 Arthur <arthur63@163.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "i18n.h"
#include "store.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct
{
  gchar     *dir, *path;
  GPtrArray *chats, *deleted_ids;
} Fixture;

static void
setup (Fixture      *f,
       gconstpointer data G_GNUC_UNUSED)
{
  g_autoptr (GError) error = NULL;
  f->dir = g_dir_make_tmp ("gchat-store-test-XXXXXX", &error);
  g_assert_no_error (error);
  f->path = g_build_filename (f->dir, "session.db", NULL);
  f->chats = g_ptr_array_new_with_free_func ((GDestroyNotify) chat_free);
  f->deleted_ids = g_ptr_array_new_with_free_func (g_free);
}

static void
teardown (Fixture      *f,
          gconstpointer data G_GNUC_UNUSED)
{
  g_ptr_array_unref (f->chats);
  g_ptr_array_unref (f->deleted_ids);
  g_unlink (f->path);
  g_assert_cmpint (g_rmdir (f->dir), ==, 0);
  g_free (f->path);
  g_free (f->dir);
}

static Chat *
add_chat (Fixture     *f,
          const gchar *id)
{
  Chat *chat = chat_new (id, "");
  g_ptr_array_add (f->chats, chat);
  return chat;
}

static JsonObject *
add_message (Chat        *chat,
             const gchar *role,
             const gchar *text)
{
  JsonObject *message = json_object_new ();
  json_object_set_string_member (message, "role", role);
  json_object_set_string_member (message, "content", text);
  json_array_add_object_element (chat->messages, message);
  return message;
}

static sqlite3 *
open_db (Fixture *f)
{
  g_autoptr (GError) error = NULL;
  sqlite3 *db = store_open (f->path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (db);
  return db;
}

static sqlite3 *
open_raw (Fixture *f)
{
  sqlite3 *db = NULL;
  g_assert_cmpint (sqlite3_open (f->path, &db), ==, SQLITE_OK);
  return db;
}

static void
exec_sql (sqlite3     *db,
          const gchar *sql)
{
  gint rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
  if (rc != SQLITE_OK)
    g_error ("%s: %s", sql, sqlite3_errmsg (db));
}

static gint
scalar_int (sqlite3     *db,
            const gchar *sql)
{
  sqlite3_stmt *stmt = NULL;
  gint rc = sqlite3_prepare_v2 (db, sql, -1, &stmt, NULL);
  g_assert_cmpint (rc, ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_step (stmt), ==, SQLITE_ROW);
  gint value = sqlite3_column_int (stmt, 0);
  g_assert_cmpint (sqlite3_finalize (stmt), ==, SQLITE_OK);
  return value;
}

static void
save (Fixture     *f,
      sqlite3     *db,
      const gchar *current)
{
  g_autoptr (GError) error = NULL;
  g_assert_true (store_save (db, f->chats, f->deleted_ids, current,
                             "deepseek-flash", "high", &error));
  g_assert_no_error (error);
}

static GPtrArray *
load (sqlite3     *db,
      const gchar *expected_id)
{
  g_autofree gchar *current = NULL;
  g_autofree gchar *model = NULL;
  g_autofree gchar *effort = NULL;
  g_autoptr (GError) error = NULL;
  GPtrArray *chats = store_load (db, &current, &model, &effort, &error);
  g_assert_no_error (error);
  g_assert_nonnull (chats);
  g_assert_cmpstr (current, ==, expected_id);
  if (expected_id != NULL)
    {
      g_assert_cmpstr (model, ==, "deepseek-flash");
      g_assert_cmpstr (effort, ==, "high");
    }
  return chats;
}

static void
roundtrip (Fixture      *f,
           gconstpointer data G_GNUC_UNUSED)
{
  g_autoptr (sqlite3) db = open_db (f);
  g_assert_cmpint (scalar_int (db, "PRAGMA user_version"), ==, 1);
  g_autoptr (GPtrArray) empty = load (db, NULL);
  g_assert_cmpuint (empty->len, ==, 0);
  struct stat st;
  g_assert_cmpint (g_stat (f->path, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);

  Chat *chat = add_chat (f, "42");
  chat->pinned = TRUE;
  g_free (chat->draft);
  chat->draft = g_strdup ("  普通草稿\n ");
  chat->edit_from = 0;
  g_free (chat->edit_text);
  chat->edit_text = g_strdup ("编辑草稿");
  add_message (chat, "user", "问题");
  JsonObject *reply = add_message (chat, "assistant", "答案");
  json_object_set_string_member (reply, "reasoning", "已收到的推理");
  json_object_set_double_member (reply, "duration", 1.5);
  save (f, db, chat->id);
  g_assert_false (chat->dirty);
  g_clear_pointer (&db, sqlite3_close);
  db = open_db (f);

  g_autoptr (GPtrArray) restored = load (db, chat->id);
  g_assert_cmpuint (restored->len, ==, 1);
  const Chat *saved = g_ptr_array_index (restored, 0);
  g_assert_cmpstr (saved->title, ==, "");
  g_assert_cmpstr (saved->draft, ==, chat->draft);
  g_assert_cmpstr (saved->edit_text, ==, "编辑草稿");
  g_assert_cmpuint (saved->edit_from, ==, 0);
  g_assert_cmpint (saved->created, ==, chat->created);
  g_assert_true (saved->pinned);
  g_assert_false (saved->dirty);
  g_assert_cmpuint (json_array_get_length (saved->messages), ==, 2);
  JsonObject *first = json_array_get_object_element (saved->messages, 0);
  g_assert_cmpstr (json_object_get_string_member (first, "content"), ==,
                   "问题");
  reply = json_array_get_object_element (saved->messages, 1);
  g_assert_cmpstr (json_object_get_string_member (reply, "reasoning"), ==,
                   "已收到的推理");
  g_assert_cmpfloat (json_object_get_double_member (reply, "duration"), ==,
                     1.5);
}

static void
incremental (Fixture      *f,
             gconstpointer data G_GNUC_UNUSED)
{
  g_autoptr (sqlite3) db = open_db (f);
  Chat *chat = add_chat (f, "first");
  add_chat (f, "untouched");
  save (f, db, chat->id);
  exec_sql (db, "CREATE TRIGGER protect_chat BEFORE UPDATE ON chats"
                " WHEN old.id = 'untouched' BEGIN"
                " SELECT RAISE(ABORT, 'unchanged chat written'); END");
  sqlite3_int64 changes = sqlite3_total_changes64 (db);
  save (f, db, chat->id);
  g_assert_cmpint (sqlite3_total_changes64 (db), ==, changes);
  g_free (chat->draft);
  chat->draft = g_strdup ("Changed draft");
  chat->dirty = TRUE;
  save (f, db, chat->id);
  g_assert_cmpint (sqlite3_total_changes64 (db), ==, changes + 1);
  g_autoptr (GPtrArray) restored = load (db, chat->id);
  g_assert_cmpuint (restored->len, ==, 2);
  Chat *saved = g_ptr_array_index (restored, 0);
  g_assert_cmpstr (saved->draft, ==, "Changed draft");
}

static void
save_rollback (Fixture      *f,
               gconstpointer data G_GNUC_UNUSED)
{
  g_autoptr (sqlite3) db = open_db (f);
  add_chat (f, "old");
  save (f, db, "old");
  g_ptr_array_add (f->deleted_ids, g_strdup ("old"));
  g_ptr_array_remove_index (f->chats, 0);
  Chat *chat = add_chat (f, "new");
  exec_sql (db, "CREATE TRIGGER fail_delete BEFORE DELETE ON chats BEGIN"
                " SELECT RAISE(ABORT, 'injected delete failure'); END");
  g_autoptr (GError) error = NULL;
  g_assert_false (store_save (db, f->chats, f->deleted_ids, chat->id,
                              "deepseek-flash", "high", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_true (chat->dirty);
  g_assert_cmpuint (f->deleted_ids->len, ==, 1);
  g_autoptr (GPtrArray) before = load (db, "old");
  g_assert_cmpuint (before->len, ==, 1);

  exec_sql (db, "DROP TRIGGER fail_delete");
  save (f, db, chat->id);
  g_assert_false (chat->dirty);
  g_assert_cmpuint (f->deleted_ids->len, ==, 0);
  g_autoptr (GPtrArray) after = load (db, chat->id);
  g_assert_cmpuint (after->len, ==, 1);
  Chat *saved = g_ptr_array_index (after, 0);
  g_assert_cmpstr (saved->id, ==, "new");
}

static void
upgrade_rollback (Fixture      *f,
                  gconstpointer data G_GNUC_UNUSED)
{
  g_autoptr (sqlite3) reader = open_raw (f);
  exec_sql (reader, "BEGIN; SELECT count(*) FROM sqlite_schema");
  /* The read lock lets the upgrade write but prevents its commit. */
  g_autoptr (GError) error = NULL;
  g_autoptr (sqlite3) failed = store_open (f->path, &error);
  g_assert_null (failed);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  exec_sql (reader, "COMMIT");
  g_assert_cmpint (scalar_int (reader, "PRAGMA user_version"), ==, 0);
  g_assert_cmpint (scalar_int (reader, "SELECT count(*) FROM sqlite_schema"),
                   ==, 0);
  g_autoptr (sqlite3) opened = open_db (f);
  g_assert_cmpint (scalar_int (opened, "PRAGMA user_version"), ==, 1);
}

static void
unsupported_schema (Fixture      *f,
                    gconstpointer data)
{
  gboolean newer = data != NULL;
  g_autoptr (sqlite3) raw = open_raw (f);
  exec_sql (raw, "CREATE TABLE contents (value INTEGER);"
                 " INSERT INTO contents VALUES (98)");
  if (newer)
    exec_sql (raw, "PRAGMA user_version = 99");
  g_autoptr (GError) error = NULL;
  g_autoptr (sqlite3) db = store_open (f->path, &error);
  g_assert_null (db);
  gint code = newer ? G_IO_ERROR_NOT_SUPPORTED : G_IO_ERROR_INVALID_DATA;
  g_assert_error (error, G_IO_ERROR, code);
  g_assert_cmpint (scalar_int (raw, "PRAGMA user_version"), ==, newer ? 99 : 0);
  g_assert_cmpint (scalar_int (raw, "SELECT value FROM contents"), ==, 98);
  g_assert_cmpint (scalar_int (raw, "SELECT count(*) FROM sqlite_schema"),
                   ==, 1);
}

static void
invalid_row (Fixture      *f,
             gconstpointer data)
{
  g_autoptr (sqlite3) db = open_db (f);
  Chat *chat = add_chat (f, "42");
  add_message (chat, "user", "Question");
  add_message (chat, "assistant", "Answer");
  save (f, db, chat->id);
  exec_sql (db, data);
  g_autofree gchar *current = NULL;
  g_autofree gchar *model = NULL;
  g_autofree gchar *effort = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) chats = store_load (db, &current, &model, &effort,
                                            &error);
  g_assert_null (chats);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_null (current);
  g_assert_null (model);
  g_assert_null (effort);
}

static void
session_lock (Fixture      *f,
              gconstpointer data G_GNUC_UNUSED)
{
  g_autoptr (GError) error = NULL;
  gint first = store_lock (f->path, &error);
  g_assert_no_error (error);
  g_assert_cmpint (first, >=, 0);
  g_assert_cmpint (store_lock (f->path, &error), ==, -1);
  g_assert_nonnull (error);
  close (first);
  g_clear_error (&error);
  gint second = store_lock (f->path, &error);
  g_assert_no_error (error);
  g_assert_cmpint (second, >=, 0);
  close (second);
  g_autofree gchar *path = g_strconcat (f->path, ".lock", NULL);
  g_assert_cmpint (g_unlink (path), ==, 0);
}

static gchar *
image_file (const gchar *dir)
{
  g_autofree gchar *id = g_uuid_string_random ();
  g_autofree gchar *path = g_build_filename (dir, id, NULL);
  g_assert_true (g_file_set_contents (path, "image", -1, NULL));
  return g_steal_pointer (&path);
}

static void
image_storage (Fixture      *f,
               gconstpointer data G_GNUC_UNUSED)
{
  g_autofree gchar *dir = store_image_dir (f->path);
  g_assert_cmpint (g_mkdir (dir, 0700), ==, 0);
  g_autofree gchar *draft = image_file (dir);
  g_autofree gchar *edit = image_file (dir);
  g_autofree gchar *sent = image_file (dir);
  g_autofree gchar *other = image_file (dir);
  g_autofree gchar *orphan = image_file (dir);
  g_autofree gchar *old_dir = g_build_filename (f->dir, "images", NULL);
  g_assert_cmpint (g_mkdir (old_dir, 0700), ==, 0);
  g_autofree gchar *old_image = image_file (old_dir);
  g_autofree gchar *old_json = g_build_filename (f->dir, "session.json", NULL);
  g_assert_true (g_file_set_contents (old_json, "legacy", -1, NULL));
  g_autoptr (sqlite3) db = open_db (f);
  g_autoptr (GPtrArray) initial = load (db, NULL);
  g_assert_cmpuint (initial->len, ==, 0);

  Chat *chat = add_chat (f, "42");
  JsonObject *msg = add_message (chat, "user", "");
  g_autofree gchar *draft_id = g_path_get_basename (draft);
  g_autofree gchar *edit_id = g_path_get_basename (edit);
  g_autofree gchar *sent_id = g_path_get_basename (sent);
  g_autofree gchar *other_id = g_path_get_basename (other);
  chat->draft_images = json_array_new ();
  json_array_add_string_element (chat->draft_images, draft_id);
  chat->edit_from = 0;
  chat->edit_images = json_array_new ();
  json_array_add_string_element (chat->edit_images, edit_id);
  g_autoptr (JsonArray) sent_ids = json_array_new ();
  json_array_add_string_element (sent_ids, sent_id);
  json_array_add_string_element (sent_ids, other_id);
  json_object_set_array_member (msg, "images", g_steal_pointer (&sent_ids));
  save (f, db, chat->id);

  g_autoptr (GPtrArray) restored = load (db, chat->id);
  const Chat *saved = g_ptr_array_index (restored, 0);
  JsonObject *message = json_array_get_object_element (saved->messages, 0);
  JsonArray *images = image_array_member (message, "images");
  g_assert_cmpstr (json_array_get_string_element (images, 0), ==, sent_id);
  g_assert_cmpstr (json_array_get_string_element (images, 1), ==, other_id);
  store_cleanup_images (f->path, restored);
  g_assert_false (g_file_test (orphan, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_test (draft, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_test (edit, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_test (sent, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_test (other, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_test (old_image, G_FILE_TEST_EXISTS));
  g_autofree gchar *legacy = NULL;
  g_assert_true (g_file_get_contents (old_json, &legacy, NULL, NULL));
  g_assert_cmpstr (legacy, ==, "legacy");

  g_ptr_array_add (f->deleted_ids, g_strdup (chat->id));
  g_ptr_array_set_size (f->chats, 0);
  save (f, db, NULL);
  g_autoptr (GPtrArray) empty = load (db, NULL);
  store_cleanup_images (f->path, empty);
  g_assert_cmpint (g_rmdir (dir), ==, 0);
  g_assert_cmpint (g_unlink (old_json), ==, 0);
  g_assert_cmpint (g_unlink (old_image), ==, 0);
  g_assert_cmpint (g_rmdir (old_dir), ==, 0);
}

static void
invalid_config (gconstpointer data)
{
  g_autofree gchar *dir =
      g_build_filename (g_get_user_config_dir (), "gchat", NULL);
  g_autofree gchar *path = g_build_filename (dir, "config.json", NULL);
  g_assert_cmpint (g_mkdir (dir, 0700), ==, 0);
  g_assert_true (g_file_set_contents (path, data, -1, NULL));
  g_autofree gchar *value = config_get ("api_key");
  g_assert_null (value);
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "cannot write *");
  g_assert_false (config_set ("theme", "dark"));
  g_test_assert_expected_messages ();
  g_autofree gchar *primary = NULL;
  g_assert_true (g_file_get_contents (path, &primary, NULL, NULL));
  g_assert_cmpstr (primary, ==, data);
  g_unlink (path);
  g_rmdir (dir);
}

static void
config_roundtrip (void)
{
  g_assert_true (config_set ("api_key", "first-key"));
  g_assert_true (config_set ("theme", "dark"));
  g_assert_true (config_set ("api_key", "second-key"));
  g_autofree gchar *key = config_get ("api_key");
  g_autofree gchar *theme = config_get ("theme");
  g_assert_cmpstr (key, ==, "second-key");
  g_assert_cmpstr (theme, ==, "dark");
  g_autofree gchar *dir =
      g_build_filename (g_get_user_config_dir (), "gchat", NULL);
  g_autofree gchar *path = g_build_filename (dir, "config.json", NULL);
  g_autofree gchar *bak = g_strconcat (path, ".bak", NULL);
  g_assert_false (g_file_test (bak, G_FILE_TEST_EXISTS));
  struct stat st;
  g_assert_cmpint (g_stat (path, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);
  g_unlink (path);
  g_rmdir (dir);
}

int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_autofree gchar *config_dir =
      g_dir_make_tmp ("gchat-config-test-XXXXXX", NULL);
  g_assert_nonnull (config_dir);
  g_setenv ("XDG_CONFIG_HOME", config_dir, TRUE);
  i18n_init (NULL);
  g_test_add ("/store/roundtrip", Fixture, NULL, setup, roundtrip,
              teardown);
  g_test_add ("/store/incremental", Fixture, NULL, setup, incremental,
              teardown);
  g_test_add ("/store/save-rollback", Fixture, NULL, setup, save_rollback,
              teardown);
  g_test_add ("/store/upgrade-rollback", Fixture, NULL, setup, upgrade_rollback,
              teardown);
  g_test_add ("/store/images", Fixture, NULL, setup, image_storage,
              teardown);
  g_test_add ("/store/lock", Fixture, NULL, setup, session_lock,
              teardown);
  g_test_add ("/store/unversioned", Fixture, NULL, setup, unsupported_schema,
              teardown);
  g_test_add ("/store/newer", Fixture, GINT_TO_POINTER (1), setup,
              unsupported_schema, teardown);
  static const gchar *invalid[] = {
    "UPDATE chats SET messages = '{ broken'",
    "UPDATE chats SET messages = '[null]'",
    "UPDATE chats SET messages = '[{\"role\":\"system\",\"content\":\"x\"}]'",
    "UPDATE chats SET draft_images = '[\"../outside\"]'",
    "UPDATE chats SET edit_from = 1",
    "UPDATE chats SET edit_images = '[1]'",
    "UPDATE session_state SET current_id = NULL",
  };
  for (guint i = 0; i < G_N_ELEMENTS (invalid); i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/store/invalid/%u", i);
      g_test_add (name, Fixture, invalid[i], setup, invalid_row, teardown);
    }
  static const gchar *configs[] = { "{ broken", "[]", "null",
                                    "{\"api_key\":42}", "{\"api_key\":null}" };
  for (guint i = 0; i < G_N_ELEMENTS (configs); i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/config/invalid/%u", i);
      g_test_add_data_func (name, configs[i], invalid_config);
    }
  g_test_add_func ("/config/roundtrip-no-backup", config_roundtrip);
  int status = g_test_run ();
  g_rmdir (config_dir);
  return status;
}
