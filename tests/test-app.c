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

static gchar *
request_body (guint        turns,
              JsonObject  *extra,
              gboolean     request_title,
              gsize       *len,
              GError     **error)
{
  return chat_request_body (context_prefix (app->chat, turns), turns, extra,
                            request_title, len, error);
}

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
  app->context_keep_turns = CONTEXT_DEFAULT_KEEP_TURNS;
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
  app->closing = TRUE;
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
  g_free (app->default_system_prompt);
  g_free (app->kill_text);
  g_queue_clear_full (&app->pending_toasts, g_object_unref);
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
    {
      g_autoptr (JsonObject) response = parse_response (invalid[i], -1);
      g_assert_null (first_choice (response));
    }

  g_autoptr (JsonObject) response = parse_response (
      "{\"choices\":\n[{\"delta\":{\"reasoning_content\":\"think\","
      "\"content\":\"hi\"}}]}",
      -1);
  JsonObject *choice = first_choice (response);
  g_assert_nonnull (choice);
  JsonObject *delta = member_object (choice, "delta");
  g_assert_cmpstr (member_string (delta, "content"), ==, "hi");
  g_assert_cmpstr (member_string (delta, "reasoning_content"), ==, "think");
  g_assert_null (member_string (delta, "role"));

  g_clear_pointer (&response, json_object_unref);
  response = parse_response ("{\"choices\":[{\"delta\":42}]}", -1);
  choice = first_choice (response);
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
context_payload (void)
{
  setup ();
  add_message ("user", "question two");
  add_message ("assistant", "answer two");
  app->default_system_prompt = g_strdup ("Use plain English.\nBe concise.");
  set_summary (app->chat, g_strdup ("Saved facts"), 2, 4);
  g_autoptr (JsonObject) extra = new_user_message ("next question");
  gsize len;
  g_autofree gchar *body = request_body (4, extra, FALSE, &len, NULL);
  g_assert_nonnull (body);
  g_autoptr (JsonObject) root = parse_response (body, len);
  JsonArray *messages = member_array (root, "messages");
  g_assert_cmpuint (json_array_get_length (messages), ==, 5);
  JsonObject *first = json_array_get_object_element (messages, 0);
  g_assert_cmpstr (member_string (first, "role"), ==, "system");
  g_assert_cmpstr (member_string (first, "content"), ==,
                   app->default_system_prompt);
  JsonObject *summary = json_array_get_object_element (messages, 1);
  g_assert_cmpstr (member_string (summary, "role"), ==, "user");
  g_assert_true (g_str_has_suffix (member_string (summary, "content"),
                                   "Saved facts"));
  g_assert_cmpstr (member_string (json_array_get_object_element (messages, 2),
                                  "content"),
                   ==, "question two");
  g_assert_cmpstr (member_string (json_array_get_object_element (messages, 4),
                                  "content"),
                   ==, "next question");
  g_assert_null (strstr (body, "question one"));
  g_assert_true (json_object_get_boolean_member (
      member_object (root, "stream_options"), "include_usage"));

  app->chat->system_prompt = g_strdup ("Override");
  g_clear_pointer (&body, g_free);
  body = request_body (4, extra, FALSE, &len, NULL);
  g_assert_nonnull (strstr (body, "Override"));
  g_assert_null (strstr (body, "Use plain English."));

  g_free (app->chat->system_prompt);
  app->chat->system_prompt = g_strdup ("");
  g_clear_pointer (&body, g_free);
  body = request_body (1, extra, FALSE, &len, NULL);
  g_assert_null (strstr (body, "\"system\""));
  g_assert_null (strstr (body, "Saved facts"));
  g_assert_nonnull (strstr (body, "question one"));

  g_autoptr (Chat) fork = chat_new_empty ();
  copy_messages (fork->messages, app->chat->messages, 1);
  copy_context (fork, app->chat);
  g_assert_null (fork->summary);
  g_assert_cmpstr (fork->system_prompt, ==, "");

  g_autoptr (Chat) covered_fork = chat_new_empty ();
  copy_messages (covered_fork->messages, app->chat->messages, 2);
  copy_context (covered_fork, app->chat);
  g_assert_cmpstr (covered_fork->summary, ==, "Saved facts");
  g_assert_cmpuint (covered_fork->summary_position, ==, 2);

  app->chat->edit_from = 0;
  g_assert_false (commit_message (extra));
  g_assert_cmpuint (json_array_get_length (app->chat->messages), ==, 4);
  g_assert_cmpuint (app->chat->edit_from, ==, 0);
  g_assert_nonnull (app->chat->summary);
  teardown ();
}

