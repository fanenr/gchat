/* SPDX-FileCopyrightText: 2026 Arthur <arthur63@163.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "i18n.h"
#include "store.h"

#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct
{
  gchar     *dir, *path, *bak;
  GPtrArray *chats;
} Fixture;

typedef enum
{
  INVALID_FILE,
  INVALID_ROOT,
  INVALID_CHAT,
  INVALID_MESSAGE,
  INVALID_EDIT,
  DUPLICATE_CHAT,
} InvalidTarget;

typedef struct
{
  InvalidTarget target;
  const gchar  *member, *json;
} InvalidCase;

static void
setup (Fixture      *f,
       gconstpointer data G_GNUC_UNUSED)
{
  GError *error = NULL;
  f->dir = g_dir_make_tmp ("gchat-store-test-XXXXXX", &error);
  g_assert_no_error (error);
  f->path = g_build_filename (f->dir, "session.json", NULL);
  f->bak = g_strconcat (f->path, ".bak", NULL);
  f->chats = g_ptr_array_new_with_free_func ((GDestroyNotify) chat_free);
  Chat *chat = chat_new ("42", "保留历史");
  JsonObject *msg = json_object_new ();
  json_object_set_string_member (msg, "role", "assistant");
  json_object_set_string_member (msg, "content", "");
  json_object_set_string_member (msg, "reasoning", "已收到的推理");
  json_object_set_double_member (msg, "duration", 1.5);
  json_array_add_object_element (chat->messages, msg);
  g_ptr_array_add (f->chats, chat);
}

static void
teardown (Fixture      *f,
          gconstpointer data G_GNUC_UNUSED)
{
  g_unlink (f->path);
  g_unlink (f->bak);
  gchar *tmp = g_strconcat (f->path, ".tmp", NULL);
  g_unlink (tmp);
  g_free (tmp);
  g_rmdir (f->dir);
  g_free (f->dir);
  g_free (f->path);
  g_free (f->bak);
  g_ptr_array_unref (f->chats);
}

static void
assert_history (GPtrArray *chats)
{
  g_assert_nonnull (chats);
  g_assert_cmpuint (chats->len, ==, 1);
  Chat *chat = g_ptr_array_index (chats, 0);
  g_assert_cmpstr (chat->title, ==, "保留历史");
  JsonObject *msg = json_array_get_object_element (chat->messages, 0);
  g_assert_cmpstr (json_object_get_string_member (msg, "reasoning"), ==,
                   "已收到的推理");
}

static void
recovery (Fixture      *f,
          gconstpointer data)
{
  const InvalidCase *test = data;
  if (test != NULL && test->target == INVALID_EDIT)
    {
      Chat *chat = g_ptr_array_index (f->chats, 0);
      json_object_set_string_member (
          json_array_get_object_element (chat->messages, 0), "role", "user");
      chat->edit_from = 0;
      g_free (chat->edit_text);
      chat->edit_text = g_strdup ("draft");
    }
  g_assert_true (
      store_save (f->path, f->chats, "42", "deepseek-flash", "high"));
  g_assert_true (
      store_save (f->path, f->chats, "42", "deepseek-flash", "high"));
  gchar *backup_before = NULL, *backup_after = NULL;
  g_assert_true (g_file_get_contents (f->bak, &backup_before, NULL, NULL));
  if (test == NULL)
    {
      g_assert_cmpint (g_unlink (f->path), ==, 0);
    }
  else if (test->target == INVALID_FILE)
    {
      g_assert_true (g_file_set_contents (f->path, test->json, -1, NULL));
    }
  else
    {
      JsonParser *parser = json_parser_new ();
      g_assert_true (json_parser_load_from_file (parser, f->path, NULL));
      JsonObject *root = json_node_get_object (json_parser_get_root (parser));
      JsonArray *chats = json_object_get_array_member (root, "chats");
      JsonObject *chat = json_array_get_object_element (chats, 0);
      JsonObject *target = root;
      if (test->target == INVALID_CHAT)
        {
          target = chat;
        }
      else if (test->target == INVALID_MESSAGE)
        {
          target = json_array_get_object_element (
              json_object_get_array_member (chat, "messages"), 0);
        }
      else if (test->target == INVALID_EDIT)
        {
          target = json_object_get_object_member (chat, "edit");
        }
      if (test->target == DUPLICATE_CHAT)
        {
          json_array_add_element (
              chats, json_node_copy (json_array_get_element (chats, 0)));
        }
      else if (test->json != NULL)
        {
          JsonNode *value = json_from_string (test->json, NULL);
          g_assert_nonnull (value);
          json_object_set_member (target, test->member, value);
        }
      else
        {
          json_object_remove_member (target, test->member);
        }
      JsonGenerator *gen = json_generator_new ();
      json_generator_set_root (gen, json_parser_get_root (parser));
      g_assert_true (json_generator_to_file (gen, f->path, NULL));
      g_object_unref (gen);
      g_object_unref (parser);
    }

  gchar *current = NULL, *model = NULL, *effort = NULL;
  GError *error = NULL;
  GPtrArray *loaded = store_load (f->path, &current, &model, &effort, &error);
  g_assert_no_error (error);
  assert_history (loaded);
  g_assert_cmpstr (current, ==, "42");
  g_assert_cmpstr (model, ==, "deepseek-flash");
  g_assert_cmpstr (effort, ==, "high");
  g_assert_true (g_file_get_contents (f->bak, &backup_after, NULL, NULL));
  g_assert_cmpstr (backup_after, ==, backup_before);
  g_free (backup_before);
  g_free (backup_after);

  /* The restored main file can be re-read at once, and a second save still
     keeps a valid backup. */
  g_ptr_array_unref (loaded);
  g_free (current);
  g_free (model);
  g_free (effort);
  loaded = store_load (f->path, &current, &model, &effort, &error);
  g_assert_no_error (error);
  assert_history (loaded);
  g_assert_cmpstr (current, ==, "42");
  g_assert_cmpstr (model, ==, "deepseek-flash");
  g_assert_cmpstr (effort, ==, "high");
  g_assert_true (store_save (f->path, loaded, current, model, effort));
  g_ptr_array_unref (loaded);
  g_free (current);
  g_free (model);
  g_free (effort);
  loaded = store_load (f->bak, &current, &model, &effort, &error);
  g_assert_no_error (error);
  assert_history (loaded);
  g_ptr_array_unref (loaded);
  g_free (current);
  g_free (model);
  g_free (effort);

  struct stat st;
  g_assert_cmpint (g_stat (f->path, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);
}

