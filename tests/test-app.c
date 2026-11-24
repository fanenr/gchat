/* SPDX-FileCopyrightText: 2026 Arthur <arthur63@163.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Test internal state transitions directly instead of widening the app
   interface for tests. */
#include "config.h"

#define main gchat_main
#include "../main.c"
#undef main

static gboolean have_display;
static gchar *test_dir;

static void
add_message (const gchar *role,
             const gchar *content)
{
  g_autoptr (JsonObject) msg = json_object_new ();
  json_object_set_string_member (msg, "role", role);
  json_object_set_string_member (msg, "content", content);
  json_array_add_object_element (app->chat->messages,
                                 g_steal_pointer (&msg));
}

static void
setup (void)
{
  app = g_new0 (App, 1);
  g_autofree gchar *path = store_session_path ();
  g_autoptr (GError) error = NULL;
  app->db = store_open (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (app->db);
  app->deleted_chat_ids = g_ptr_array_new_with_free_func (g_free);
  app->answer = g_string_new (NULL);
  app->reasoning = g_string_new (NULL);
  app->model = g_strdup ("deepseek-flash");
  app->effort = g_strdup ("high");
  app->auto_title = load_enabled_preference ("auto_title");
  app->confirm_delete_chat = load_enabled_preference ("confirm_delete_chat");
  app->web_enabled = load_disabled_preference ("web_enabled");
  app->latest_reminder = load_disabled_preference ("latest_reminder");
  app->chats = g_ptr_array_new_with_free_func ((GDestroyNotify) chat_free);
  app->chat = chat_new ("42", "测试");
  g_ptr_array_add (app->chats, app->chat);
  add_message ("user", "question one");
  add_message ("assistant", "answer one");
}

static void
teardown (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    {
    }
  cancel_ui_updates ();
  if (app->window != NULL)
    {
      gtk_window_destroy (GTK_WINDOW (app->window));
      g_object_unref (app->window);
    }
  g_clear_object (&app->cancel);
  g_clear_object (&app->image_cancel);
  g_clear_object (&app->session);
  g_ptr_array_unref (app->chats);
  g_ptr_array_unref (app->deleted_chat_ids);
  sqlite3_close (app->db);
  g_string_free (app->answer, TRUE);
  g_string_free (app->reasoning, TRUE);
  g_free (app->model);
  g_free (app->effort);
  g_free (app->api_key);
  g_free (app->kill_text);
  g_free (app);
  app = NULL;
  chat_stick = (Stick){ 0 };
  input_stick = (Stick){ 0 };
  g_autofree gchar *path = store_session_path ();
  g_unlink (path);
}

static gboolean
setup_ui (void)
{
  if (!have_display)
    {
      g_test_skip ("需要 GTK 显示后端");
      return FALSE;
    }
  setup ();
  app->window = adw_application_window_new (NULL);
  g_object_ref (app->window);
  build_shell ();
  return TRUE;
}

static void
begin_reply (gboolean reasoning)
{
  add_message ("assistant", "");
  app->reply_msg = json_array_get_object_element (app->chat->messages, 2);
  app->reply_bubble = bubble_new ("assistant");
  app->reply_body = g_object_get_data (G_OBJECT (app->reply_bubble), "body");
  app->reply_label = gtk_label_new ("…");
  gtk_box_append (GTK_BOX (app->reply_body), app->reply_label);
  app->cancel = g_cancellable_new ();
  app->request_started = g_get_monotonic_time () - G_USEC_PER_SEC;
  set_busy (TRUE);
  if (reasoning)
    {
      g_string_append (app->reasoning, "已收到的推理");
      app->reason_box = thought_widget (app->reasoning->str, 0,
                                        &app->reason_view, &app->reason_title);
      app->reasoning_shown = app->reasoning->len;
      gtk_box_prepend (GTK_BOX (app->reply_body), app->reason_box);
    }
}

static void
sse_parser (void)
{
  g_assert_null (sse_data (": keep-alive"));
  g_assert_null (sse_data ("datax: ignored"));
  g_assert_cmpstr (sse_data ("data"), ==, "");
  g_assert_cmpstr (sse_data ("data:"), ==, "");
  g_assert_cmpstr (sse_data ("data:  value"), ==, " value");

  static const gchar *invalid[] = {
    "[DONE]",
    "not json",
    "null",
    "[]",
    "{}",
    "{\"choices\":null}",
    "{\"choices\":{}}",
    "{\"choices\":[]}",
    "{\"choices\":[null]}",
  };
  for (guint i = 0; i < G_N_ELEMENTS (invalid); i++)
    g_assert_null (sse_parse_choice (invalid[i]));

  g_autoptr (JsonObject) choice = sse_parse_choice (
      "{\"choices\":\n[{\"delta\":{\"reasoning_content\":\"think\","
      "\"content\":\"hi\"}}]}");
  g_assert_nonnull (choice);
  JsonObject *delta = member_object (choice, "delta");
  g_assert_cmpstr (member_string (delta, "content"), ==, "hi");
  g_assert_cmpstr (member_string (delta, "reasoning_content"), ==, "think");
  g_assert_null (member_string (delta, "role"));

  g_clear_pointer (&choice, json_object_unref);
  choice = sse_parse_choice ("{\"choices\":[{\"delta\":42}]}");
  g_assert_nonnull (choice);
  g_assert_null (member_object (choice, "delta"));
}

static void
sse_stream (gconstpointer data)
{
  if (!setup_ui ())
    return;
  begin_reply (FALSE);

  const gchar *events = GPOINTER_TO_INT (data) == 0
      ? ": keep-alive\r\nevent: message\r\n"
        "data: {\"choices\":\r\ndata\r\n"
        "data:[{\"delta\":{\"content\":\"你好\"}}]}\r\n\r\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"ignored\"}}]}\r\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"ignored\"}}]}\r\n\r\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"!\"}}]}\r\n\r\n"
        "data: [DONE]\r\n\r\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"late\"}}]}\r\n\r\n"
      : "data: {\"choices\":[{\"delta\":{\"content\":\"kept\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"unfinished\"}}]}\n";
  g_autoptr (GInputStream) input =
      g_memory_input_stream_new_from_data (events, -1, NULL);
  g_autoptr (GDataInputStream) lines = g_data_input_stream_new (input);
  g_data_input_stream_set_newline_type (lines, G_DATA_STREAM_NEWLINE_TYPE_ANY);
  g_data_input_stream_read_line_async (g_object_ref (lines), G_PRIORITY_DEFAULT,
                                       app->cancel, stream_line_cb,
                                       g_object_ref (app->cancel));
  if (GPOINTER_TO_INT (data) == 0)
    {
      GString *event = NULL;
      while (event == NULL)
        {
          g_main_context_iteration (NULL, TRUE);
          event = g_object_get_data (G_OBJECT (lines), "sse-data");
        }
      g_assert_cmpstr (event->str, ==, "{\"choices\":\n");
      g_assert_cmpuint (app->answer->len, ==, 0);
    }
  while (app->busy)
    g_main_context_iteration (NULL, TRUE);

  JsonObject *reply = json_array_get_object_element (app->chat->messages, 2);
  g_assert_cmpstr (json_object_get_string_member (reply, "content"), ==,
                   GPOINTER_TO_INT (data) == 0 ? "你好!" : "kept");
  teardown ();
}

static void
sse_cancel_pending (void)
{
  if (!setup_ui ())
    return;
  begin_reply (FALSE);

  const gchar *events =
      "data: {\"choices\":[{\"delta\":{\"content\":\"pending\"}}]}\n";
  g_autoptr (GInputStream) input =
      g_memory_input_stream_new_from_data (events, -1, NULL);
  g_autoptr (GDataInputStream) lines = g_data_input_stream_new (input);
  g_data_input_stream_read_line_async (g_object_ref (lines), G_PRIORITY_DEFAULT,
                                       app->cancel, stream_line_cb,
                                       g_object_ref (app->cancel));
  while (g_object_get_data (G_OBJECT (lines), "sse-data") == NULL)
    g_main_context_iteration (NULL, TRUE);
  g_cancellable_cancel (app->cancel);
  while (app->busy)
    g_main_context_iteration (NULL, TRUE);
  g_assert_cmpuint (json_array_get_length (app->chat->messages), ==, 2);

  GDataInputStream *weak_lines = lines;
  g_object_add_weak_pointer (G_OBJECT (lines), (gpointer *) &weak_lines);
  g_clear_object (&lines);
  g_assert_null (weak_lines);
  teardown ();
}

static void
stream_frame_batch (void)
{
  if (!setup_ui ())
    return;
  render_chat ();
  begin_reply (FALSE);

  process_sse_event ("{\"choices\":[{\"delta\":{"
                     "\"reasoning_content\":\"甲\"}}]}");
  guint tick = app->stream_tick;
  g_assert_cmpuint (tick, !=, 0);
  process_sse_event ("{\"choices\":[{\"delta\":{"
                     "\"reasoning_content\":\"乙\"}}]}");
  g_assert_cmpuint (app->stream_tick, ==, tick);
  GtkTextBuffer *reason = gtk_text_view_get_buffer (
      GTK_TEXT_VIEW (app->reason_view));
  g_assert_cmpint (gtk_text_buffer_get_char_count (reason), ==, 0);
  gtk_widget_remove_tick_callback (app->window, app->stream_tick);
  update_stream_frame (app->window, NULL, NULL);
  g_assert_cmpuint (app->stream_tick, ==, 0);
  g_assert_cmpint (gtk_text_buffer_get_char_count (reason), ==, 2);

  process_sse_event ("{\"choices\":[{\"delta\":{\"content\":\"one\"}}]}");
  tick = app->stream_tick;
  g_assert_cmpuint (tick, !=, 0);
  process_sse_event ("{\"choices\":[{\"delta\":{\"content\":\" two\"}}]}");
  g_assert_cmpuint (app->stream_tick, ==, tick);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (app->reply_label)), ==,
                   _ ("Thinking…"));
  gtk_widget_remove_tick_callback (app->window, app->stream_tick);
  update_stream_frame (app->window, NULL, NULL);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (app->reply_label)), ==,
                   "one two▍");

  /* An end event can arrive before the frame that would display the tail. */
  process_sse_event ("{\"choices\":[{\"delta\":{\"reasoning_content\":\"丙\","
                     "\"content\":\" three\"}}]}");
  g_assert_cmpuint (app->stream_tick, !=, 0);
  end_stream ();
  g_assert_cmpuint (app->stream_tick, ==, 0);
  JsonObject *reply = json_array_get_object_element (app->chat->messages, 2);
  g_assert_cmpstr (member_string (reply, "content"), ==, "one two three");
  g_assert_cmpstr (member_string (reply, "reasoning"), ==, "甲乙丙");
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds (reason, &start, &end);
  g_autofree gchar *shown = gtk_text_buffer_get_text (reason, &start, &end,
                                                      FALSE);
  g_assert_cmpstr (shown, ==, "甲乙丙");
  teardown ();
}