static void
compression_rules (void)
{
  setup ();
  add_message ("user", "question two");
  add_message ("assistant", "answer two");
  add_message ("user", "question three");
  add_message ("assistant", "answer three");
  g_autoptr (JsonObject) extra = new_user_message ("next question");
  ContextPrefix prefix = context_prefix (app->chat, 6);
  gint64 estimate = estimate_prompt (prefix, 6, extra);
  app->context_threshold = 2000;
  g_assert_cmpuint (manual_compression_boundary (), ==, 2);
  g_assert_cmpuint (automatic_compression_boundary (prefix, 6, 2000), ==, 0);
  app->auto_compact = TRUE;
  g_assert_cmpuint (automatic_compression_boundary (prefix, 6, estimate),
                    ==, 0);
  g_assert_cmpuint (automatic_compression_boundary (prefix, 6, 2000), ==, 2);

  app->request_token_estimate = estimate - 5;
  process_sse_event ("{\"choices\":[],\"usage\":{\"prompt_tokens\":1.5}}");
  g_assert_cmpint (app->chat->prompt_tokens, ==, 0);
  process_sse_event ("{\"choices\":[],\"usage\":{\"prompt_tokens\":1995}}");
  g_assert_cmpint (app->chat->prompt_tokens, ==, 1995);
  g_assert_cmpuint (automatic_compression_boundary (prefix, 6, estimate),
                    ==, 2);
  g_free (app->model);
  app->model = g_strdup ("deepseek-v4-pro");
  g_assert_cmpuint (automatic_compression_boundary (prefix, 6, estimate),
                    ==, 0);
  g_assert_cmpuint (compression_boundary (prefix, 4), ==, 0);

  Compression compression = { .turns = 6, .until = 2 };
  apply_compression (&compression, g_strdup ("Remember the first turn."));
  g_assert_cmpuint (manual_compression_boundary (), ==, 0);
  g_assert_cmpuint (json_array_get_length (app->chat->messages), ==, 6);
  g_assert_cmpuint (app->chat->summary_until, ==, 2);
  g_assert_cmpstr (app->chat->summary, ==, "Remember the first turn.");
  g_assert_null (app->chat->usage_model);
  g_assert_false (app->chat->dirty);

  add_message ("user", "question four");
  g_assert_cmpuint (manual_compression_boundary (), ==, 4);
  prefix = context_prefix (app->chat, 7);
  gsize len;
  g_autofree gchar *body = summary_request_body (prefix, 4, &len, NULL);
  g_assert_nonnull (body);
  g_assert_nonnull (strstr (body, "Remember the first turn."));
  g_assert_nonnull (strstr (body, "question two"));
  g_assert_null (strstr (body, "question one"));
  g_assert_null (strstr (body, "question three"));

  g_free (app->chat->draft);
  app->chat->draft = g_strdup ("not sent");
  g_autofree gchar *id = g_uuid_string_random ();
  g_autoptr (JsonArray) images = json_array_new ();
  json_array_add_string_element (images, id);
  json_object_set_array_member (extra, "images", g_steal_pointer (&images));
  Compression blocked = { .extra = extra, .turns = 7, .until = 4 };
  apply_compression (&blocked, g_strdup ("Do not install this summary."));
  g_assert_cmpstr (app->chat->summary, ==, "Remember the first turn.");
  g_assert_cmpuint (app->chat->summary_until, ==, 2);
  g_assert_cmpstr (app->chat->draft, ==, "not sent");
  g_assert_cmpuint (json_array_get_length (app->chat->messages), ==, 7);
  teardown ();
}

static void
compression_retention (void)
{
  setup ();
  add_message ("user", "question two");
  add_message ("assistant", "answer two");
  add_message ("user", "question three");
  add_message ("assistant", "answer three");
  ContextPrefix prefix = context_prefix (app->chat, 6);
  app->auto_compact = TRUE;
  app->context_threshold = 1024;
  static const struct
  {
    guint keep;
    guint boundary;
  } cases[] = { { 0, 6 }, { 1, 4 }, { 2, 2 }, { 3, 0 }, { 100, 0 } };
  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      app->context_keep_turns = cases[i].keep;
      g_assert_cmpuint (manual_compression_boundary (), ==, cases[i].boundary);
      g_assert_cmpuint (automatic_compression_boundary (prefix, 6, 1024),
                        ==, cases[i].boundary);
    }

  set_summary (app->chat, g_strdup ("Existing summary"), 2, 6);
  g_assert_cmpuint (manual_compression_boundary (), ==, 0);
  g_assert_cmpuint (app->chat->summary_until, ==, 2);
  app->context_keep_turns = 0;
  g_assert_cmpuint (manual_compression_boundary (), ==, 6);
  set_summary (app->chat, g_strdup ("All earlier history"), 6, 6);
  g_assert_cmpuint (manual_compression_boundary (), ==, 0);
  teardown ();
}

static void
summary_response (void)
{
  static const gchar *invalid[] = {
    "",
    "<html>error</html>",
    "{\"choices\":[]}",
    "{\"choices\":[{\"finish_reason\":\"length\","
    "\"message\":{\"content\":\"a\"}}]}",
    "{\"choices\":[{\"finish_reason\":\"stop\","
    "\"message\":{\"content\":\" \\n\"}}]}",
  };
  for (guint i = 0; i < G_N_ELEMENTS (invalid); i++)
    {
      g_autoptr (GBytes) bytes = g_bytes_new_static (invalid[i],
                                                     strlen (invalid[i]));
      g_assert_null (parse_summary (bytes));
    }
  static const gchar valid[] =
      "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{"
      "\"content\":\" \\n摘要 \\t\"}}]}";
  g_autoptr (GBytes) bytes = g_bytes_new_static (valid, sizeof valid - 1);
  g_autofree gchar *parsed = parse_summary (bytes);
  g_assert_cmpstr (parsed, ==, "摘要");
}