static void
legacy_pinned (Fixture      *f,
               gconstpointer data G_GNUC_UNUSED)
{
  const gchar *json = "{\"current\":\"42\",\"model\":\"m\",\"effort\":\"e\","
                      "\"chats\":[{\"id\":\"42\",\"title\":\"t\",\"draft\":\"\","
                      "\"created\":0,\"messages\":[]}]}";
  g_assert_true (g_file_set_contents (f->path, json, -1, NULL));
  gchar *current = NULL, *model = NULL, *effort = NULL;
  GError *error = NULL;
  GPtrArray *loaded = store_load (f->path, &current, &model, &effort, &error);
  g_assert_no_error (error);
  g_assert_false (((Chat *) g_ptr_array_index (loaded, 0))->pinned);
  g_ptr_array_unref (loaded);
  g_free (current);
  g_free (model);
  g_free (effort);
}

static void
unrecoverable (Fixture      *f,
               gconstpointer data G_GNUC_UNUSED)
{
  g_assert_true (g_file_set_contents (f->path, "{}", -1, NULL));
  g_assert_true (g_file_set_contents (f->bak, "{\"chats\":[42]}", -1, NULL));
  gchar *current = NULL, *model = NULL, *effort = NULL;
  GError *error = NULL;
  GPtrArray *loaded = store_load (f->path, &current, &model, &effort, &error);
  g_assert_null (loaded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);
  g_assert_null (current);
  g_assert_null (model);
  g_assert_null (effort);
  gchar *primary = NULL, *backup = NULL;
  g_assert_true (g_file_get_contents (f->path, &primary, NULL, NULL));
  g_assert_true (g_file_get_contents (f->bak, &backup, NULL, NULL));
  g_assert_cmpstr (primary, ==, "{}");
  g_assert_cmpstr (backup, ==, "{\"chats\":[42]}");
  g_free (primary);
  g_free (backup);
}