static void
assert_reasoning_saved (void)
{
  g_assert_cmpuint (json_array_get_length (app->chat->messages), ==, 3);
  JsonObject *msg = json_array_get_object_element (app->chat->messages, 2);
  g_assert_cmpstr (json_object_get_string_member (msg, "content"), ==, "");
  g_assert_cmpstr (json_object_get_string_member (msg, "reasoning"), ==,
                   "已收到的推理");
  g_assert_cmpfloat (json_object_get_double_member (msg, "duration"), >=, 1);
  g_assert_false (app->busy);

  g_autofree gchar *current = NULL;
  g_autofree gchar *model = NULL;
  g_autofree gchar *effort = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) chats =
      store_load (app->db, &current, &model, &effort, &error);
  g_assert_no_error (error);
  Chat *chat = g_ptr_array_index (chats, 0);
  msg = json_array_get_object_element (chat->messages, 2);
  g_assert_cmpstr (json_object_get_string_member (msg, "reasoning"), ==,
                   "已收到的推理");
}

static void
reasoning_lifecycle (void)
{
  if (!setup_ui ())
    return;
  begin_reply (FALSE);

  const gchar *events =
      "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"first\"}}]}\n\n"
      "data: {\"choices\":[{\"delta\":{\"content\":\"answer\"}}]}\n\n"
      "data: [DONE]\n\n";
  g_autoptr (GInputStream) input =
      g_memory_input_stream_new_from_data (events, -1, NULL);
  g_autoptr (GDataInputStream) lines = g_data_input_stream_new (input);
  g_data_input_stream_read_line_async (lines, G_PRIORITY_DEFAULT, app->cancel,
                                       stream_line_cb,
                                       g_object_ref (app->cancel));
  (void) g_steal_pointer (&lines);
  while (app->reason_box == NULL)
    g_main_context_iteration (NULL, TRUE);
  GtkWidget *content = gtk_widget_get_last_child (app->reason_box);
  g_assert_true (gtk_widget_get_visible (content));
  while (app->busy)
    g_main_context_iteration (NULL, TRUE);
  g_assert_false (gtk_widget_get_visible (content));
  teardown ();

  if (!setup_ui ())
    return;
  begin_reply (TRUE);
  fail_stream ("模拟断网");
  assert_reasoning_saved ();
  teardown ();

  if (!setup_ui ())
    return;
  begin_reply (TRUE);
  on_close (NULL, NULL);
  assert_reasoning_saved ();
  teardown ();
}