static void
context_ui (void)
{
  if (!setup_ui ())
    return;
  GtkWidget *toolbar = gtk_widget_get_parent (app->context_menu);
  g_assert_true (gtk_widget_get_parent (toolbar) ==
                 gtk_widget_get_parent (app->input_frame));
  g_assert_true (gtk_widget_get_first_child (toolbar) == app->context_menu);
  g_assert_true (gtk_widget_get_next_sibling (app->context_menu) ==
                 app->attach);

  GtkMenuButton *menu = GTK_MENU_BUTTON (app->context_menu);
  g_assert_cmpint (gtk_menu_button_get_direction (menu), ==, GTK_ARROW_UP);
  g_assert_true (gtk_menu_button_get_child (menu) == app->context_ring);
  g_assert_true (GTK_IS_DRAWING_AREA (app->context_ring));
  g_autofree gchar *small = format_token_count (999);
  g_autofree gchar *thousands = format_token_count (11000);
  g_autofree gchar *rounded = format_token_count (6058);
  g_assert_cmpstr (small, ==, "999");
  g_assert_cmpstr (thousands, ==, "11K");
  g_assert_cmpstr (rounded, ==, "6K");
  GtkPopover *popover = gtk_menu_button_get_popover (menu);
  GtkWidget *entries = gtk_popover_get_child (popover);
  GtkWidget *prompt_button = gtk_widget_get_prev_sibling (app->compress_button);
  g_assert_true (GTK_IS_BUTTON (prompt_button));
  g_assert_false (gtk_widget_has_css_class (prompt_button, "flat"));
  g_assert_false (gtk_widget_has_css_class (app->compress_button, "flat"));
  g_assert_true (gtk_widget_has_css_class (app->context_percent, "dimmed"));
  g_assert_true (gtk_widget_has_css_class (app->context_percent, "numeric"));
  GtkWidget *actions = gtk_widget_get_parent (app->compress_button);
  g_assert_true (gtk_widget_get_parent (actions) == entries);
  g_assert_cmpint (gtk_orientable_get_orientation (GTK_ORIENTABLE (actions)),
                   ==, GTK_ORIENTATION_HORIZONTAL);
  g_assert_cmpstr (gtk_button_get_label (GTK_BUTTON (prompt_button)), ==,
                   "System prompt");
  g_assert_cmpstr (gtk_button_get_label (GTK_BUTTON (app->compress_button)),
                   ==, "Compress context");
  for (GtkWidget *child = gtk_widget_get_first_child (entries); child != NULL;
       child = gtk_widget_get_next_sibling (child))
    g_assert_false (GTK_IS_SEPARATOR (child));
  g_assert_null (gtk_widget_get_next_sibling (app->compress_button));
  GtkWidget *spinner = gtk_widget_get_first_child (app->context_status);
  g_assert_true (GTK_IS_SPINNER (spinner));
  g_assert_false (gtk_spinner_get_spinning (GTK_SPINNER (spinner)));
  gtk_widget_set_visible (app->context_status, TRUE);
  g_assert_true (gtk_spinner_get_spinning (GTK_SPINNER (spinner)));
  gtk_widget_set_visible (app->context_status, FALSE);
  g_assert_false (gtk_spinner_get_spinning (GTK_SPINNER (spinner)));

  g_assert_cmpint (estimate_prompt ((ContextPrefix){ 0 }, 0, NULL), ==, 0);
  on_context_popover_show (NULL, NULL);
  g_assert_false (gtk_widget_get_sensitive (app->compress_button));
  g_assert_cmpstr (gtk_widget_get_tooltip_text (app->compress_button), ==,
                   "Needs more than 2 active turns to compress");
  ContextUsage initial = context_usage ();
  g_assert_cmpuint (initial.messages, ==, 2);
  g_assert_cmpuint (initial.turns, ==, 1);
  g_assert_cmpint (calibrated_prompt (initial.estimate), ==, initial.estimate);
  GtkTextBuffer *input = gtk_text_view_get_buffer (GTK_TEXT_VIEW (app->input));
  gtk_text_buffer_set_text (input, " \n  ", -1);
  g_assert_cmpint (context_usage ().estimate, ==, initial.estimate);
  gtk_text_buffer_set_text (input, "A longer draft in the input", -1);
  ContextUsage draft = context_usage ();
  g_assert_cmpint (draft.estimate, >, initial.estimate);
  g_assert_cmpfloat (app->context_fraction, ==,
                     (gdouble) draft.estimate / CONTEXT_DEFAULT_THRESHOLD);
  app->chat->draft_images = json_array_new ();
  json_array_add_string_element (app->chat->draft_images, "test-image");
  refresh_context ();
  ContextUsage with_image = context_usage ();
  g_assert_cmpint (with_image.estimate, ==, draft.estimate + 1024);
  g_assert_cmpfloat (app->context_fraction, ==,
                     (gdouble) with_image.estimate /
                         CONTEXT_DEFAULT_THRESHOLD);
  g_clear_pointer (&app->chat->draft_images, json_array_unref);

  app->chat->usage_model = g_strdup (app->model);
  app->chat->prompt_tokens = 500;
  app->chat->estimated_tokens = draft.estimate;
  ContextUsage calibrated = context_usage ();
  g_assert_cmpint (calibrated.estimate, ==, 500);
  app->chat->edit_from = 1;
  refresh_context ();
  g_assert_cmpint (calibrated_prompt (100), ==, 100);
  g_assert_cmpuint (context_usage ().messages, ==, 1);
  g_assert_cmpstr (gtk_widget_get_tooltip_text (app->compress_button), ==,
                   "Finish or cancel editing before compressing");
  app->chat->edit_from = G_MAXUINT;
  g_free (app->model);
  app->model = g_strdup ("deepseek-v4-pro");
  g_assert_cmpint (calibrated_prompt (100), ==, 100);
  g_free (app->model);
  app->model = g_strdup ("deepseek-flash");
  app->context_threshold = 625;
  refresh_context ();
  g_assert_true (gtk_widget_has_css_class (app->context_ring, "warning"));
  g_assert_true (gtk_widget_has_css_class (app->context_progress, "warning"));
  app->context_threshold = 400;
  refresh_context ();
  g_assert_true (gtk_widget_has_css_class (app->context_ring, "danger"));
  g_assert_true (gtk_widget_has_css_class (app->context_progress, "danger"));
  cairo_surface_t *surface = cairo_recording_surface_create (
      CAIRO_CONTENT_COLOR_ALPHA, NULL);
  cairo_t *cr = cairo_create (surface);
  draw_context_ring (GTK_DRAWING_AREA (app->context_ring), cr, 40, 40, NULL);
  gdouble x, y, width, height;
  cairo_recording_surface_ink_extents (surface, &x, &y, &width, &height);
  g_assert_cmpfloat (width, <=, 18);
  g_assert_cmpfloat (height, <=, 18);
  cairo_destroy (cr);
  cairo_surface_destroy (surface);
  g_assert_cmpfloat (gtk_progress_bar_get_fraction (
                         GTK_PROGRESS_BAR (app->context_progress)),
                     ==, 1);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (app->context_percent)), ==,
                   "500/400 (125%)");
  app->chat->prompt_tokens = 11000;
  app->context_threshold = 123000;
  refresh_context ();
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (app->context_percent)), ==,
                   "11K/123K (9%)");
  set_summary (app->chat, g_strdup ("Earlier facts"), 2, 2);
  refresh_context ();
  ContextUsage summarized = context_usage ();
  g_assert_cmpint (calibrated_prompt (100), ==, 100);
  g_assert_cmpuint (summarized.summarized, ==, 2);
  g_assert_cmpuint (summarized.messages, ==, 0);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (app->context_summary_info)),
                   ==, "2 messages compressed");
  g_assert_cmpstr (gtk_widget_get_tooltip_text (app->compress_button), ==,
                   "No earlier messages to compress");
  app->auto_compact = TRUE;
  refresh_context ();
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (app->context_auto_info)),
                   ==, "On · keep 2 turns");
  g_assert_true (gtk_widget_get_last_child (entries) == actions);
  on_prompt_clicked (NULL, NULL);
  AdwAlertDialog *editor = ADW_ALERT_DIALOG (
      adw_application_window_get_visible_dialog (
          ADW_APPLICATION_WINDOW (app->window)));
  g_assert_nonnull (editor);
  g_assert_cmpstr (adw_alert_dialog_get_body (editor), ==,
                   "Applies to future replies in this chat");
  GtkWidget *mode = g_object_get_data (G_OBJECT (editor), "prompt-mode");
  GtkWidget *scroll = g_object_get_data (G_OBJECT (editor), "prompt-scroll");
  GtkTextView *view = g_object_get_data (G_OBJECT (editor), "prompt-view");
  GtkTextBuffer *buffer = g_object_get_data (G_OBJECT (editor),
                                             "prompt-buffer");
  GtkLabel *tokens = g_object_get_data (G_OBJECT (editor), "prompt-tokens");
  g_assert_true (gtk_widget_has_css_class (mode, "linked"));
  g_assert_cmpuint (selected_prompt_mode (editor), ==, PROMPT_INHERIT);
  g_assert_true (gtk_widget_get_visible (scroll));
  g_assert_false (gtk_text_view_get_editable (view));
  g_assert_true (gtk_widget_has_css_class (GTK_WIDGET (view), "dimmed"));
  g_assert_cmpstr (gtk_label_get_text (tokens), ==, "0 tokens");
  GtkWidget *inherit_btn = gtk_widget_get_first_child (mode);
  GtkWidget *custom_btn = gtk_widget_get_next_sibling (inherit_btn);
  GtkWidget *omit_btn = gtk_widget_get_next_sibling (custom_btn);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (custom_btn), TRUE);
  g_assert_cmpuint (selected_prompt_mode (editor), ==, PROMPT_CUSTOM);
  g_assert_true (gtk_text_view_get_editable (view));
  g_assert_false (gtk_widget_has_css_class (GTK_WIDGET (view), "dimmed"));
  g_assert_false (adw_alert_dialog_get_response_enabled (editor, "save"));
  gtk_text_buffer_set_text (buffer, " \n\t ", -1);
  g_assert_false (adw_alert_dialog_get_response_enabled (editor, "save"));
  g_assert_cmpstr (gtk_label_get_text (tokens), ==, "0 tokens");
  gtk_text_buffer_set_text (buffer, "第一行\n第二行", -1);
  g_assert_true (adw_alert_dialog_get_response_enabled (editor, "save"));
  g_assert_cmpstr (gtk_label_get_text (tokens), ==,
                   "Approximately 11 tokens");
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (omit_btn), TRUE);
  g_assert_cmpuint (selected_prompt_mode (editor), ==, PROMPT_OMIT);
  g_assert_false (gtk_text_view_get_editable (view));
  g_assert_cmpstr (gtk_label_get_text (tokens), ==, "0 tokens");
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (custom_btn), TRUE);
  g_assert_true (gtk_text_view_get_buffer (view) == buffer);
  g_signal_emit_by_name (editor, "response::save", "save");
  g_assert_cmpstr (app->chat->system_prompt, ==, "第一行\n第二行");
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (app->context_prompt_info)),
                   ==, "Custom prompt");
  adw_dialog_close (ADW_DIALOG (editor));
  on_prompt_clicked (NULL, GINT_TO_POINTER (TRUE));
  editor = ADW_ALERT_DIALOG (adw_application_window_get_visible_dialog (
      ADW_APPLICATION_WINDOW (app->window)));
  g_assert_nonnull (editor);
  g_assert_cmpstr (adw_alert_dialog_get_body (editor), ==,
                   "Leave blank to omit the default prompt");
  adw_dialog_close (ADW_DIALOG (editor));

  g_autoptr (GtkBuilder) builder = gtk_builder_new_from_resource (
      "/cc/arthur63/gchat/settings.ui");
  const gchar *rows[] = { "default_prompt_row", "auto_compact",
                          "context_threshold", "context_keep_turns",
                          "language" };
  for (guint i = 0; i < G_N_ELEMENTS (rows); i++)
    {
      AdwActionRow *setting = ADW_ACTION_ROW (
          gtk_builder_get_object (builder, rows[i]));
      g_assert_cmpstr (adw_action_row_get_subtitle (setting), ==, "");
    }
  AdwActionRow *prompt_row = ADW_ACTION_ROW (
      gtk_builder_get_object (builder, "default_prompt_row"));
  refresh_default_prompt_row (prompt_row);
  g_assert_cmpstr (adw_action_row_get_subtitle (prompt_row), ==, "Not set");
  app->default_system_prompt = g_strdup ("默认第一行\n默认第二行");
  refresh_default_prompt_row (prompt_row);
  g_assert_cmpstr (adw_action_row_get_subtitle (prompt_row), ==,
                   "默认第一行 默认第二行");
  g_clear_pointer (&app->default_system_prompt, g_free);
  GtkButton *edit = GTK_BUTTON (
      gtk_builder_get_object (builder, "default_system_prompt"));
  g_assert_cmpstr (gtk_button_get_label (edit), ==, "Edit");
  setup_uint_preference (builder, "context_keep_turns",
                         &app->context_keep_turns);
  AdwSpinRow *row = ADW_SPIN_ROW (
      gtk_builder_get_object (builder, "context_keep_turns"));
  g_assert_cmpfloat (adw_spin_row_get_value (row), ==, 2);
  adw_spin_row_set_value (row, 0);
  g_assert_cmpuint (app->context_keep_turns, ==, 0);
  g_assert_cmpuint (load_context_keep_turns (), ==, 0);
  g_assert_true (config_set ("context_keep_turns", NULL));
  g_clear_object (&builder);
  teardown ();
}

