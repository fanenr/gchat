/* SPDX-FileCopyrightText: 2026 Arthur <arthur63@163.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "store.h"
#include "i18n.h"

#include <errno.h>
#include <fcntl.h>
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

gchar *
store_session_path (void)
{
  return g_build_filename (g_get_user_data_dir (), "gchat", "session.json",
                           NULL);
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
  return g_build_filename (g_get_user_config_dir (), "gchat", "config.json",
                           NULL);
}

/* Consume root; a session keeps the previous snapshot, a configuration is only
   replaced atomically. */
static gboolean
write_json (const gchar *path,
            JsonObject  *root,
            gboolean     backup)
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
  if (ok)
    {
      g_autofree gchar *bak = backup ? g_strconcat (path, ".bak", NULL) : NULL;
      gboolean moved = FALSE;
      if (backup)
        {
          moved = g_rename (path, bak) == 0;
          ok = moved || errno == ENOENT;
        }
      if (!ok || g_rename (tmp, path) != 0)
        {
          ok = FALSE;
          g_set_error (&error, G_FILE_ERROR, g_file_error_from_errno (errno),
                       "%s", g_strerror (errno));
          if (moved)
            g_rename (bak, path);
        }
    }
  if (!ok)
    {
      g_warning ("cannot write %s: %s", path, error->message);
      g_unlink (tmp);
    }

  return ok;
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

/* Turn the legacy single UUID into the array used by current sessions. */
static gboolean
normalize_image_member (JsonObject  *obj,
                        const gchar *name,
                        const gchar *legacy_name)
{
  JsonNode *node = json_object_get_member (obj, name);
  JsonNode *legacy = json_object_get_member (obj, legacy_name);
  if (legacy != NULL)
    {
      if (node != NULL || !image_id_valid (legacy))
        return FALSE;
      g_autoptr (JsonArray) images = json_array_new ();
      json_array_add_string_element (images, json_node_get_string (legacy));
      json_object_set_array_member (obj, name, g_steal_pointer (&images));
      json_object_remove_member (obj, legacy_name);
      return TRUE;
    }
  if (node == NULL)
    return TRUE;
  if (!JSON_NODE_HOLDS_ARRAY (node))
    return FALSE;
  JsonArray *images = json_node_get_array (node);
  guint count = json_array_get_length (images);
  for (guint i = 0; i < count; i++)
    if (!image_id_valid (json_array_get_element (images, i)))
      return FALSE;
  if (count == 0)
    json_object_remove_member (obj, name);
  return TRUE;
}

static gboolean
number_member_valid (JsonObject  *obj,
                     const gchar *name,
                     gboolean     integer)
{
  JsonNode *node = json_object_get_member (obj, name);
  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
    return FALSE;
  GType type = json_node_get_value_type (node);
  if (type != G_TYPE_INT64 && (integer || type != G_TYPE_DOUBLE))
    return FALSE;
  gdouble value = json_node_get_double (node);
  return isfinite (value) && value >= 0;
}