static void
assert_web_citations (GtkWidget *body)
{
  GtkWidget *label = gtk_widget_get_last_child (body);
  g_assert_true (GTK_IS_LABEL (label));
  g_autofree gchar *expected = g_strdup_printf (
      "%s\nA < B & C\nhttp://example.org/", _ ("Sources"));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (label)), ==, expected);
  g_assert_nonnull (strstr (gtk_label_get_label (GTK_LABEL (label)),
                            "href=\"https://example.com/?a=1&amp;b=2\""));
}

static void
web_stream (void)
{
  if (!setup_ui ())
    return;
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (app->web_toggle), TRUE);
  begin_reply (FALSE);
  g_assert_false (gtk_widget_get_sensitive (app->web_toggle));
  GtkWidget *body_widget = app->reply_body;
  const gchar *events =
      "data: {\"choices\":[{\"server_tool_event\":{\"type\":\"call_started\","
      "\"call\":{\"type\":\"deepseek_web_search\"}}}]}\n\n"
      "data: {\"choices\":[{\"server_tool_event\":{\"type\":\"call_started\","
      "\"call\":{\"type\":\"deepseek_read_url\"}}}]}\n\n"
      "data: {\"choices\":[{\"server_tool_event\":{\"type\":\"call_completed\","
      "\"call\":{\"type\":\"deepseek_read_url\","
      "\"status\":\"completed\"}}}]}\n\n"
      "data: {\"choices\":[{\"server_tool_event\":{\"type\":\"call_completed\","
      "\"call\":{\"type\":\"deepseek_web_search\","
      "\"status\":\"completed\"}}}]}\n\n"
      "data: {\"choices\":[{\"delta\":{\"content\":\"Answer\","
      "\"server_tool_calls\":[{\"ignored\":true}]}}]}\n\n"
      "data: {\"choices\":[{\"finish_reason\":\"stop\",\"delta\":{"
      "\"server_tool_calls\":[{\"index\":0,\"reasoning_content\":\"\","
      "\"calls\":[{\"index\":0,\"id\":\"srvtoolu_rl_0\",\"kind\":\"read_link\","
      "\"type\":\"deepseek_read_url\",\"status\":\"completed\","
      "\"action\":{\"type\":\"read_link\",\"url\":\"https://example.com/\"},"
      "\"signature\":\"test-signature\"}]}],\n"
      "data: \"annotations\":[null,{\"type\":\"url_citation\","
      "\"url_citation\":{\"title\":\"A < B & C\","
      "\"url\":\"https://example.com/?a=1&b=2\"}},"
      "{\"type\":\"url_citation\",\"url_citation\":{\"title\":\"Duplicate\","
      "\"url\":\"https://example.com/?a=1&b=2\"}},"
      "{\"type\":\"url_citation\",\"url_citation\":{"
      "\"url\":\"file:///tmp/a\"}},"
      "{\"type\":\"url_citation\",\"url_citation\":{"
      "\"url\":\"http://example.org/\"}}"
      "]}}]}\n\n"
      "data: [DONE]\n\n";
  g_autoptr (GInputStream) input =
      g_memory_input_stream_new_from_data (events, -1, NULL);
  GDataInputStream *lines = g_data_input_stream_new (input);
  /* The stream callback takes ownership of this reference. */
  g_data_input_stream_read_line_async (lines, G_PRIORITY_DEFAULT, app->cancel,
                                       stream_line_cb,
                                       g_object_ref (app->cancel));

  while (app->busy && app->web_searches == 0)
    g_main_context_iteration (NULL, TRUE);
  g_assert_true (app->busy);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (app->reply_label)), ==,
                   _ ("Searching the web…"));
  while (app->busy && app->web_reads == 0)
    g_main_context_iteration (NULL, TRUE);
  g_assert_true (app->busy);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (app->reply_label)), ==,
                   _ ("Reading links…"));
  while (app->busy && app->web_reads > 0)
    g_main_context_iteration (NULL, TRUE);
  g_assert_true (app->busy);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (app->reply_label)), ==,
                   _ ("Searching the web…"));
  while (app->busy && app->answer->len == 0)
    g_main_context_iteration (NULL, TRUE);
  g_assert_true (app->busy);
  g_assert_false (json_object_has_member (app->reply_msg, "server_tool_calls"));
  while (app->busy)
    g_main_context_iteration (NULL, TRUE);
  g_assert_cmpuint (app->web_searches, ==, 0);
  g_assert_cmpuint (app->web_reads, ==, 0);
  assert_web_citations (body_widget);

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (app->web_toggle), FALSE);
  add_message ("user", "What did the link say?");
  gsize len;
  g_autoptr (GError) error = NULL;
  g_autofree gchar *body = request_body (4, NULL, FALSE, &len, &error);
  g_assert_no_error (error);
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_assert_true (json_parser_load_from_data (parser, body, len, NULL));
  JsonObject *root = json_node_get_object (json_parser_get_root (parser));
  g_assert_false (json_object_has_member (root, "tools"));
  JsonObject *sent = json_array_get_object_element (
      json_object_get_array_member (root, "messages"), 2);
  g_assert_false (json_object_has_member (sent, "annotations"));
  g_assert_cmpstr (json_object_get_string_member (sent, "content"), ==,
                   "Answer");
  JsonObject *saved = json_array_get_object_element (app->chat->messages, 2);
  JsonNode *replayed = json_object_get_member (sent, "server_tool_calls");
  g_assert_nonnull (replayed);
  g_assert_true (json_node_equal (
      replayed, json_object_get_member (saved, "server_tool_calls")));
  g_assert_nonnull (strstr (body, "test-signature"));
  teardown ();
}

static void
web_stop (void)
{
  if (!setup_ui ())
    return;
  begin_reply (FALSE);
  process_sse_event (
      "{\"choices\":[{\"server_tool_event\":{\"type\":\"call_started\","
      "\"call\":{\"type\":\"deepseek_read_url\"}},"
      "\"delta\":{\"content\":\"partial answer\"}}]}");
  end_stream ();
  JsonObject *msg = json_array_get_object_element (app->chat->messages, 2);
  g_assert_cmpstr (json_object_get_string_member (msg, "content"), ==,
                   "partial answer");
  g_assert_false (json_object_has_member (msg, "annotations"));
  g_assert_false (json_object_has_member (msg, "server_tool_calls"));
  g_assert_cmpuint (app->web_reads, ==, 0);
  teardown ();
}