static guint
visible_bubble_actions (GtkWidget *bubble)
{
  GtkWidget *actions = g_object_get_data (G_OBJECT (bubble), "actions");
  guint count = 0;
  for (GtkWidget *button = gtk_widget_get_first_child (actions);
       button != NULL; button = gtk_widget_get_next_sibling (button))
    {
      if (gtk_widget_get_visible (button))
        count++;
    }
  return count;
}

static void
context_marker (void)
{
  if (!setup_ui ())
    return;
  add_message ("user", "question two");
  add_message ("assistant", "answer two");
  add_message ("user", "question three");
  add_message ("assistant", "answer three");
  render_chat ();

  GtkWidget *first = gtk_widget_get_first_child (app->messages);
  GtkWidget *before = gtk_widget_get_next_sibling (first);
  GtkWidget *after = gtk_widget_get_next_sibling (before);
  GtkWidget *last = last_message_bubble (app->messages);
  g_assert_null (app->summary_marker);
  g_assert_cmpuint (visible_bubble_actions (first), ==, 4);
  g_assert_cmpuint (visible_bubble_actions (last), ==, 4);
  Compression compression = { .turns = 6, .until = 2 };
  gtk_widget_allocate (app->scroller, 600, 180, -1, NULL);
  g_assert_cmpfloat (stick_bottom (&chat_stick), >, 0);
  gtk_adjustment_set_value (chat_stick.adj, 0);
  chat_stick.follow = FALSE;
  apply_compression (&compression, g_strdup ("Saved facts.\nMore details."));

  GtkWidget *marker = app->summary_marker;
  g_assert_true (GTK_IS_BOX (marker));
  g_assert_cmpuint (app->chat->summary_until, ==, 2);
  g_assert_cmpuint (app->chat->summary_position, ==, 6);
  g_assert_cmpuint (visible_bubble_actions (first), ==, 2);
  g_assert_cmpuint (visible_bubble_actions (last), ==, 2);
  g_assert_true (gtk_widget_get_next_sibling (before) == after);
  g_assert_true (gtk_widget_get_next_sibling (last) == marker);
  g_assert_null (gtk_widget_get_next_sibling (marker));
  g_assert_true (chat_stick.follow);
  gtk_widget_allocate (app->scroller, 600, 180, -1, NULL);
  while (g_main_context_iteration (NULL, FALSE))
    {
    }
  GtkWidget *viewport = gtk_scrolled_window_get_child (
      GTK_SCROLLED_WINDOW (app->scroller));
  GtkWidget *content = gtk_viewport_get_child (GTK_VIEWPORT (viewport));
  gdouble offset = gtk_adjustment_get_value (chat_stick.adj);
  g_assert_cmpfloat (offset, ==, stick_bottom (&chat_stick));
  graphene_rect_t bounds;
  g_assert_true (gtk_widget_compute_bounds (marker, content, &bounds));
  g_assert_cmpfloat (bounds.origin.y - offset, >=, 0);
  g_assert_cmpfloat (bounds.origin.y - offset, <,
                     gtk_widget_get_height (viewport));
  g_assert_cmpuint (bubble_index (after), ==, 2);
  GtkListBox *nav = GTK_LIST_BOX (app->message_nav_list);
  GtkListBoxRow *row = gtk_list_box_get_row_at_index (nav, 1);
  g_assert_true (g_object_get_data (G_OBJECT (row), "bubble") == after);
  g_assert_null (gtk_list_box_get_row_at_index (nav, 3));

  on_edit_message (NULL, first);
  g_assert_cmpuint (app->chat->edit_from, ==, G_MAXUINT);
  regenerate ();
  app->chat->edit_from = 0;
  send_message ();
  g_assert_cmpuint (app->chat->edit_from, ==, 0);
  app->chat->edit_from = G_MAXUINT;
  g_assert_null (adw_application_window_get_visible_dialog (
      ADW_APPLICATION_WINDOW (app->window)));
  g_assert_cmpuint (json_array_get_length (app->chat->messages), ==, 6);

  GtkWidget *toggle = gtk_widget_get_first_child (marker);
  g_assert_true (GTK_IS_BUTTON (toggle));
  g_assert_null (gtk_widget_get_tooltip_text (toggle));
  GtkWidget *header = gtk_button_get_child (GTK_BUTTON (toggle));
  GtkWidget *title = gtk_widget_get_first_child (header);
  g_assert_true (GTK_IS_LABEL (title));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (title)), ==,
                   "Context compressed · 2 messages");
  GtkWidget *chevron = gtk_widget_get_next_sibling (title);
  g_assert_true (GTK_IS_IMAGE (chevron));
  g_assert_null (gtk_widget_get_next_sibling (chevron));
  g_assert_cmpstr (gtk_image_get_icon_name (GTK_IMAGE (chevron)), ==,
                   "pan-end-symbolic");
  GtkWidget *text_view = gtk_widget_get_next_sibling (toggle);
  g_assert_false (gtk_widget_get_visible (text_view));
  g_signal_emit_by_name (toggle, "clicked");
  g_assert_true (gtk_widget_get_visible (text_view));
  g_assert_cmpstr (gtk_image_get_icon_name (GTK_IMAGE (chevron)), ==,
                   "pan-down-symbolic");
  GtkTextView *view = GTK_TEXT_VIEW (text_view);
  g_assert_null (gtk_widget_get_next_sibling (text_view));
  g_assert_false (gtk_text_view_get_editable (view));
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (view);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds (buffer, &start, &end);
  g_autofree gchar *text = gtk_text_buffer_get_text (buffer, &start, &end,
                                                     FALSE);
  g_assert_cmpstr (text, ==, "Saved facts.\nMore details.");
  refresh_message_fonts ();
  g_signal_emit_by_name (toggle, "clicked");
  g_assert_false (gtk_widget_get_visible (text_view));
  g_assert_cmpstr (gtk_image_get_icon_name (GTK_IMAGE (chevron)), ==,
                   "pan-end-symbolic");

  add_message ("user", "question four");
  render_chat ();
  marker = app->summary_marker;
  GtkWidget *anchor = gtk_widget_get_prev_sibling (marker);
  g_assert_cmpuint (bubble_index (anchor), ==, 5);
  g_assert_cmpuint (bubble_index (gtk_widget_get_next_sibling (marker)), ==, 6);

  on_delete_message (NULL, anchor);
  g_assert_cmpuint (app->chat->summary_until, ==, 2);
  g_assert_cmpuint (app->chat->summary_position, ==, 6);
  g_assert_true (app->summary_marker == marker);
  g_assert_cmpuint (json_array_get_length (app->chat->messages), ==, 7);

  on_delete_message (NULL, gtk_widget_get_next_sibling (marker));
  g_assert_cmpuint (json_array_get_length (app->chat->messages), ==, 6);
  g_assert_cmpuint (app->chat->summary_position, ==, 6);
  g_assert_true (app->summary_marker ==
                 gtk_widget_get_last_child (app->messages));

  Chat *source = app->chat;
  on_fork_message (NULL, last_message_bubble (app->messages));
  g_assert_true (app->chat != source);
  g_assert_cmpuint (app->chat->summary_position, ==, 6);
  g_assert_cmpuint (source->summary_position, ==, 6);
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

  ContextPrefix prefix = { 0 };
  g_autofree gchar *compaction = summary_request_body (prefix, 2, &len, &error);
  g_assert_no_error (error);
  g_autoptr (JsonObject) summary = parse_response (compaction, len);
  g_assert_cmpstr (member_string (summary, "model"), ==, "deepseek-flash");
  messages = member_array (summary, "messages");
  parts = member_array (json_array_get_object_element (messages, 1),
                        "content");
  g_assert_cmpuint (json_array_get_length (parts), ==, 4);
  g_assert_cmpstr (member_string (json_array_get_object_element (parts, 1),
                                  "type"),
                   ==, "image_url");
  set_summary (app->chat, g_strdup ("Red and green squares"), 2, 2);
  g_autofree gchar *summarized = request_body (2, NULL, FALSE, &len, &error);
  g_assert_no_error (error);
  g_assert_nonnull (strstr (summarized, "Red and green squares"));
  g_assert_null (strstr (summarized, "data:image"));
  g_assert_nonnull (image_array_member (msg, "images"));

  g_free (app->model);
  app->model = g_strdup ("deepseek-flash");
  g_assert_cmpint (g_unlink (path), ==, 0);
  g_autofree gchar *missing = request_body (1, NULL, FALSE, &len, &error);
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
  source->system_prompt = g_strdup ("Keep responses short.");
  set_summary (source, g_strdup ("Earlier conversation"), 2, 2);
  g_free (source->draft);
  source->draft = g_strdup ("original draft");
  render_chat ();

  GtkListBox *nav = GTK_LIST_BOX (app->message_nav_list);
  GtkListBoxRow *row = gtk_list_box_get_row_at_index (nav, 1);
  GtkWidget *next = g_object_get_data (G_OBJECT (row), "bubble");
  on_edit_message (NULL, next);
  g_assert_cmpuint (source->edit_from, ==, 2);
  g_assert_cmpstr (source->edit_text, ==, "question two");
  g_assert_cmpstr (source->draft, ==, "original draft");
  g_assert_nonnull (app->summary_marker);
  g_assert_true (gtk_widget_get_last_child (app->messages) ==
                 app->summary_marker);
  GtkWidget *last = last_message_bubble (app->messages);
  g_assert_cmpuint (bubble_index (last), ==, 1);

  regenerate ();
  g_assert_cmpuint (json_array_get_length (source->messages), ==, 4);
  g_assert_cmpuint (source->edit_from, ==, 2);

  GtkWidget *first = gtk_widget_get_first_child (app->messages);
  on_delete_message (NULL, first);
  g_assert_cmpuint (source->edit_from, ==, 2);
  g_assert_cmpuint (json_array_get_length (source->messages), ==, 4);
  g_assert_nonnull (source->summary);
  g_assert_nonnull (app->summary_marker);

  g_autoptr (JsonObject) edited = new_user_message ("edited question two");
  g_assert_true (commit_message (edited));
  g_assert_cmpuint (json_array_get_length (source->messages), ==, 3);
  g_assert_cmpstr (
      json_object_get_string_member (
          json_array_get_object_element (source->messages, 2), "content"),
      ==, "edited question two");
  g_assert_cmpuint (source->edit_from, ==, G_MAXUINT);

  render_chat ();
  on_fork_message (NULL, last_message_bubble (app->messages));
  Chat *fork = app->chat;
  g_assert_true (fork != source);
  g_assert_cmpuint (app->chats->len, ==, 2);
  g_assert_cmpuint (json_array_get_length (fork->messages), ==, 2);
  g_assert_cmpstr (fork->draft, ==, "edited question two");
  g_assert_cmpstr (fork->system_prompt, ==, "Keep responses short.");

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
  set_summary (app->chat, g_strdup ("Earlier conversation"), 2, 2);
  render_chat ();
  /* Allocate the viewport without waiting for a native compositor frame. */
  gtk_widget_allocate (app->scroller, 600, 450, -1, NULL);
  while (g_main_context_iteration (NULL, FALSE))
    {
    }
  g_assert_cmpfloat (stick_bottom (&chat_stick), >, 0);
  g_assert_cmpfloat (gtk_adjustment_get_value (chat_stick.adj), ==,
                     stick_bottom (&chat_stick));

  gtk_editable_set_text (GTK_EDITABLE (app->message_nav_search), "jump target");
  g_signal_emit_by_name (app->message_nav_search, "activate");
  g_assert_false (chat_stick.follow);
  GtkWidget *marker = app->summary_marker;
  g_assert_true (GTK_IS_BOX (marker));
  GtkWidget *bubble = gtk_widget_get_next_sibling (marker);
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