static gboolean
normalize_session (JsonNode *node)
{
  if (node == NULL || !JSON_NODE_HOLDS_OBJECT (node))
    return FALSE;
  JsonObject *root = json_node_get_object (node);
  if (!string_member_valid (root, "current") ||
      !string_member_valid (root, "model") ||
      !string_member_valid (root, "effort"))
    {
      return FALSE;
    }
  JsonNode *array_node = json_object_get_member (root, "chats");
  if (array_node == NULL || !JSON_NODE_HOLDS_ARRAY (array_node))
    return FALSE;
  g_autoptr (GHashTable) ids = g_hash_table_new (g_str_hash, g_str_equal);
  JsonArray *array = json_node_get_array (array_node);
  for (guint i = 0; i < json_array_get_length (array); i++)
    {
      JsonNode *chat_node = json_array_get_element (array, i);
      if (!JSON_NODE_HOLDS_OBJECT (chat_node))
        return FALSE;
      JsonObject *chat = json_node_get_object (chat_node);
      if (!string_member_valid (chat, "id") ||
          !*json_object_get_string_member (chat, "id") ||
          !string_member_valid (chat, "title") ||
          !string_member_valid (chat, "draft") ||
          !normalize_image_member (chat, "draft_images", "draft_image") ||
          !number_member_valid (chat, "created", TRUE))
        {
          return FALSE;
        }
      JsonNode *pinned = json_object_get_member (chat, "pinned");
      if (pinned != NULL &&
          (!JSON_NODE_HOLDS_VALUE (pinned) ||
           json_node_get_value_type (pinned) != G_TYPE_BOOLEAN))
        return FALSE;
      const gchar *id = json_object_get_string_member (chat, "id");
      if (!g_hash_table_add (ids, (gpointer) id))
        return FALSE;
      JsonNode *messages_node = json_object_get_member (chat, "messages");
      if (messages_node == NULL || !JSON_NODE_HOLDS_ARRAY (messages_node))
        return FALSE;
      JsonArray *messages = json_node_get_array (messages_node);
      JsonNode *edit_node = json_object_get_member (chat, "edit");
      if (edit_node != NULL)
        {
          if (!JSON_NODE_HOLDS_OBJECT (edit_node))
            return FALSE;
          JsonObject *edit = json_node_get_object (edit_node);
          if (!number_member_valid (edit, "index", TRUE) ||
              !string_member_valid (edit, "text") ||
              !normalize_image_member (edit, "images", "image"))
            {
              return FALSE;
            }
          gint64 index = json_object_get_int_member (edit, "index");
          if (index >= json_array_get_length (messages))
            return FALSE;
          JsonNode *target = json_array_get_element (messages, (guint) index);
          if (!JSON_NODE_HOLDS_OBJECT (target) ||
              !string_member_valid (json_node_get_object (target), "role") ||
              g_strcmp0 (json_object_get_string_member (
                             json_node_get_object (target), "role"),
                         "user") != 0)
            {
              return FALSE;
            }
        }
      for (guint j = 0; j < json_array_get_length (messages); j++)
        {
          JsonNode *msg_node = json_array_get_element (messages, j);
          if (!JSON_NODE_HOLDS_OBJECT (msg_node))
            return FALSE;
          JsonObject *msg = json_node_get_object (msg_node);
          if (!string_member_valid (msg, "role") ||
              !string_member_valid (msg, "content") ||
              !normalize_image_member (msg, "images", "image") ||
              (json_object_has_member (msg, "reasoning") &&
               !string_member_valid (msg, "reasoning")) ||
              (json_object_has_member (msg, "duration") &&
               !number_member_valid (msg, "duration", FALSE)))
            {
              return FALSE;
            }
          const gchar *role = json_object_get_string_member (msg, "role");
          if (!g_str_equal (role, "user") && !g_str_equal (role, "assistant"))
            return FALSE;
          if (g_str_equal (role, "assistant") &&
              json_object_has_member (msg, "images"))
            {
              return FALSE;
            }
        }
    }
  const gchar *current = json_object_get_string_member (root, "current");
  return json_array_get_length (array) ? g_hash_table_contains (ids, current)
                                       : !*current;
}

static void
collect_image_array (GHashTable *refs,
                     JsonArray  *images)
{
  if (images == NULL)
    return;
  for (guint i = 0; i < json_array_get_length (images); i++)
    g_hash_table_add (refs, g_strdup (json_array_get_string_element (images,
                                                                     i)));
}