static void
web_toggle (void)
{
  if (!have_display)
    {
      g_test_skip ("需要 GTK 显示后端");
      return;
    }
  g_assert_true (config_set ("web_enabled", NULL));
  if (!setup_ui ())
    return;
  g_assert_true (gtk_widget_get_visible (app->web_toggle));
  g_assert_false (gtk_toggle_button_get_active (
      GTK_TOGGLE_BUTTON (app->web_toggle)));

  g_autoptr (JsonParser) parser = json_parser_new ();
  for (guint i = 0; i < 3; i++)
    {
      if (i == 1)
        {
          gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (app->web_toggle),
                                        TRUE);
          g_assert_true (app->web_enabled);
          g_autofree gchar *saved = config_get ("web_enabled");
          g_assert_cmpstr (saved, ==, "true");
        }
      if (i == 2)
        {
          on_model_choice (NULL, "deepseek-v4-pro");
          g_assert_false (gtk_widget_get_visible (app->web_toggle));
        }
      gsize len;
      g_autofree gchar *body = request_body (2, NULL, FALSE, &len, NULL);
      g_assert_true (json_parser_load_from_data (parser, body, len, NULL));
      JsonObject *root = json_node_get_object (json_parser_get_root (parser));
      if (i != 1)
        {
          g_assert_false (json_object_has_member (root, "tools"));
          continue;
        }
      JsonArray *tools = member_array (root, "tools");
      g_assert_nonnull (tools);
      g_assert_cmpuint (json_array_get_length (tools), ==, 2);
      JsonObject *search = json_array_get_object_element (tools, 0);
      JsonObject *read = json_array_get_object_element (tools, 1);
      g_assert_cmpstr (member_string (search, "type"), ==,
                       "deepseek_web_search");
      g_assert_cmpstr (member_string (read, "type"), ==, "deepseek_read_url");
    }
  g_assert_true (config_set ("web_enabled", "false"));
  teardown ();
}

static void
title_generation (void)
{
  g_autofree gchar *short_line = short_title ("  第一行标题\n第二行  ");
  g_assert_cmpstr (short_line, ==, "第一行标题");
  g_assert_null (short_title ("   \n\t  "));

  if (!setup_ui ())
    return;
  gsize len;
  g_autoptr (GError) error = NULL;
  g_autofree gchar *with_title =
      request_body (1, NULL, TRUE, &len, &error);
  g_assert_no_error (error);
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_assert_true (json_parser_load_from_data (parser, with_title, len, NULL));
  JsonObject *root = json_node_get_object (json_parser_get_root (parser));
  JsonObject *title_obj = member_object (root, "title");
  g_assert_nonnull (title_obj);
  g_assert_true (json_object_get_boolean_member (title_obj, "enabled"));

  g_autofree gchar *without_title =
      request_body (1, NULL, FALSE, &len, &error);
  g_assert_no_error (error);
  g_assert_true (json_parser_load_from_data (parser, without_title, len, NULL));
  root = json_node_get_object (json_parser_get_root (parser));
  g_assert_false (json_object_has_member (root, "title"));

  begin_reply (FALSE);
  app->request_title = TRUE;
  const gchar *events =
      "data: {\"choices\":[{\"delta\":{\"content\":\"Answer\"}}]}\n\n"
      "data: {\"choices\":[{\"finish_reason\":\"stop\","
      "\"delta\":{\"title\":\"  自动标题\\n忽略次行  \"}}]}\n\n"
      "data: [DONE]\n\n";
  g_autoptr (GInputStream) input =
      g_memory_input_stream_new_from_data (events, -1, NULL);
  GDataInputStream *lines = g_data_input_stream_new (input);
  g_data_input_stream_read_line_async (lines, G_PRIORITY_DEFAULT, app->cancel,
                                       stream_line_cb,
                                       g_object_ref (app->cancel));
  while (app->busy)
    g_main_context_iteration (NULL, TRUE);
  g_assert_cmpstr (app->chat->title, ==, "自动标题");
  teardown ();
}

static void
payload_filters_reasoning (void)
{
  setup ();
  add_message ("assistant", "");
  JsonObject *reason = json_array_get_object_element (app->chat->messages, 2);
  json_object_set_string_member (reason, "reasoning", "仅推理");
  add_message ("user", "next");
  gsize len;
  g_autofree gchar *body = request_body (4, NULL, FALSE, &len, NULL);
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_assert_true (json_parser_load_from_data (parser, body, len, NULL));
  JsonObject *root = json_node_get_object (json_parser_get_root (parser));
  JsonArray *messages = json_object_get_array_member (root, "messages");
  g_assert_cmpuint (json_array_get_length (messages), ==, 3);
  g_assert_cmpstr (json_object_get_string_member (
                       json_array_get_object_element (messages, 2), "content"),
                   ==, "next");
  g_assert_false (json_object_has_member (
      json_array_get_object_element (messages, 1), "reasoning"));
  teardown ();
}

static void
latest_reminder_payload (void)
{
  g_autoptr (GDateTime) date = g_date_time_new_utc (2026, 11, 9, 12, 0, 0);
  g_autofree gchar *en = format_latest_reminder (date, "en");
  g_autofree gchar *zh = format_latest_reminder (date, "zh_CN");
  g_assert_cmpstr (en, ==, "2026-11-09,Monday,,App,English");
  g_assert_cmpstr (zh, ==, "2026-11-09,星期一,,App,中文");

  g_assert_true (config_set ("latest_reminder", NULL));
  setup ();
  g_assert_false (app->latest_reminder);
  app->latest_reminder = TRUE;
  g_autoptr (JsonObject) user = new_user_message ("new question");
  json_object_set_string_member (user, "latest_reminder", en);
  gsize len;
  g_autofree gchar *body = request_body (2, user, FALSE, &len, NULL);
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_assert_true (json_parser_load_from_data (parser, body, len, NULL));
  JsonObject *root = json_node_get_object (json_parser_get_root (parser));
  JsonArray *messages = json_object_get_array_member (root, "messages");
  g_assert_cmpuint (json_array_get_length (messages), ==, 4);
  JsonObject *marker = json_array_get_object_element (messages, 2);
  g_assert_cmpstr (member_string (marker, "role"), ==, "latest_reminder");
  g_assert_cmpstr (member_string (marker, "content"), ==, en);
  g_assert_cmpstr (member_string (json_array_get_object_element (messages, 3),
                                  "role"), ==, "user");
  commit_message (user);
  g_autoptr (GError) error = NULL;
  g_assert_true (store_save (app->db, app->chats, app->deleted_chat_ids,
                             app->chat->id, app->model, app->effort, &error));
  g_assert_no_error (error);
  g_autofree gchar *current = NULL;
  g_autofree gchar *model = NULL;
  g_autofree gchar *effort = NULL;
  g_autoptr (GPtrArray) chats = store_load (app->db, &current, &model,
                                            &effort, &error);
  g_assert_no_error (error);
  Chat *chat = g_ptr_array_index (chats, 0);
  JsonObject *saved = json_array_get_object_element (chat->messages, 2);
  g_assert_cmpstr (member_string (saved, "latest_reminder"), ==, en);

  g_clear_pointer (&body, g_free);
  body = request_body (3, NULL, FALSE, &len, NULL);
  g_assert_true (json_parser_load_from_data (parser, body, len, NULL));
  root = json_node_get_object (json_parser_get_root (parser));
  messages = json_object_get_array_member (root, "messages");
  marker = json_array_get_object_element (messages, 2);
  g_assert_cmpstr (member_string (marker, "content"), ==, en);

  app->latest_reminder = FALSE;
  g_clear_pointer (&body, g_free);
  body = request_body (3, NULL, FALSE, &len, NULL);
  g_assert_true (json_parser_load_from_data (parser, body, len, NULL));
  root = json_node_get_object (json_parser_get_root (parser));
  messages = json_object_get_array_member (root, "messages");
  g_assert_cmpuint (json_array_get_length (messages), ==, 3);
  g_assert_false (json_object_has_member (
      json_array_get_object_element (messages, 2), "latest_reminder"));
  teardown ();
}