static gboolean
widget_contains_label (GtkWidget   *widget,
                       const gchar *text)
{
  if (GTK_IS_LABEL (widget) &&
      g_strcmp0 (gtk_label_get_text (GTK_LABEL (widget)), text) == 0)
    return TRUE;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child != NULL;
       child = gtk_widget_get_next_sibling (child))
    if (widget_contains_label (child, text))
      return TRUE;
  return FALSE;
}

static void
deferred_window_toasts (void)
{
  if (!setup_ui ())
    return;
  AdwDialog *settings = adw_preferences_dialog_new ();
  adw_dialog_present (settings, app->window);
  AdwDialog *alert = adw_alert_dialog_new ("Alert", "Details");
  adw_dialog_present (alert, app->window);

  app_toast ("Background notice");
  app_toast_retry ("Background failure");
  g_assert_cmpuint (g_queue_get_length (&app->pending_toasts), ==, 2);
  g_assert_false (widget_contains_label (app->toasts, "Background notice"));
  AdwToast *retry = g_queue_peek_nth (&app->pending_toasts, 1);
  g_assert_cmpstr (adw_toast_get_button_label (retry), ==, "Retry");
  g_assert_cmpstr (adw_toast_get_action_name (retry), ==, "win.retry");
  g_assert_cmpstr (g_variant_get_string (
                       adw_toast_get_action_target_value (retry), NULL),
                   ==, app->chat->id);
  adw_dialog_close (alert);
  g_assert_cmpuint (g_queue_get_length (&app->pending_toasts), ==, 2);
  adw_dialog_close (settings);
  g_assert_true (g_queue_is_empty (&app->pending_toasts));
  g_assert_true (widget_contains_label (app->toasts, "Background notice"));
  teardown ();
}