static gboolean
collect_image_refs (const gchar *path,
                    GHashTable  *refs,
                    gboolean     optional)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GError) error = NULL;
  if (!json_parser_load_from_file (parser, path, &error))
    return optional && g_error_matches (error, G_FILE_ERROR,
                                        G_FILE_ERROR_NOENT);
  JsonNode *root = json_parser_get_root (parser);
  if (!normalize_session (root))
    return FALSE;
  JsonArray *chats = json_object_get_array_member (json_node_get_object (root),
                                                   "chats");
  for (guint i = 0; i < json_array_get_length (chats); i++)
    {
      JsonObject *chat = json_array_get_object_element (chats, i);
      collect_image_array (refs, image_array_member (chat, "draft_images"));
      if (json_object_has_member (chat, "edit"))
        {
          JsonObject *edit = json_object_get_object_member (chat, "edit");
          collect_image_array (refs, image_array_member (edit, "images"));
        }
      JsonArray *messages = json_object_get_array_member (chat, "messages");
      for (guint j = 0; j < json_array_get_length (messages); j++)
        {
          JsonObject *msg = json_array_get_object_element (messages, j);
          collect_image_array (refs, image_array_member (msg, "images"));
        }
    }
  return TRUE;
}

void
store_cleanup_images (const gchar *path)
{
  g_autoptr (GHashTable) refs =
      g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autofree gchar *bak = g_strconcat (path, ".bak", NULL);
  if (!collect_image_refs (path, refs, FALSE) ||
      !collect_image_refs (bak, refs, TRUE))
    return;
  g_autofree gchar *dir = g_path_get_dirname (path);
  g_autofree gchar *images = g_build_filename (dir, "images", NULL);
  g_autoptr (GDir) folder = g_dir_open (images, 0, NULL);
  if (folder == NULL)
    return;
  const gchar *name;
  while ((name = g_dir_read_name (folder)) != NULL)
    if (g_uuid_string_is_valid (name) && !g_hash_table_contains (refs, name))
      {
        g_autofree gchar *file = g_build_filename (images, name, NULL);
        if (g_unlink (file) != 0)
          g_warning ("cannot remove unused image %s: %s", file,
                     g_strerror (errno));
      }
}

static gboolean
load_session (JsonParser  *parser,
              const gchar *path,
              GError     **error)
{
  if (!json_parser_load_from_file (parser, path, error))
    return FALSE;
  if (normalize_session (json_parser_get_root (parser)))
    return TRUE;
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
               _ ("Invalid conversation file structure: %s"), path);
  return FALSE;
}