static void
latest_reminder_setting (void)
{
  g_assert_true (config_set ("latest_reminder", NULL));
  if (!setup_ui ())
    return;
  g_autoptr (GtkBuilder) builder = gtk_builder_new_from_resource (
      "/cc/arthur63/gchat/settings.ui");
  AdwSwitchRow *row = ADW_SWITCH_ROW (
      gtk_builder_get_object (builder, "latest_reminder"));
  setup_switch_preference (builder, "latest_reminder", &app->latest_reminder);
  g_assert_false (adw_switch_row_get_active (row));
  adw_switch_row_set_active (row, TRUE);
  g_assert_true (app->latest_reminder);
  g_assert_true (load_disabled_preference ("latest_reminder"));
  adw_switch_row_set_active (row, FALSE);
  g_assert_false (app->latest_reminder);
  g_assert_false (load_disabled_preference ("latest_reminder"));
  teardown ();
}

static JsonArray *
image_ids (const gchar *first,
           const gchar *second)
{
  JsonArray *ids = json_array_new ();
  json_array_add_string_element (ids, first);
  if (second != NULL)
    json_array_add_string_element (ids, second);
  return ids;
}

static void
image_payload (void)
{
  setup ();
  g_autofree gchar *id = g_uuid_string_random ();
  g_autofree gchar *path = image_path (id);
  g_autofree gchar *dir = g_path_get_dirname (path);
  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  g_autoptr (GdkPixbuf) pixbuf =
      gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, 2, 2);
  gdk_pixbuf_fill (pixbuf, 0xff0000ff);
  g_autoptr (GError) error = NULL;
  g_assert_true (gdk_pixbuf_save (pixbuf, path, "png", &error, NULL));
  g_assert_no_error (error);
  g_autofree gchar *second = g_uuid_string_random ();
  g_autofree gchar *second_path = image_path (second);
  gdk_pixbuf_fill (pixbuf, 0x22ff11ff);
  g_assert_true (gdk_pixbuf_save (pixbuf, second_path, "png", &error, NULL));
  g_assert_no_error (error);
  JsonObject *msg = json_array_get_object_element (app->chat->messages, 0);
  json_object_set_array_member (msg, "images", image_ids (id, second));
  gsize len;
  g_autofree gchar *body = request_body (2, NULL, FALSE, &len, &error);
  g_assert_no_error (error);
  g_assert_nonnull (body);
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_assert_true (json_parser_load_from_data (parser, body, len, &error));
  JsonObject *root = json_node_get_object (json_parser_get_root (parser));
  JsonArray *messages = json_object_get_array_member (root, "messages");
  JsonArray *parts = json_object_get_array_member (
      json_array_get_object_element (messages, 0), "content");
  g_assert_cmpuint (json_array_get_length (parts), ==, 3);
  JsonObject *text = json_array_get_object_element (parts, 0);
  g_assert_cmpstr (json_object_get_string_member (text, "text"), ==,
                   "question one");
  const gchar *paths[] = { path, second_path };
  for (guint i = 0; i < G_N_ELEMENTS (paths); i++)
    {
      JsonObject *part = json_array_get_object_element (parts, i + 1);
      JsonObject *image_url = json_object_get_object_member (part, "image_url");
      const gchar *url = json_object_get_string_member (image_url, "url");
      const gchar *prefix = "data:image/png;base64,";
      g_assert_true (g_str_has_prefix (url, prefix));
      gsize decoded_len, image_len;
      g_autofree guchar *decoded = g_base64_decode (url + strlen (prefix),
                                                    &decoded_len);
      g_autofree gchar *bytes = NULL;
      g_assert_true (g_file_get_contents (paths[i], &bytes, &image_len, NULL));
      g_assert_cmpmem (decoded, decoded_len, bytes, image_len);
    }
  g_autoptr (JsonObject) extra = json_object_new ();
  json_object_set_string_member (extra, "role", "user");
  json_object_set_string_member (extra, "content", "");
  json_object_set_array_member (extra, "images", image_ids (id, second));
  g_autofree gchar *image_only =
      request_body (0, extra, FALSE, &len, &error);
  g_assert_no_error (error);
  g_assert_true (json_parser_load_from_data (parser, image_only, len, &error));
  root = json_node_get_object (json_parser_get_root (parser));
  messages = json_object_get_array_member (root, "messages");
  parts = json_object_get_array_member (
      json_array_get_object_element (messages, 0), "content");
  g_assert_cmpuint (json_array_get_length (parts), ==, 2);
  g_free (app->model);
  app->model = g_strdup ("deepseek-v4-pro");
  g_autofree gchar *unsupported =
      request_body (2, NULL, FALSE, &len, &error);
  g_assert_null (unsupported);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error (&error);
  g_free (app->model);
  app->model = g_strdup ("deepseek-flash");
  g_assert_cmpint (g_unlink (path), ==, 0);
  g_autofree gchar *missing = request_body (2, NULL, FALSE, &len, &error);
  g_assert_null (missing);
  g_assert_nonnull (error);
  g_assert_cmpint (g_unlink (second_path), ==, 0);
  g_rmdir (dir);
  teardown ();
}