static void
discard_deferred_toasts_on_close (void)
{
  if (!setup_ui ())
    return;
  AdwDialog *dialog = adw_preferences_dialog_new ();
  adw_dialog_present (dialog, app->window);
  app_toast_retry ("Background failure");
  g_assert_cmpuint (g_queue_get_length (&app->pending_toasts), ==, 1);
  g_assert_false (on_close (NULL, NULL));
  g_assert_true (app->closing);
  g_assert_true (g_queue_is_empty (&app->pending_toasts));
  teardown ();
}

static void
settings_feedback (gconstpointer data)
{
  if (!setup_ui ())
    return;
  guint kind = GPOINTER_TO_UINT (data);
  g_assert_true (config_set ("language", NULL));
  g_autofree gchar *path = store_config_path ();
  g_autofree gchar *contents = NULL;
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_autoptr (GtkBuilder) builder = gtk_builder_new_from_resource (
      "/cc/arthur63/gchat/settings.ui");
  AdwDialog *dialog = ADW_DIALOG (
      gtk_builder_get_object (builder, "settings_dialog"));
  AdwSwitchRow *toggle = ADW_SWITCH_ROW (
      gtk_builder_get_object (builder, "auto_compact"));
  AdwSpinRow *spin = ADW_SPIN_ROW (
      gtk_builder_get_object (builder, "context_keep_turns"));
  AdwEntryRow *key = ADW_ENTRY_ROW (gtk_builder_get_object (builder,
                                                            "key_row"));
  GtkFontDialogButton *font = GTK_FONT_DIALOG_BUTTON (
      gtk_builder_get_object (builder, "message_font"));
  AdwSpinRow *font_size_row = ADW_SPIN_ROW (
      gtk_builder_get_object (builder, "message_font_size"));
  setup_switch_preference (builder, "auto_compact", &app->auto_compact);
  setup_uint_preference (builder, "context_keep_turns",
                         &app->context_keep_turns);
  setup_font_settings (builder, &FONT_SETTINGS[0]);
  adw_dialog_present (dialog, app->window);
  if (kind != 5 && kind != 8)
    g_assert_true (g_file_set_contents (path, "invalid json", -1, NULL));
  const gchar *message = "Failed to save setting";
  if (kind != 5 && kind != 8)
    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*cannot write*");
  if (kind == 0)
    adw_switch_row_set_active (toggle, TRUE);
  else if (kind == 1)
    adw_spin_row_set_value (spin, 3);
  else if (kind == 2)
    {
      message = "Failed to save API key";
      gtk_editable_set_text (GTK_EDITABLE (key), "test-key");
      on_settings_key_apply (key, NULL);
    }
  else if (kind == 3)
    {
      message = "Failed to save font";
      on_font_reset (NULL, font);
    }
  else if (kind == 4)
    {
      GtkButton *edit = GTK_BUTTON (
          gtk_builder_get_object (builder, "default_system_prompt"));
      on_prompt_clicked (edit, GINT_TO_POINTER (TRUE));
      AdwAlertDialog *alert = ADW_ALERT_DIALOG (
          adw_application_window_get_visible_dialog (
              ADW_APPLICATION_WINDOW (app->window)));
      g_assert_true (ADW_IS_ALERT_DIALOG (alert));
      on_prompt_response (alert, "save", GINT_TO_POINTER (TRUE));
      adw_dialog_close (ADW_DIALOG (alert));
    }
  else if (kind == 5)
    {
      message = "API key saved";
      gtk_editable_set_text (GTK_EDITABLE (key), "test-key");
      on_settings_key_apply (key, NULL);
    }
  else if (kind == 6)
    {
      message = "Failed to save font size";
      adw_spin_row_set_value (font_size_row, 17);
    }
  else if (kind == 7)
    {
      message = "Failed to save font";
      g_autoptr (PangoFontDescription) desc =
          pango_font_description_from_string ("gchat test font");
      gtk_font_dialog_button_set_font_desc (font, desc);
    }
  else
    {
      message = "API key cannot be empty";
      on_settings_key_apply (key, NULL);
    }
  if (kind != 5 && kind != 8)
    g_test_assert_expected_messages ();
  g_assert_true (widget_contains_label (GTK_WIDGET (dialog), message));
  g_assert_false (widget_contains_label (app->toasts, message));
  g_assert_true (g_queue_is_empty (&app->pending_toasts));
  if (kind == 0)
    g_assert_false (adw_switch_row_get_active (toggle));
  if (kind == 1)
    g_assert_cmpfloat (adw_spin_row_get_value (spin), ==, 2);
  adw_dialog_close (dialog);
  g_assert_true (g_file_set_contents (path, contents, -1, NULL));
  g_clear_object (&builder);
  teardown ();
}