GPtrArray *
store_load (const gchar *path,
            gchar      **current_id,
            gchar      **model,
            gchar      **effort,
            GError     **error)
{
  *current_id = NULL;
  *model = NULL;
  *effort = NULL;
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autofree gchar *bak = g_strconcat (path, ".bak", NULL);
  g_autoptr (GError) primary_error = NULL;
  g_autoptr (GError) backup_error = NULL;
  gboolean loaded = load_session (parser, path, &primary_error);
  if (!loaded && load_session (parser, bak, &backup_error))
    {
      /* A restore does not rotate the backup, so a broken main file cannot
         overwrite a valid one. */
      g_autoptr (JsonGenerator) gen = json_generator_new ();
      json_generator_set_root (gen, json_parser_get_root (parser));
      gsize len;
      g_autofree gchar *bytes = json_generator_to_data (gen, &len);
      loaded = g_file_set_contents_full (path, bytes, (gssize) len,
                                         G_FILE_SET_CONTENTS_CONSISTENT, 0600,
                                         error);
      if (loaded)
        g_message ("session.json restored from backup");
    }
  else if (!loaded &&
           !(g_error_matches (primary_error, G_FILE_ERROR,
                              G_FILE_ERROR_NOENT) &&
             g_error_matches (backup_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)))
    {
      g_set_error (
          error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
          _ ("Neither the conversation file nor its backup is usable: %s; %s"),
          primary_error->message, backup_error->message);
    }

  g_autoptr (GPtrArray) chats = NULL;
  if (loaded ||
      (g_error_matches (primary_error, G_FILE_ERROR, G_FILE_ERROR_NOENT) &&
       g_error_matches (backup_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)))
    {
      chats = g_ptr_array_new_with_free_func ((GDestroyNotify) chat_free);
    }
  if (loaded)
    {
      JsonObject *root = json_node_get_object (json_parser_get_root (parser));
      *current_id = g_strdup (json_object_get_string_member (root, "current"));
      *model = g_strdup (json_object_get_string_member (root, "model"));
      *effort = g_strdup (json_object_get_string_member (root, "effort"));
      JsonArray *array = json_object_get_array_member (root, "chats");
      for (guint i = 0; i < json_array_get_length (array); i++)
        {
          JsonObject *obj = json_array_get_object_element (array, i);
          g_autoptr (Chat) chat = chat_new (
              json_object_get_string_member (obj, "id"),
              json_object_get_string_member (obj, "title"));
          g_free (chat->draft);
          chat->draft = g_strdup (json_object_get_string_member (obj, "draft"));
          JsonArray *images = image_array_member (obj, "draft_images");
          if (images != NULL)
            chat->draft_images = json_array_ref (images);
          if (json_object_has_member (obj, "edit"))
            {
              JsonObject *edit = json_object_get_object_member (obj, "edit");
              chat->edit_from =
                  (guint) json_object_get_int_member (edit, "index");
              g_free (chat->edit_text);
              chat->edit_text =
                  g_strdup (json_object_get_string_member (edit, "text"));
              images = image_array_member (edit, "images");
              if (images != NULL)
                chat->edit_images = json_array_ref (images);
            }
          chat->created = json_object_get_int_member (obj, "created");
          chat->pinned = json_object_get_boolean_member_with_default (
              obj, "pinned", FALSE);
          json_array_unref (chat->messages);
          chat->messages =
              json_array_ref (json_object_get_array_member (obj, "messages"));
          g_ptr_array_add (chats, g_steal_pointer (&chat));
        }
    }
  return g_steal_pointer (&chats);
}

gboolean
store_save (const gchar *path,
            GPtrArray   *chats,
            const gchar *current_id,
            const gchar *model,
            const gchar *effort)
{
  g_autoptr (JsonArray) array = json_array_new ();
  for (guint i = 0; i < chats->len; i++)
    {
      Chat *chat = g_ptr_array_index (chats, i);
      g_autoptr (JsonObject) obj = json_object_new ();
      json_object_set_string_member (obj, "id", chat->id);
      json_object_set_string_member (obj, "title", chat->title);
      json_object_set_string_member (obj, "draft", chat->draft);
      if (chat->draft_images != NULL)
        json_object_set_array_member (obj, "draft_images",
                                      json_array_ref (chat->draft_images));
      if (chat->edit_from != G_MAXUINT)
        {
          g_autoptr (JsonObject) edit = json_object_new ();
          json_object_set_int_member (edit, "index", chat->edit_from);
          json_object_set_string_member (edit, "text", chat->edit_text);
          if (chat->edit_images != NULL)
            json_object_set_array_member (edit, "images",
                                          json_array_ref (chat->edit_images));
          json_object_set_object_member (obj, "edit", g_steal_pointer (&edit));
        }
      json_object_set_int_member (obj, "created", chat->created);
      json_object_set_boolean_member (obj, "pinned", chat->pinned);
      json_object_set_array_member (obj, "messages",
                                    json_array_ref (chat->messages));
      json_array_add_object_element (array, g_steal_pointer (&obj));
    }

  g_autoptr (JsonObject) root = json_object_new ();
  json_object_set_array_member (root, "chats", g_steal_pointer (&array));
  json_object_set_string_member (root, "current",
                                 current_id != NULL ? current_id : "");
  json_object_set_string_member (root, "model", model);
  json_object_set_string_member (root, "effort", effort);
  return write_json (path, g_steal_pointer (&root), TRUE);
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
  return write_json (path, g_steal_pointer (&root), FALSE);
}