static void
image_payload_limits (void)
{
  setup ();
  g_autofree gchar *edge_id = g_uuid_string_random ();
  g_autofree gchar *wide_id = g_uuid_string_random ();
  g_autofree gchar *edge_path = image_path (edge_id);
  g_autofree gchar *wide_path = image_path (wide_id);
  g_autofree gchar *dir = g_path_get_dirname (edge_path);
  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  g_autoptr (GdkPixbuf) wide =
      gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 4097, 1);
  gdk_pixbuf_fill (wide, 0x336699ff);
  g_autoptr (GdkPixbuf) edge = gdk_pixbuf_new_subpixbuf (wide, 0, 0, 4096, 1);
  g_assert_true (gdk_pixbuf_save (wide, wide_path, "png", NULL, NULL));
  g_assert_true (gdk_pixbuf_save (edge, edge_path, "png", NULL, NULL));

  JsonArray *messages = app->chat->messages;
  guint text_turns = json_array_get_length (messages);
  g_autoptr (JsonObject) extra = json_object_new ();
  json_object_set_string_member (extra, "role", "user");
  json_object_set_string_member (extra, "content", "");
  json_object_set_array_member (extra, "images", image_ids (edge_id, NULL));
  for (guint i = 0; i < 600; i++)
    copy_message (messages, extra);
  JsonObject *first = json_array_get_object_element (messages, text_turns);
  json_object_set_array_member (first, "images", image_ids (wide_id, NULL));
  json_object_set_array_member (extra, "images", image_ids (edge_id, edge_id));

  gsize len;
  g_autoptr (GError) error = NULL;
  g_autofree gchar *below_threshold =
      request_body (text_turns + 12, extra, FALSE, &len, &error);
  g_assert_no_error (error);
  g_assert_nonnull (below_threshold);
  g_autofree gchar *too_wide =
      request_body (text_turns + 13, extra, FALSE, &len, &error);
  g_assert_null (too_wide);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);

  json_object_set_array_member (first, "images", image_ids (edge_id, NULL));
  g_autofree gchar *at_limit =
      request_body (text_turns + 600, NULL, FALSE, &len, &error);
  g_assert_no_error (error);
  g_assert_nonnull (at_limit);
  g_autofree gchar *too_many =
      request_body (text_turns + 599, extra, FALSE, &len, &error);
  g_assert_null (too_many);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);

  g_assert_cmpint (g_unlink (wide_path), ==, 0);
  g_assert_cmpint (g_unlink (edge_path), ==, 0);
  g_rmdir (dir);
  teardown ();
}

static void
image_import (void)
{
  setup ();
  g_autofree gchar *input = g_build_filename (test_dir, "input.png", NULL);
  g_autoptr (GdkPixbuf) pixbuf =
      gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, 2, 2);
  gdk_pixbuf_fill (pixbuf, 0x123456ff);
  g_autoptr (GError) error = NULL;
  g_assert_true (gdk_pixbuf_save (pixbuf, input, "png", &error, NULL));
  g_autoptr (GFile) file = g_file_new_for_path (input);
  g_autoptr (GCancellable) cancel = g_cancellable_new ();
  g_autofree gchar *id = import_image (file, cancel, &error);
  g_assert_no_error (error);
  g_assert_nonnull (id);
  g_autofree gchar *path = image_path (id);
  g_autofree gchar *original = NULL;
  g_autofree gchar *copied = NULL;
  gsize original_len, copied_len;
  g_assert_true (g_file_get_contents (input, &original, &original_len, NULL));
  g_assert_true (g_file_get_contents (path, &copied, &copied_len, NULL));
  g_assert_cmpmem (original, original_len, copied, copied_len);
  GStatBuf statbuf;
  g_assert_cmpint (g_stat (path, &statbuf), ==, 0);
  g_assert_cmpint (statbuf.st_mode & 0777, ==, 0600);
  g_cancellable_cancel (cancel);
  g_autofree gchar *cancelled = import_image (file, cancel, &error);
  g_assert_null (cancelled);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&error);
  g_assert_true (g_file_set_contents (input, "not an image", -1, NULL));
  g_autofree gchar *invalid = import_image (file, NULL, &error);
  g_assert_null (invalid);
  g_assert_nonnull (error);
  g_clear_error (&error);
  const gchar broken_png[] = "\x89PNG\r\n\x1a\n";
  g_assert_true (g_file_set_contents (input, broken_png, 8, NULL));
  g_autofree gchar *broken = import_image (file, NULL, &error);
  g_assert_null (broken);
  g_assert_nonnull (error);
  g_clear_error (&error);
  g_autoptr (GdkPixbuf) wide =
      gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, 8193, 1);
  gdk_pixbuf_fill (wide, 0xffffffff);
  g_assert_true (gdk_pixbuf_save (wide, input, "png", &error, NULL));
  g_autofree gchar *dimensions = import_image (file, NULL, &error);
  g_assert_null (dimensions);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);
  g_autofree gchar *large = g_malloc0 (IMAGE_MAX_SIZE + 1);
  g_assert_true (g_file_set_contents (input, large, IMAGE_MAX_SIZE + 1, NULL));
  g_autofree gchar *oversize = import_image (file, NULL, &error);
  g_assert_null (oversize);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
  g_clear_error (&error);
  g_autoptr (GBytes) large_bytes =
      g_bytes_new_static (large, IMAGE_MAX_SIZE + 1);
  g_autofree gchar *large_clipboard =
      save_image_bytes (large_bytes, NULL, &error);
  g_assert_null (large_clipboard);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
  g_assert_cmpint (g_unlink (input), ==, 0);
  g_assert_cmpint (g_unlink (path), ==, 0);
  g_autofree gchar *dir = g_path_get_dirname (path);
  g_rmdir (dir);
  teardown ();
}

static void
message_whitespace (void)
{
  if (!setup_ui ())
    return;
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (app->input));
  g_assert_null (input_message ());
  gtk_text_buffer_set_text (buffer, " \t\n", -1);
  g_assert_null (input_message ());
  const gchar *code = "    indented code  \n\n";
  gtk_text_buffer_set_text (buffer, code, -1);
  g_autofree gchar *text = input_message ();
  g_assert_cmpstr (text, ==, code);
  g_autoptr (JsonObject) user_msg = new_user_message (text);
  commit_message (user_msg);
  JsonObject *msg = json_array_get_object_element (app->chat->messages, 2);
  g_assert_cmpstr (json_object_get_string_member (msg, "content"), ==, code);
  teardown ();
}

static void
edit_and_fork (void)
{
  if (!setup_ui ())
    return;
  add_message ("user", "question two");
  add_message ("assistant", "answer two");
  Chat *source = app->chat;
  g_free (source->draft);
  source->draft = g_strdup ("original draft");
  render_chat ();

  GtkWidget *first = gtk_widget_get_first_child (app->messages);
  GtkWidget *third =
      gtk_widget_get_next_sibling (gtk_widget_get_next_sibling (first));
  on_edit_message (NULL, third);
  g_assert_cmpuint (source->edit_from, ==, 2);
  g_assert_cmpstr (source->edit_text, ==, "question two");
  g_assert_cmpstr (source->draft, ==, "original draft");

  regenerate ();
  g_assert_cmpuint (json_array_get_length (source->messages), ==, 4);
  g_assert_cmpuint (source->edit_from, ==, 2);

  first = gtk_widget_get_first_child (app->messages);
  on_delete_message (NULL, first);
  g_assert_cmpuint (source->edit_from, ==, 1);
  g_assert_cmpuint (json_array_get_length (source->messages), ==, 3);

  g_autoptr (JsonObject) edited = new_user_message ("edited question two");
  commit_message (edited);
  g_assert_cmpuint (json_array_get_length (source->messages), ==, 2);
  g_assert_cmpstr (
      json_object_get_string_member (
          json_array_get_object_element (source->messages, 1), "content"),
      ==, "edited question two");
  g_assert_cmpuint (source->edit_from, ==, G_MAXUINT);

  render_chat ();
  GtkWidget *second =
      gtk_widget_get_next_sibling (gtk_widget_get_first_child (app->messages));
  on_fork_message (NULL, second);
  Chat *fork = app->chat;
  g_assert_true (fork != source);
  g_assert_cmpuint (app->chats->len, ==, 2);
  g_assert_cmpuint (json_array_get_length (fork->messages), ==, 1);
  g_assert_cmpstr (fork->draft, ==, "edited question two");

  delete_chat (fork->id);
  g_assert_cmpuint (app->chats->len, ==, 1);
  g_assert_true (app->chat == source);
  teardown ();
}