static void
missing (Fixture      *f,
         gconstpointer data G_GNUC_UNUSED)
{
  gchar *current = NULL, *model = NULL, *effort = NULL;
  GError *error = NULL;
  GPtrArray *loaded = store_load (f->path, &current, &model, &effort, &error);
  g_assert_no_error (error);
  g_assert_nonnull (loaded);
  g_assert_cmpuint (loaded->len, ==, 0);
  g_ptr_array_unref (loaded);
  g_assert_false (g_file_test (f->path, G_FILE_TEST_EXISTS));
}

static void
empty_title (Fixture      *f,
             gconstpointer data G_GNUC_UNUSED)
{
  g_ptr_array_set_size (f->chats, 0);
  Chat *chat = chat_new_empty ();
  g_assert_cmpstr (chat->title, ==, "");
  g_ptr_array_add (f->chats, chat);
  g_assert_true (
      store_save (f->path, f->chats, chat->id, "deepseek-flash", "high"));
  gchar *current = NULL, *model = NULL, *effort = NULL;
  GError *error = NULL;
  GPtrArray *loaded = store_load (f->path, &current, &model, &effort, &error);
  g_assert_no_error (error);
  g_assert_nonnull (loaded);
  g_assert_cmpuint (loaded->len, ==, 1);
  g_assert_cmpstr (((Chat *) g_ptr_array_index (loaded, 0))->title, ==, "");
  g_ptr_array_unref (loaded);
  g_free (current);
  g_free (model);
  g_free (effort);
}

static void
drafts (Fixture      *f,
        gconstpointer data)
{
  Chat *chat = g_ptr_array_index (f->chats, 0);
  g_free (chat->draft);
  chat->draft = g_strdup ("草稿\n  保留空白 ");
  if (data != NULL)
    {
      JsonObject *msg = json_array_get_object_element (chat->messages, 0);
      json_object_set_string_member (msg, "role", "user");
      chat->edit_from = 0;
      g_free (chat->edit_text);
      chat->edit_text = g_strdup ("编辑草稿");
    }
  g_assert_true (
      store_save (f->path, f->chats, "42", "deepseek-flash", "high"));
  gchar *current = NULL, *model = NULL, *effort = NULL;
  GError *error = NULL;
  GPtrArray *loaded = store_load (f->path, &current, &model, &effort, &error);
  g_assert_no_error (error);
  Chat *restored = g_ptr_array_index (loaded, 0);
  g_assert_cmpstr (restored->draft, ==, chat->draft);
  g_assert_cmpstr (restored->edit_text, ==, chat->edit_text);
  g_assert_cmpuint (restored->edit_from, ==, chat->edit_from);
  g_assert_cmpuint (json_array_get_length (restored->messages), ==, 1);
  g_ptr_array_unref (loaded);
  g_free (current);
  g_free (model);
  g_free (effort);
}

static void
empty_session (Fixture      *f,
               gconstpointer data G_GNUC_UNUSED)
{
  g_ptr_array_set_size (f->chats, 0);
  g_assert_true (
      store_save (f->path, f->chats, NULL, "deepseek-flash", "high"));
  gchar *current = NULL, *model = NULL, *effort = NULL;
  GError *error = NULL;
  GPtrArray *loaded = store_load (f->path, &current, &model, &effort, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (loaded->len, ==, 0);
  g_assert_cmpstr (current, ==, "");
  g_assert_cmpstr (model, ==, "deepseek-flash");
  g_assert_cmpstr (effort, ==, "high");
  g_ptr_array_unref (loaded);
  g_free (current);
  g_free (model);
  g_free (effort);
}

static void
save_failure (Fixture      *f,
              gconstpointer data G_GNUC_UNUSED)
{
  g_assert_true (
      store_save (f->path, f->chats, "42", "deepseek-flash", "high"));
  gchar *before = NULL, *after = NULL;
  g_assert_true (g_file_get_contents (f->path, &before, NULL, NULL));
  g_assert_cmpint (g_mkdir (f->bak, 0700), ==, 0);
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "cannot write *");
  g_assert_false (
      store_save (f->path, f->chats, "42", "deepseek-flash", "high"));
  g_test_assert_expected_messages ();
  g_assert_true (g_file_get_contents (f->path, &after, NULL, NULL));
  g_assert_cmpstr (after, ==, before);
  g_free (before);
  g_free (after);
  gchar *tmp = g_strconcat (f->path, ".tmp", NULL);
  g_assert_false (g_file_test (tmp, G_FILE_TEST_EXISTS));
  g_free (tmp);
  g_assert_cmpint (g_rmdir (f->bak), ==, 0);
}