static void
settings_language_toast (void)
{
  if (!setup_ui ())
    return;
  g_assert_true (config_set ("language", NULL));
  g_autoptr (GtkBuilder) builder = gtk_builder_new_from_resource (
      "/cc/arthur63/gchat/settings.ui");
  AdwDialog *dialog = ADW_DIALOG (
      gtk_builder_get_object (builder, "settings_dialog"));
  AdwComboRow *row = ADW_COMBO_ROW (
      gtk_builder_get_object (builder, "language"));
  setup_combo_preference (builder, "language", LANGUAGES,
                          G_N_ELEMENTS (LANGUAGES),
                          G_CALLBACK (on_language_selected));
  adw_dialog_present (dialog, app->window);
  g_assert_true (gtk_widget_get_ancestor (GTK_WIDGET (row),
                                          ADW_TYPE_PREFERENCES_DIALOG) ==
                 GTK_WIDGET (dialog));
  adw_combo_row_set_selected (row, 1);
  g_autofree gchar *saved = config_get ("language");
  g_assert_cmpstr (saved, ==, "en");

  const gchar *message = "Restart the app to apply language changes";
  g_assert_true (widget_contains_label (GTK_WIDGET (dialog), message));
  g_assert_false (widget_contains_label (app->toasts, message));

  adw_dialog_close (dialog);
  g_clear_object (&builder);

  builder = gtk_builder_new_from_resource ("/cc/arthur63/gchat/settings.ui");
  dialog = ADW_DIALOG (gtk_builder_get_object (builder, "settings_dialog"));
  row = ADW_COMBO_ROW (gtk_builder_get_object (builder, "language"));
  setup_combo_preference (builder, "language", LANGUAGES,
                          G_N_ELEMENTS (LANGUAGES),
                          G_CALLBACK (on_language_selected));
  adw_dialog_present (dialog, app->window);
  g_autofree gchar *path = store_config_path ();
  g_autofree gchar *contents = NULL;
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_true (g_file_set_contents (path, "invalid json", -1, NULL));
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*cannot write*");
  adw_combo_row_set_selected (row, 2);
  g_test_assert_expected_messages ();
  g_assert_cmpuint (adw_combo_row_get_selected (row), ==, 1);
  g_assert_true (widget_contains_label (GTK_WIDGET (dialog),
                                        "Failed to save language"));
  g_assert_false (widget_contains_label (app->toasts,
                                         "Failed to save language"));
  adw_dialog_close (dialog);
  g_assert_true (g_file_set_contents (path, contents, -1, NULL));
  g_assert_true (config_set ("language", NULL));
  g_clear_object (&builder);
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

  g_assert_true (config_set ("context_threshold", NULL));
  g_assert_cmpuint (load_context_threshold (), ==, 64000);
  g_assert_true (config_set ("context_threshold", "32000"));
  g_assert_cmpuint (load_context_threshold (), ==, 32000);
  g_assert_true (config_set ("context_threshold", "1000001"));
  g_assert_cmpuint (load_context_threshold (), ==, 64000);
  g_assert_true (config_set ("context_threshold", NULL));

  g_assert_true (config_set ("context_keep_turns", NULL));
  g_assert_cmpuint (load_context_keep_turns (), ==, 2);
  g_assert_true (config_set ("context_keep_turns", "100"));
  g_assert_cmpuint (load_context_keep_turns (), ==, 100);
  g_assert_true (config_set ("context_keep_turns", "101"));
  g_assert_cmpuint (load_context_keep_turns (), ==, 2);
  g_assert_true (config_set ("context_keep_turns", "-1"));
  g_assert_cmpuint (load_context_keep_turns (), ==, 2);
  g_assert_true (config_set ("context_keep_turns", NULL));

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
  g_test_add_func ("/app/context/payload", context_payload);
  g_test_add_func ("/app/context/compression-rules", compression_rules);
  g_test_add_func ("/app/context/retention", compression_retention);
  g_test_add_func ("/app/context/summary-response", summary_response);
  g_test_add_func ("/app/context/ui", context_ui);
  g_test_add_func ("/app/context/marker", context_marker);
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
  g_test_add_func ("/app/settings/language-toast", settings_language_toast);
  g_test_add_func ("/app/settings/deferred-toasts", deferred_window_toasts);
  g_test_add_func ("/app/settings/discard-deferred-on-close",
                   discard_deferred_toasts_on_close);
  g_test_add_data_func ("/app/settings/switch-failure", GINT_TO_POINTER (0),
                        settings_feedback);
  g_test_add_data_func ("/app/settings/number-failure", GINT_TO_POINTER (1),
                        settings_feedback);
  g_test_add_data_func ("/app/settings/key-failure", GINT_TO_POINTER (2),
                        settings_feedback);
  g_test_add_data_func ("/app/settings/font-failure", GINT_TO_POINTER (3),
                        settings_feedback);
  g_test_add_data_func ("/app/settings/prompt-failure", GINT_TO_POINTER (4),
                        settings_feedback);
  g_test_add_data_func ("/app/settings/key-success", GINT_TO_POINTER (5),
                        settings_feedback);
  g_test_add_data_func ("/app/settings/font-size-failure", GINT_TO_POINTER (6),
                        settings_feedback);
  g_test_add_data_func ("/app/settings/font-change-failure",
                        GINT_TO_POINTER (7), settings_feedback);
  g_test_add_data_func ("/app/settings/empty-key", GINT_TO_POINTER (8),
                        settings_feedback);
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