static void
history_search (void)
{
  setup ();
  g_autoptr (GString) long_text = g_string_new (NULL);
  for (guint i = 0; i < 200; i++)
    g_string_append (long_text, "很长的前文 ");
  g_string_append (long_text, "Needle [a.*] 中文\n后文");
  JsonObject *msg = json_array_get_object_element (app->chat->messages, 1);
  json_object_set_string_member (msg, "content", long_text->str);
  json_object_set_string_member (msg, "reasoning", "not-in-body");

  g_autoptr (GRegex) title_re =
      g_regex_new ("测试", G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL);
  g_autofree gchar *title_hit = search_preview (app->chat, title_re);
  g_assert_cmpstr (title_hit, ==, "");

  g_autofree gchar *escaped = g_regex_escape_string ("[a.*]", -1);
  g_autoptr (GRegex) body_re =
      g_regex_new (escaped, G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL);
  g_autofree gchar *body_hit = search_preview (app->chat, body_re);
  g_assert_nonnull (body_hit);
  g_assert_true (g_str_has_prefix (body_hit, "…[a.*] 中文 后文"));

  g_autoptr (GRegex) miss_re =
      g_regex_new ("not-in-body", G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0,
                   NULL);
  g_assert_null (search_preview (app->chat, miss_re));
  teardown ();
}

static void
message_navigation_rows (void)
{
  if (!setup_ui ())
    return;

  GtkListBox *list = GTK_LIST_BOX (app->message_nav_list);
  g_autoptr (GtkWidget) placeholder = g_object_ref (app->message_nav_empty);
  g_autoptr (GString) question = g_string_new ("  例子\n");
  for (guint i = 0; i < 120; i++)
    g_string_append (question, "正文");
  g_string_append (question, "Needle [a.*]");
  add_message ("user", question->str);
  add_message ("assistant", "reply-only term");
  add_message ("user", "");
  render_chat ();

  GtkEditable *search = GTK_EDITABLE (app->message_nav_search);
  GtkListBoxRow *first = gtk_list_box_get_row_at_index (list, 0);
  GtkListBoxRow *second = gtk_list_box_get_row_at_index (list, 1);
  GtkListBoxRow *third = gtk_list_box_get_row_at_index (list, 2);
  g_assert_true (gtk_widget_get_visible (app->message_nav));
  g_assert_nonnull (first);
  g_assert_nonnull (second);
  g_assert_nonnull (third);
  g_assert_null (gtk_list_box_get_row_at_index (list, 3));
  g_assert_false (gtk_widget_get_visible (placeholder));
  GtkLabel *label = GTK_LABEL (gtk_list_box_row_get_child (second));
  const gchar *preview = gtk_label_get_text (label);
  g_assert_true (g_utf8_validate (preview, -1, NULL));
  g_assert_true (g_str_has_prefix (preview, "2. 例子 正文"));
  g_assert_true (g_str_has_suffix (preview, "…"));
  label = GTK_LABEL (gtk_list_box_row_get_child (third));
  g_assert_cmpstr (gtk_label_get_text (label), ==, "3. No text");

  gtk_editable_set_text (search, "needle [a.*]");
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (first)));
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (second)));
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (third)));
  g_assert_false (gtk_widget_get_visible (placeholder));
  gtk_editable_set_text (search, "reply-only term");
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (second)));
  g_assert_true (gtk_widget_get_parent (placeholder) == GTK_WIDGET (list));
  g_assert_true (gtk_widget_get_visible (placeholder));
  g_assert_true (gtk_widget_get_child_visible (placeholder));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (placeholder)), ==,
                   "No matching messages");

  app->chat->edit_from = 2;
  render_chat ();
  first = gtk_list_box_get_row_at_index (list, 0);
  g_assert_nonnull (first);
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (first)));
  g_assert_false (gtk_widget_get_visible (placeholder));
  g_assert_null (gtk_list_box_get_row_at_index (list, 1));
  g_assert_cmpstr (gtk_editable_get_text (search), ==, "");

  app->chat = chat_new ("43", "New chat");
  g_ptr_array_add (app->chats, app->chat);
  render_chat ();
  g_assert_false (gtk_widget_get_visible (app->message_nav));
  g_assert_null (gtk_list_box_get_row_at_index (list, 0));
  teardown ();
}

static void
message_navigation_jump (void)
{
  if (!setup_ui ())
    return;

  add_message ("user", "Jump target");
  add_message ("assistant", "target answer");
  for (guint i = 0; i < 24; i++)
    {
      add_message ("user", "Later question");
      add_message ("assistant", "Later answer");
    }
  render_chat ();
  gtk_window_present (GTK_WINDOW (app->window));

  gint64 deadline = g_get_monotonic_time () + 3 * G_TIME_SPAN_SECOND;
  while (g_get_monotonic_time () < deadline)
    {
      while (g_main_context_iteration (NULL, FALSE))
        {
        }
      if (stick_bottom (&chat_stick) > 0 &&
          gtk_adjustment_get_value (chat_stick.adj) == stick_bottom (&chat_stick))
        break;
      g_usleep (1000);
    }
  g_assert_cmpfloat (stick_bottom (&chat_stick), >, 0);
  g_assert_cmpfloat (gtk_adjustment_get_value (chat_stick.adj), ==,
                     stick_bottom (&chat_stick));

  GtkMenuButton *menu = GTK_MENU_BUTTON (app->message_nav);
  gtk_menu_button_popup (menu);
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (
      gtk_menu_button_get_popover (menu))));
  gtk_editable_set_text (GTK_EDITABLE (app->message_nav_search), "jump target");
  g_signal_emit_by_name (app->message_nav_search, "activate");
  g_assert_false (chat_stick.follow);
  GtkWidget *bubble = gtk_widget_get_first_child (app->messages);
  bubble = gtk_widget_get_next_sibling (gtk_widget_get_next_sibling (bubble));
  GtkWidget *viewport = gtk_scrolled_window_get_child (
      GTK_SCROLLED_WINDOW (app->scroller));
  GtkWidget *content = gtk_viewport_get_child (GTK_VIEWPORT (viewport));
  graphene_rect_t bounds;
  g_assert_true (gtk_widget_compute_bounds (bubble, content, &bounds));
  g_assert_cmpfloat (bounds.origin.y, >, 12);
  g_assert_cmpfloat (gtk_adjustment_get_value (chat_stick.adj), ==,
                     bounds.origin.y - 12);

  on_message_nav_latest (NULL, NULL);
  while (g_main_context_iteration (NULL, FALSE))
    {
    }
  g_assert_true (chat_stick.follow);
  g_assert_cmpfloat (gtk_adjustment_get_value (chat_stick.adj), ==,
                     stick_bottom (&chat_stick));
  teardown ();
}