static void
session_lock (Fixture      *f,
              gconstpointer data G_GNUC_UNUSED)
{
  GError *error = NULL;
  gint first = store_lock (f->path, &error);
  g_assert_no_error (error);
  g_assert_cmpint (first, >=, 0);
  g_assert_cmpint (store_lock (f->path, &error), ==, -1);
  g_assert_nonnull (error);
  g_clear_error (&error);
  close (first);
  gint second = store_lock (f->path, &error);
  g_assert_no_error (error);
  g_assert_cmpint (second, >=, 0);
  close (second);
  gchar *lock = g_strconcat (f->path, ".lock", NULL);
  g_unlink (lock);
  g_free (lock);
}

static void
invalid_config (Fixture      *f G_GNUC_UNUSED,
                gconstpointer data)
{
  gchar *dir = g_build_filename (g_get_user_config_dir (), "gchat", NULL);
  gchar *path = g_build_filename (dir, "config.json", NULL);
  g_assert_cmpint (g_mkdir (dir, 0700), ==, 0);
  g_assert_true (g_file_set_contents (path, data, -1, NULL));
  gchar *value = config_get ("api_key");
  g_assert_null (value);
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "cannot write *");
  g_assert_false (config_set ("theme", "dark"));
  g_test_assert_expected_messages ();
  gchar *primary = NULL;
  g_assert_true (g_file_get_contents (path, &primary, NULL, NULL));
  g_assert_cmpstr (primary, ==, data);
  g_free (primary);
  g_unlink (path);
  g_rmdir (dir);
  g_free (dir);
  g_free (path);
}