static void
config_and_font_helpers (void)
{
  g_autofree gchar *previous_env = g_strdup (g_getenv ("DEEPSEEK_API_KEY"));
  g_setenv ("DEEPSEEK_API_KEY", "env-key", TRUE);
  setup ();
  g_assert_true (save_api_key ("local-key"));
  g_assert_cmpstr (app->api_key, ==, "env-key");
  g_autofree gchar *saved = config_get ("api_key");
  g_assert_cmpstr (saved, ==, "local-key");
  g_unsetenv ("DEEPSEEK_API_KEY");
  g_assert_true (save_api_key ("second-key"));
  g_assert_cmpstr (app->api_key, ==, "second-key");
  teardown ();
  if (previous_env != NULL)
    g_setenv ("DEEPSEEK_API_KEY", previous_env, TRUE);

  g_assert_false (font_family_valid (NULL));
  g_assert_false (font_family_valid (""));
  g_assert_false (font_family_valid ("font\nname"));
  g_assert_false (font_family_valid ("\xff"));
  g_assert_true (font_family_valid ("苹方-简"));
  static const gchar *invalid[] = { "0", "73", "nan", "inf", "15,5",
                                    "15px", "15 trailing" };
  for (guint i = 0; i < G_N_ELEMENTS (invalid); i++)
    {
      g_assert_true (config_set ("message_font_size", invalid[i]));
      g_assert_cmpfloat (font_size (&FONT_SETTINGS[0]), ==, 14);
    }
  g_assert_true (config_set ("message_font_size", "15.5"));
  g_assert_cmpfloat (font_size (&FONT_SETTINGS[0]), ==, 15.5);
  g_autofree gchar *path = store_config_path ();
  g_unlink (path);
}

static void
corrupt_startup (void)
{
  g_autofree gchar *path = store_session_path ();
  g_autofree gchar *dir = g_path_get_dirname (path);
  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  g_assert_true (g_file_set_contents (path, "not a database", -1, NULL));
  gchar *argv[] = { "gchat", NULL };
  g_assert_cmpint (gchat_main (1, argv), ==, 1);
  g_assert_null (app);
  g_autofree gchar *contents = NULL;
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==, "not a database");
  g_unlink (path);
}

static void
locked_startup (void)
{
  g_autofree gchar *path = store_session_path ();
  g_autoptr (GError) error = NULL;
  gint lock = store_lock (path, &error);
  g_assert_no_error (error);
  g_assert_cmpint (lock, >=, 0);
  g_assert_true (g_file_set_contents (path, "do not overwrite", -1, NULL));
  gchar *argv[] = { "gchat", NULL };
  g_assert_cmpint (gchat_main (1, argv), ==, 1);
  g_assert_null (app);
  g_autofree gchar *text = NULL;
  g_assert_true (g_file_get_contents (path, &text, NULL, NULL));
  g_assert_cmpstr (text, ==, "do not overwrite");
  g_unlink (path);
  close (lock);
}

int
main (int    argc,
      char **argv)
{
  g_autoptr (GError) error = NULL;
  test_dir = g_dir_make_tmp ("gchat-app-test-XXXXXX", &error);
  g_assert_no_error (error);
  g_unsetenv ("FLATPAK_ID");
  g_setenv ("XDG_DATA_HOME", test_dir, TRUE);
  g_setenv ("XDG_CONFIG_HOME", test_dir, TRUE);
  g_test_init (&argc, &argv, NULL);
  i18n_init (NULL);
  gtk_disable_setlocale ();
  have_display = gtk_init_check ();
  if (have_display)
    {
      adw_init ();
      load_css ();
    }
  g_test_add_func ("/app/sse-parse", sse_parser);
  g_test_add_data_func ("/app/sse-stream/multiline", GINT_TO_POINTER (0),
                        sse_stream);
  g_test_add_data_func ("/app/sse-stream/incomplete", GINT_TO_POINTER (1),
                        sse_stream);
  g_test_add_func ("/app/sse-stream/cancel-pending", sse_cancel_pending);
  g_test_add_func ("/app/stream/frame-batch", stream_frame_batch);
  g_test_add_func ("/app/reasoning-lifecycle", reasoning_lifecycle);
  g_test_add_func ("/app/web/stream", web_stream);
  g_test_add_func ("/app/web/stop", web_stop);
  g_test_add_func ("/app/web/toggle", web_toggle);
  g_test_add_func ("/app/title-generation", title_generation);
  g_test_add_func ("/app/payload-filters-reasoning", payload_filters_reasoning);
  g_test_add_func ("/app/latest-reminder", latest_reminder_payload);
  g_test_add_func ("/app/latest-reminder-setting", latest_reminder_setting);
  g_test_add_func ("/app/image-payload", image_payload);
  g_test_add_func ("/app/image-payload-limits", image_payload_limits);
  g_test_add_func ("/app/image-import", image_import);
  g_test_add_func ("/app/message-whitespace", message_whitespace);
  g_test_add_func ("/app/edit-and-fork", edit_and_fork);
  g_test_add_func ("/app/history-search", history_search);
  g_test_add_func ("/app/message-navigation/rows", message_navigation_rows);
  g_test_add_func ("/app/message-navigation/jump", message_navigation_jump);
  g_test_add_func ("/app/config-and-font-helpers", config_and_font_helpers);
  g_test_add_func ("/app/corrupt-startup", corrupt_startup);
  g_test_add_func ("/app/locked-startup", locked_startup);
  int status = g_test_run ();
  g_autofree gchar *lock =
      g_build_filename (test_dir, "gchat", "session.db.lock", NULL);
  g_unlink (lock);
  g_autofree gchar *dir = g_build_filename (test_dir, "gchat", NULL);
  g_rmdir (dir);
  g_rmdir (test_dir);
  g_free (test_dir);
  return status;
}