static void
config_roundtrip (void)
{
  g_assert_true (config_set ("api_key", "first-key"));
  g_assert_true (config_set ("theme", "dark"));
  g_assert_true (config_set ("api_key", "second-key"));
  gchar *key = config_get ("api_key"), *theme = config_get ("theme");
  g_assert_cmpstr (key, ==, "second-key");
  g_assert_cmpstr (theme, ==, "dark");
  g_free (key);
  g_free (theme);
  gchar *dir = g_build_filename (g_get_user_config_dir (), "gchat", NULL);
  gchar *path = g_build_filename (dir, "config.json", NULL),
        *bak = g_strconcat (path, ".bak", NULL);
  g_assert_false (g_file_test (bak, G_FILE_TEST_EXISTS));
  struct stat st;
  g_assert_cmpint (g_stat (path, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);
  g_unlink (path);
  g_rmdir (dir);
  g_free (bak);
  g_free (path);
  g_free (dir);
}

int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);
  gchar *config_dir = g_dir_make_tmp ("gchat-config-test-XXXXXX", NULL);
  g_assert_nonnull (config_dir);
  g_setenv ("XDG_CONFIG_HOME", config_dir, TRUE);
  i18n_init (NULL);
  static const InvalidCase invalid[] = {
    { INVALID_FILE, NULL, "{ truncated" },
    { INVALID_FILE, NULL, "{}" },
    { INVALID_FILE, NULL, "null" },
    { INVALID_FILE, NULL, "[]" },
    { INVALID_ROOT, "chats", "null" },
    { INVALID_ROOT, "chats", "[42]" },
    { INVALID_ROOT, "current", NULL },
    { INVALID_ROOT, "current", "null" },
    { INVALID_ROOT, "current", "\"missing-chat\"" },
    { INVALID_ROOT, "model", NULL },
    { INVALID_ROOT, "model", "null" },
    { INVALID_ROOT, "effort", NULL },
    { INVALID_CHAT, "id", NULL },
    { INVALID_CHAT, "id", "\"\"" },
    { INVALID_CHAT, "title", NULL },
    { INVALID_CHAT, "title", "null" },
    { INVALID_CHAT, "draft", NULL },
    { INVALID_CHAT, "draft", "null" },
    { INVALID_CHAT, "created", NULL },
    { INVALID_CHAT, "created", "\"bad\"" },
    { INVALID_CHAT, "created", "-1" },
    { INVALID_CHAT, "created", "0.5" },
    { INVALID_CHAT, "created", "1e30" },
    { INVALID_CHAT, "pinned", "null" },
    { INVALID_CHAT, "pinned", "1" },
    { INVALID_CHAT, "messages", "null" },
    { INVALID_CHAT, "messages", "[null]" },
    { INVALID_CHAT, "edit", "null" },
    { INVALID_CHAT, "edit", "{}" },
    { INVALID_CHAT, "edit", "{\"index\":0,\"text\":\"draft\"}" },
    { INVALID_MESSAGE, "role", NULL },
    { INVALID_MESSAGE, "role", "\"\"" },
    { INVALID_MESSAGE, "role", "\"unexpected\"" },
    { INVALID_MESSAGE, "role", "\"system\"" },
    { INVALID_MESSAGE, "content", NULL },
    { INVALID_MESSAGE, "content", "null" },
    { INVALID_MESSAGE, "reasoning", "null" },
    { INVALID_MESSAGE, "duration", "-1" },
    { INVALID_MESSAGE, "duration", "\"bad\"" },
    { INVALID_EDIT, "index", NULL },
    { INVALID_EDIT, "index", "-1" },
    { INVALID_EDIT, "index", "0.5" },
    { INVALID_EDIT, "index", "1" },
    { INVALID_EDIT, "index", "4294967296" },
    { INVALID_EDIT, "text", NULL },
    { INVALID_EDIT, "text", "null" },
    { DUPLICATE_CHAT, NULL, NULL },
  };
  for (guint i = 0; i < G_N_ELEMENTS (invalid); i++)
    {
      gchar *name = g_strdup_printf ("/store/recovery/%u", i);
      g_test_add (name, Fixture, &invalid[i], setup, recovery, teardown);
      g_free (name);
    }
  g_test_add ("/store/recovery/missing-primary", Fixture, NULL, setup, recovery,
              teardown);
  g_test_add ("/store/legacy-pinned", Fixture, NULL, setup, legacy_pinned,
              teardown);
  g_test_add ("/store/unrecoverable", Fixture, NULL, setup, unrecoverable,
              teardown);
  g_test_add ("/store/save-failure", Fixture, NULL, setup, save_failure,
              teardown);
  g_test_add ("/store/lock", Fixture, NULL, setup, session_lock, teardown);
  g_test_add ("/store/missing", Fixture, NULL, setup, missing, teardown);
  g_test_add ("/store/empty-session", Fixture, NULL, setup, empty_session,
              teardown);
  g_test_add ("/store/empty-title", Fixture, NULL, setup, empty_title,
              teardown);
  g_test_add ("/store/draft", Fixture, NULL, setup, drafts, teardown);
  g_test_add ("/store/edit-draft", Fixture, GINT_TO_POINTER (1), setup, drafts,
              teardown);
  static const gchar *configs[] = { "{ broken", "[]", "null",
                                    "{\"api_key\":42}", "{\"api_key\":null}" };
  for (guint i = 0; i < G_N_ELEMENTS (configs); i++)
    {
      gchar *name = g_strdup_printf ("/config/invalid/%u", i);
      g_test_add (name, Fixture, configs[i], setup, invalid_config, teardown);
      g_free (name);
    }
  g_test_add_func ("/config/roundtrip-no-backup", config_roundtrip);
  int status = g_test_run ();
  g_rmdir (config_dir);
  g_free (config_dir);
  return status;
}
