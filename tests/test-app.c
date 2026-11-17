/* SPDX-FileCopyrightText: 2026 Arthur <arthur63@163.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Test internal state transitions directly instead of widening the app
   interface for tests. */
#include "config.h"

#define main gchat_main
#include "../main.c"
#undef main

#include <math.h>

static gboolean have_display;
static gchar *test_dir;

/* Replay the macOS notification order with a real IM module; do not touch GTK
   private fields or class methods. */
typedef struct
{
  GtkIMContext parent;
  const gchar *preedit;
  gint         cursor;
} TestIMContext;
typedef GtkIMContextClass TestIMContextClass;
static TestIMContext *test_context;
G_DEFINE_TYPE (TestIMContext, test_im_context, GTK_TYPE_IM_CONTEXT)

static void
test_im_preedit (GtkIMContext   *context,
                 gchar         **text,
                 PangoAttrList **attrs,
                 gint           *cursor)
{
  TestIMContext *test = (TestIMContext *) context;
  if (text != NULL)
    *text = g_strdup (test->preedit);
  if (attrs != NULL)
    *attrs = pango_attr_list_new ();
  if (cursor != NULL)
    *cursor = test->cursor;
}

static void
test_im_context_class_init (TestIMContextClass *klass)
{
  klass->get_preedit_string = test_im_preedit;
}

static void
test_im_context_init (TestIMContext *context)
{
  context->preedit = "";
  test_context = context;
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
  app->answer = g_string_new (NULL);
  app->reasoning = g_string_new (NULL);
  app->model = g_strdup ("deepseek-flash");
  app->effort = g_strdup ("high");
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
  g_clear_object (&app->session);
  g_ptr_array_unref (app->chats);
  g_string_free (app->answer, TRUE);
  g_string_free (app->reasoning, TRUE);
  g_free (app->model);
  g_free (app->effort);
  g_free (app->api_key);
  g_free (app);
  app = NULL;
  chat_stick = (Stick){ 0 };
  input_stick = (Stick){ 0 };
  g_autofree gchar *path = store_session_path ();
  g_autofree gchar *bak = g_strconcat (path, ".bak", NULL);
  g_unlink (path);
  g_unlink (bak);
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

typedef struct
{
  GMainLoop *loop;
  guint      frames;
} FrameWait;

static gboolean
layout_frame (GtkWidget     *widget G_GNUC_UNUSED,
              GdkFrameClock *clock G_GNUC_UNUSED,
              gpointer       data)
{
  FrameWait *wait = data;
  if (--wait->frames)
    return G_SOURCE_CONTINUE;
  g_main_loop_quit (wait->loop);
  return G_SOURCE_REMOVE;
}

static gboolean
layout_timeout (gpointer data)
{
  g_main_loop_quit (data);
  return G_SOURCE_REMOVE;
}

static void
wait_for_layout (void)
{
  g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  FrameWait wait = { .loop = loop, .frames = 3 };
  guint tick =
      gtk_widget_add_tick_callback (app->window, layout_frame, &wait, NULL);
  guint timeout = g_timeout_add (2000, layout_timeout, wait.loop);
  g_main_loop_run (wait.loop);
  if (g_main_context_find_source_by_id (NULL, timeout) != NULL)
    g_source_remove (timeout);
  if (wait.frames)
    gtk_widget_remove_tick_callback (app->window, tick);
  g_assert_cmpuint (wait.frames, ==, 0);
}

static void
show_window (void)
{
  gtk_window_set_default_size (GTK_WINDOW (app->window), 940, 800);
  gtk_window_set_title (GTK_WINDOW (app->window), "gchat 布局测试");
  fit_input_height ();
  gtk_window_present (GTK_WINDOW (app->window));
  wait_for_layout ();
}

static void
assert_user_bubble_layout (const gchar *text)
{
  GtkWidget *bubble = gtk_widget_get_first_child (app->messages);
  GtkWidget *body = g_object_get_data (G_OBJECT (bubble), "body");
  GtkWidget *view = gtk_widget_get_first_child (body);
  g_assert_true (GTK_IS_TEXT_VIEW (view));
  g_autoptr (PangoLayout) layout =
      gtk_widget_create_pango_layout (view, text);
  int text_width, requested_width;
  pango_layout_get_pixel_size (layout, &text_width, NULL);
  gtk_widget_get_size_request (body, &requested_width, NULL);
  g_assert_cmpint (requested_width, ==, MIN (text_width + 2, 520));
  int expected_lines = pango_layout_get_line_count (layout);

  g_assert_true (gtk_widget_get_realized (view));
  g_assert_true (gtk_widget_get_mapped (view));
  g_assert_cmpint (gtk_text_view_get_pixels_below_lines (GTK_TEXT_VIEW (view)),
                   ==, 0);
  GtkTextIter cursor;
  gtk_text_buffer_get_start_iter (
      gtk_text_view_get_buffer (GTK_TEXT_VIEW (view)), &cursor);
  int display_lines = 1;
  while (gtk_text_view_forward_display_line (GTK_TEXT_VIEW (view), &cursor))
    display_lines++;
  if (text_width <= 520)
    g_assert_cmpint (display_lines, ==, expected_lines);
  else
    g_assert_cmpint (display_lines, >, 1);
  GtkWidget *surface = gtk_widget_get_parent (body);
  graphene_rect_t surface_bounds, body_bounds, view_bounds, actions_bounds;
  g_assert_true (gtk_widget_compute_bounds (surface, bubble, &surface_bounds));
  g_assert_true (gtk_widget_compute_bounds (body, bubble, &body_bounds));
  g_assert_true (gtk_widget_compute_bounds (view, bubble, &view_bounds));
  g_assert_true (gtk_widget_compute_bounds (
      g_object_get_data (G_OBJECT (bubble), "actions"), bubble,
      &actions_bounds));
  g_test_message (
      "bubble %s: surface %.0fx%.0f, body %.0fx%.0f, view %.0fx%.0f", text,
      surface_bounds.size.width, surface_bounds.size.height,
      body_bounds.size.width, body_bounds.size.height, view_bounds.size.width,
      view_bounds.size.height);
  /* The action bar can be wider than a short message but must not grow the
     fixed bubble padding. */
  g_assert_cmpfloat (body_bounds.origin.x - surface_bounds.origin.x, ==, 16);
  g_assert_cmpfloat (surface_bounds.origin.x + surface_bounds.size.width -
                         body_bounds.origin.x - body_bounds.size.width,
                     ==, 16);
  g_assert_cmpfloat (body_bounds.origin.y - surface_bounds.origin.y, ==, 10);
  g_assert_cmpfloat (surface_bounds.origin.y + surface_bounds.size.height -
                         body_bounds.origin.y - body_bounds.size.height,
                     ==, 10);
  g_assert_cmpint (gtk_widget_get_width (body), ==, requested_width);
  g_assert_cmpfloat (surface_bounds.origin.x + surface_bounds.size.width, ==,
                     actions_bounds.origin.x + actions_bounds.size.width);
  if (expected_lines == 1 && text_width <= 520)
    {
      GtkTextIter start;
      gtk_text_buffer_get_start_iter (
          gtk_text_view_get_buffer (GTK_TEXT_VIEW (view)), &start);
      GdkRectangle first;
      gtk_text_view_get_iter_location (GTK_TEXT_VIEW (view), &start, &first);
      /* Use the logical width of the whole run for mixed fonts, which avoids
         the rounding error of per-character cursor rectangles. */
      gdouble text_x = view_bounds.origin.x + first.x + text_width / 2.0;
      gdouble text_y = view_bounds.origin.y + first.y + first.height / 2.0;
      g_assert_cmpfloat (fabs (text_y - surface_bounds.origin.y -
                               surface_bounds.size.height / 2),
                         <=, 1);
      g_assert_cmpfloat (fabs (text_x - surface_bounds.origin.x -
                               surface_bounds.size.width / 2),
                         <=, 1);
    }
}

static void
user_bubble_layout (gconstpointer data)
{
  if (!setup_ui ())
    return;
  const gchar *text = data;
  JsonObject *user = json_array_get_object_element (app->chat->messages, 0);
  json_object_set_string_member (user, "content", text);
  render_history (1);
  show_window ();
  assert_user_bubble_layout (text);

  /* Regenerate rebuilds the user message in a mapped message area, then inserts
     a reply placeholder. */
  clear_messages ();
  render_history (1);
  GtkWidget *reply = bubble_new ("assistant");
  GtkWidget *body = g_object_get_data (G_OBJECT (reply), "body");
  gtk_box_append (GTK_BOX (body), gtk_label_new ("…"));
  wait_for_layout ();
  assert_user_bubble_layout (text);
  teardown ();
}

static void
code_block_layout (gconstpointer data)
{
  if (!setup_ui ())
    return;
  apply_theme (GPOINTER_TO_INT (data) ? "dark" : "light");
  static const gchar *codes[] = {
    "if (arr[j] > arr[j + 1])",
    "if (arr[j] < arr[j + 1]) {\n    int temp = arr[j];\n    arr[j] = arr[j + "
    "1];\n    arr[j + 1] = temp;\n}",
    "const char *long_line = \"this deliberately long code line must wrap "
    "inside the card without clipping the final display line or leaving an "
    "extra empty line at the bottom\";",
  };
  for (guint i = 0; i < G_N_ELEMENTS (codes); i++)
    {
      g_autofree gchar *markdown =
          g_strdup_printf ("```c\n%s\n```\n", codes[i]);
      JsonObject *msg = json_array_get_object_element (app->chat->messages, 1);
      json_object_set_string_member (msg, "content", markdown);
      render_chat ();
      GtkWidget *bubble = gtk_widget_get_last_child (app->messages);
      GtkWidget *body = g_object_get_data (G_OBJECT (bubble), "body");
      GtkWidget *card = gtk_widget_get_first_child (body);
      g_assert_true (gtk_widget_has_css_class (card, "code-card"));
      GtkWidget *view = gtk_widget_get_last_child (card);
      g_assert_true (GTK_IS_TEXT_VIEW (view));
      show_window ();
      wait_for_layout ();
      g_assert_true (gtk_widget_get_mapped (view));
      GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
      GtkTextIter end;
      gtk_text_buffer_get_end_iter (buffer, &end);
      gtk_text_iter_backward_char (&end);
      GdkRectangle glyph;
      gtk_text_view_get_iter_location (GTK_TEXT_VIEW (view), &end, &glyph);
      graphene_rect_t bounds;
      g_assert_true (gtk_widget_compute_bounds (view, card, &bounds));
      gdouble bottom_gap = gtk_widget_get_height (card) - bounds.origin.y -
                           glyph.y - glyph.height;
      g_test_message ("code case %u: bottom gap %.1f px", i, bottom_gap);
      g_assert_cmpfloat (bottom_gap, >=, 8);
      g_assert_cmpfloat (bottom_gap, <=, 12);
      g_assert_cmpint (glyph.y + glyph.height, <=,
                       gtk_widget_get_height (view));
    }
  apply_theme ("system");
  teardown ();
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
      gtk_box_prepend (GTK_BOX (app->reply_body), app->reason_box);
    }
}

static void
action_visibility (void)
{
  if (!setup_ui ())
    return;
  render_chat ();
  show_window ();
  GtkWidget *first = gtk_widget_get_first_child (app->messages);
  GtkWidget *last = gtk_widget_get_last_child (app->messages);
  GtkWidget *first_actions = g_object_get_data (G_OBJECT (first), "actions");
  GtkWidget *last_actions = g_object_get_data (G_OBJECT (last), "actions");
  GtkWidget *body = g_object_get_data (G_OBJECT (first), "body");
  GtkWidget *text = gtk_widget_get_first_child (body);
  GtkEventController *motion = g_object_get_data (G_OBJECT (first), "motion");

  g_signal_emit_by_name (motion, "leave");
  gtk_widget_grab_focus (app->input);
  g_assert_cmpfloat (gtk_widget_get_opacity (first_actions), ==, 0);
  g_assert_false (gtk_widget_get_can_target (first_actions));
  g_assert_cmpfloat (gtk_widget_get_opacity (last_actions), ==, 1);
  g_assert_true (gtk_widget_get_can_target (last_actions));
  g_signal_emit_by_name (motion, "enter", 0.0, 0.0);
  g_assert_cmpfloat (gtk_widget_get_opacity (first_actions), ==, 1);
  g_assert_true (gtk_widget_get_can_target (first_actions));
  g_signal_emit_by_name (motion, "leave");
  g_assert_cmpfloat (gtk_widget_get_opacity (first_actions), ==, 0);
  g_assert_false (gtk_widget_get_can_target (first_actions));
  g_assert_true (gtk_widget_grab_focus (text));
  g_signal_emit_by_name (motion, "enter", 0.0, 0.0);
  g_signal_emit_by_name (motion, "leave");
  g_assert_cmpfloat (gtk_widget_get_opacity (first_actions), ==, 1);
  g_assert_true (gtk_widget_get_can_target (first_actions));
  GtkWidget *button = gtk_widget_get_first_child (first_actions);
  g_assert_true (gtk_widget_grab_focus (button));
  g_assert_cmpfloat (gtk_widget_get_opacity (first_actions), ==, 1);
  gtk_widget_grab_focus (app->input);
  g_assert_cmpfloat (gtk_widget_get_opacity (first_actions), ==, 0);
  g_assert_false (gtk_widget_get_can_target (first_actions));
  g_assert_cmpfloat (gtk_widget_get_opacity (last_actions), ==, 1);
  g_assert_true (gtk_widget_grab_focus (button));
  g_assert_cmpfloat (gtk_widget_get_opacity (first_actions), ==, 1);
  g_signal_emit_by_name (motion, "enter", 0.0, 0.0);
  gtk_widget_grab_focus (app->input);
  g_assert_cmpfloat (gtk_widget_get_opacity (first_actions), ==, 1);
  g_signal_emit_by_name (motion, "leave");
  g_assert_cmpfloat (gtk_widget_get_opacity (first_actions), ==, 0);

  body = g_object_get_data (G_OBJECT (last), "body");
  text = gtk_widget_get_first_child (body);
  g_assert_true (gtk_widget_grab_focus (text));
  begin_reply (FALSE);
  GtkWidget *reply_actions =
      g_object_get_data (G_OBJECT (app->reply_bubble), "actions");
  g_assert_cmpfloat (gtk_widget_get_opacity (last_actions), ==, 1);
  g_assert_cmpfloat (gtk_widget_get_opacity (reply_actions), ==, 1);
  gtk_widget_grab_focus (app->input);
  g_assert_cmpfloat (gtk_widget_get_opacity (last_actions), ==, 0);
  drop_reply ();
  g_assert_cmpfloat (gtk_widget_get_opacity (last_actions), ==, 1);
  on_delete_message (NULL, last);
  first = gtk_widget_get_first_child (app->messages);
  first_actions = g_object_get_data (G_OBJECT (first), "actions");
  g_assert_cmpfloat (gtk_widget_get_opacity (first_actions), ==, 1);
  teardown ();
}

static void
sse_parser (void)
{
  g_autofree gchar *content = NULL;
  g_autofree gchar *reasoning = NULL;

  g_assert_false (sse_parse ("data: [DONE]", &content, &reasoning));
  g_assert_false (sse_parse (": keep-alive", &content, &reasoning));
  g_assert_false (sse_parse ("data: not json", &content, &reasoning));
  g_assert_false (sse_parse ("data: {\"choices\":[]}", &content, &reasoning));

  g_assert_true (
      sse_parse ("data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}",
                 &content, &reasoning));
  g_assert_cmpstr (content, ==, "hi");
  g_assert_null (reasoning);
  g_clear_pointer (&content, g_free);

  g_assert_true (sse_parse ("data: "
                            "{\"choices\":[{\"delta\":{\"reasoning_content\":"
                            "\"think\",\"content\":\"a\"}}]}",
                            &content, &reasoning));
  g_assert_cmpstr (content, ==, "a");
  g_assert_cmpstr (reasoning, ==, "think");
  g_clear_pointer (&content, g_free);
  g_clear_pointer (&reasoning, g_free);

  static const gchar *invalid[] = {
    "data: null",
    "data: []",
    "data: {}",
    "data: {\"choices\":null}",
    "data: {\"choices\":{}}",
    "data: {\"choices\":[null]}",
    "data: {\"choices\":[{\"delta\":null}]}",
    "data: {\"choices\":[{\"delta\":42}]}",
  };
  for (guint i = 0; i < G_N_ELEMENTS (invalid); i++)
    {
      g_assert_false (sse_parse (invalid[i], &content, &reasoning));
      g_assert_null (content);
      g_assert_null (reasoning);
    }
  g_assert_true (
      sse_parse ("data:{\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\r",
                 &content, &reasoning));
  g_assert_cmpstr (content, ==, "hi");
  g_clear_pointer (&content, g_free);
  g_assert_true (
      sse_parse ("data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}",
                 &content, &reasoning));
  g_assert_null (content);
  g_assert_null (reasoning);
}

static void
choice_lookup (void)
{
  const Choice *model =
      find_choice (MODELS, G_N_ELEMENTS (MODELS), "deepseek-v4-pro");
  const Choice *effort = find_choice (EFFORTS, G_N_ELEMENTS (EFFORTS), "none");
  g_assert_nonnull (model);
  g_assert_nonnull (effort);
  g_assert_cmpstr (model->label, ==, "DeepSeek-V4-Pro");
  g_assert_cmpstr (_ (effort->label), ==, _ ("Off"));
  g_assert_nonnull (
      find_choice (MODELS, G_N_ELEMENTS (MODELS), "deepseek-flash"));
  g_assert_null (
      find_choice (MODELS, G_N_ELEMENTS (MODELS), "unsupported-model"));
  g_assert_null (find_choice (EFFORTS, G_N_ELEMENTS (EFFORTS), NULL));
}

static void
choice_selection (void)
{
  if (!setup_ui ())
    return;
  on_model_choice (NULL, (gpointer) MODELS[1].value);
  on_effort_choice (NULL, (gpointer) EFFORTS[0].value);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (app->chip_model)), ==,
                   MODELS[1].label);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (app->chip_effort)), ==,
                   _ (EFFORTS[0].label));
  g_autofree gchar *path = store_session_path ();
  g_autofree gchar *current = NULL;
  g_autofree gchar *model = NULL;
  g_autofree gchar *effort = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) loaded =
      store_load (path, &current, &model, &effort, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (model, ==, MODELS[1].value);
  g_assert_cmpstr (effort, ==, EFFORTS[0].value);
  teardown ();
}

static void
api_key_prompt_path (void)
{
  if (!setup_ui ())
    return;
  show_window ();
  prompt_api_key ();
  wait_for_layout ();
  AdwDialog *dialog = adw_application_window_get_visible_dialog (
      ADW_APPLICATION_WINDOW (app->window));
  g_assert_true (ADW_IS_ALERT_DIALOG (dialog));
  g_autofree gchar *path = store_config_path ();
  g_autofree gchar *body =
      g_strdup_printf (_ ("Your key is saved in %s for future use."), path);
  g_assert_cmpstr (adw_alert_dialog_get_body (ADW_ALERT_DIALOG (dialog)), ==,
                   body);
  teardown ();
}

static void
api_key_priority (gconstpointer data)
{
  const gchar *env = data;
  g_autofree gchar *previous_env = g_strdup (g_getenv ("DEEPSEEK_API_KEY"));
  if (env != NULL)
    g_setenv ("DEEPSEEK_API_KEY", env, TRUE);
  else
    g_unsetenv ("DEEPSEEK_API_KEY");
  setup ();
  app->api_key = g_strdup ("old-key");
  g_assert_false (save_api_key (""));
  g_assert_cmpstr (app->api_key, ==, "old-key");
  g_assert_true (save_api_key ("local-key"));
  g_assert_cmpstr (app->api_key, ==, env != NULL && *env ? env : "local-key");
  g_autofree gchar *saved = config_get ("api_key");
  g_assert_cmpstr (saved, ==, "local-key");
  g_clear_pointer (&saved, g_free);

  g_autofree gchar *path =
      g_build_filename (test_dir, "gchat", "config.json", NULL);
  g_autofree gchar *tmp = g_strconcat (path, ".tmp", NULL);
  g_autofree gchar *bak = g_strconcat (path, ".bak", NULL);
  g_assert_cmpint (g_mkdir (tmp, 0700), ==, 0);
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "cannot write *");
  g_assert_false (save_api_key ("unsaved-key"));
  g_test_assert_expected_messages ();
  g_assert_cmpstr (app->api_key, ==, env != NULL && *env ? env : "local-key");
  saved = config_get ("api_key");
  g_assert_cmpstr (saved, ==, "local-key");
  g_assert_cmpint (g_rmdir (tmp), ==, 0);
  g_unlink (path);
  g_unlink (bak);
  teardown ();
  if (previous_env != NULL)
    g_setenv ("DEEPSEEK_API_KEY", previous_env, TRUE);
  else
    g_unsetenv ("DEEPSEEK_API_KEY");
}

static GtkWidget *
find_label (GtkWidget   *root,
            const gchar *text)
{
  if (GTK_IS_LABEL (root) &&
      g_str_equal (gtk_label_get_text (GTK_LABEL (root)), text))
    {
      return root;
    }
  for (GtkWidget *child = gtk_widget_get_first_child (root); child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_label (child, text);
      if (found != NULL)
        return found;
    }
  return NULL;
}

static void
sidebar_builder (void)
{
  if (!setup_ui ())
    return;

  GtkWidget *sidebar = adw_overlay_split_view_get_sidebar (
      ADW_OVERLAY_SPLIT_VIEW (app->split));
  g_assert_nonnull (find_label (sidebar, _ ("New chat")));
  g_assert_nonnull (find_label (sidebar, _ ("Settings")));
  g_assert_cmpstr (gtk_widget_get_tooltip_text (app->new_btn), ==,
                   _ ("New chat (Ctrl+N)"));
  g_assert_cmpstr (gtk_widget_get_tooltip_text (app->search), ==,
                   _ ("Search conversations (Ctrl/Cmd+F)"));
  g_autofree gchar *placeholder = NULL;
  g_object_get (app->search, "placeholder-text", &placeholder, NULL);
  g_assert_cmpstr (placeholder, ==, _ ("Search conversations"));
  g_assert_true (GTK_IS_LIST_BOX (app->sidebar_list));
  teardown ();
}

static void
content_builder (void)
{
  if (!setup_ui ())
    return;

  g_assert_true (adw_overlay_split_view_get_pin_sidebar (
      ADW_OVERLAY_SPLIT_VIEW (app->split)));
  double max_width = adw_overlay_split_view_get_max_sidebar_width (
      ADW_OVERLAY_SPLIT_VIEW (app->split));
  g_assert_cmpfloat (max_width, ==, 280);
  GtkWidget *chat =
      gtk_stack_get_child_by_name (GTK_STACK (app->stack), "chat");
  GtkWidget *empty =
      gtk_stack_get_child_by_name (GTK_STACK (app->stack), "empty");
  g_assert_true (GTK_IS_OVERLAY (chat));
  g_assert_true (ADW_IS_STATUS_PAGE (empty));
  g_assert_cmpstr (adw_status_page_get_title (ADW_STATUS_PAGE (empty)), ==,
                   _ ("Start a conversation with DeepSeek"));
  g_assert_true (gtk_scrollbar_get_adjustment (GTK_SCROLLBAR (app->chat_bar)) ==
                 gtk_scrolled_window_get_vadjustment (
                     GTK_SCROLLED_WINDOW (app->scroller)));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (app->placeholder)), ==,
                   _ ("Message DeepSeek"));
  g_assert_false (gtk_widget_get_visible (app->edit_bar));
  GtkPopover *popover =
      gtk_menu_button_get_popover (GTK_MENU_BUTTON (app->model_chip));
  g_assert_true (popover == GTK_POPOVER (app->choice_popover));
  teardown ();
}

static void
language_settings (void)
{
  if (!setup_ui ())
    return;
  g_assert_true (config_set ("api_key", "test-key"));
  g_assert_true (config_set ("theme", "dark"));
  render_history (2);
  rebuild_sidebar ();
  gtk_stack_set_visible_child_name (GTK_STACK (app->stack), "chat");
  show_window ();
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (app->input));
  const gchar *draft = "正在编辑的草稿\nDraft";
  gtk_text_buffer_set_text (buffer, draft, -1);
  app->chat->edit_from = 0;
  set_busy (TRUE);
  refresh_input ();
  wait_for_layout ();
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (app->placeholder)), ==,
                   _ ("Message DeepSeek"));
  g_assert_cmpstr (gtk_widget_get_tooltip_text (app->send), ==,
                   _ ("Stop generating (Ctrl/Cmd+.)"));
  g_assert_nonnull (find_label (
      adw_overlay_split_view_get_sidebar (ADW_OVERLAY_SPLIT_VIEW (app->split)),
      _ ("New chat")));

  on_settings_clicked (NULL, NULL);
  wait_for_layout ();
  AdwDialog *dialog = adw_application_window_get_visible_dialog (
      ADW_APPLICATION_WINDOW (app->window));
  g_assert_nonnull (dialog);
  g_assert_cmpstr (adw_dialog_get_title (dialog), ==, _ ("Settings"));
  g_assert_nonnull (
      find_label (GTK_WIDGET (dialog), GCHAT_VERSION " · GTK 4 + libadwaita"));
  const gchar *env_key = g_getenv ("DEEPSEEK_API_KEY");
  if (env_key == NULL || !*env_key)
    {
      g_autofree gchar *path = store_config_path ();
      g_autofree gchar *source =
          g_strdup_printf (_ ("Local configuration file: %s"), path);
      g_assert_nonnull (find_label (GTK_WIDGET (dialog), source));
    }
  GtkWidget *title = find_label (GTK_WIDGET (dialog), _ ("Language"));
  g_assert_nonnull (title);
  g_assert_true (gtk_widget_get_mapped (title));
  AdwComboRow *row =
      ADW_COMBO_ROW (gtk_widget_get_ancestor (title, ADW_TYPE_COMBO_ROW));
  g_assert_nonnull (row);
  GtkStringList *choices = GTK_STRING_LIST (adw_combo_row_get_model (row));
  g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (choices)), ==, 3);
  g_assert_cmpstr (gtk_string_list_get_string (choices, 0), ==,
                   _ ("System language"));
  g_assert_cmpstr (gtk_string_list_get_string (choices, 1), ==, "English");
  g_assert_cmpstr (gtk_string_list_get_string (choices, 2), ==, "简体中文");
  g_assert_cmpuint (adw_combo_row_get_selected (row), ==, 0);
  g_assert_cmpstr (adw_action_row_get_subtitle (ADW_ACTION_ROW (row)), ==,
                   _ ("Restart the app to apply language changes."));

  g_autofree gchar *active = g_strdup (_ ("Settings"));
  guint selected = g_strcmp0 (g_getenv ("LANGUAGE"), "zh_CN") == 0 ? 1 : 2;
  g_autofree gchar *path =
      g_build_filename (test_dir, "gchat", "config.json", NULL);
  g_autofree gchar *tmp = g_strconcat (path, ".tmp", NULL);
  g_autofree gchar *bak = g_strconcat (path, ".bak", NULL);
  g_assert_cmpint (g_mkdir (tmp, 0700), ==, 0);
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "cannot write *");
  adw_combo_row_set_selected (row, selected);
  g_test_assert_expected_messages ();
  g_assert_cmpuint (adw_combo_row_get_selected (row), ==, 0);
  g_assert_null (config_get ("language"));
  g_assert_cmpint (g_rmdir (tmp), ==, 0);
  adw_combo_row_set_selected (row, selected);
  g_autofree gchar *language = config_get ("language");
  g_autofree gchar *key = config_get ("api_key");
  g_autofree gchar *theme = config_get ("theme");
  g_assert_cmpstr (language, ==, LANGUAGES[selected].value);
  g_assert_cmpstr (key, ==, "test-key");
  g_assert_cmpstr (theme, ==, "dark");
  g_assert_cmpstr (_ ("Settings"), ==, active);
  g_assert_cmpstr (adw_dialog_get_title (dialog), ==, active);
  g_assert_true (app->busy);
  g_assert_cmpuint (app->chat->edit_from, ==, 0);
  g_assert_cmpstr (app->chat->title, ==, "测试");
  g_assert_cmpuint (json_array_get_length (app->chat->messages), ==, 2);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds (buffer, &start, &end);
  g_autofree gchar *text =
      gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
  g_assert_cmpstr (text, ==, draft);
  g_clear_pointer (&language, g_free);

  g_assert_cmpint (g_mkdir (tmp, 0700), ==, 0);
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "cannot write *");
  adw_combo_row_set_selected (row, 0);
  g_test_assert_expected_messages ();
  g_assert_cmpuint (adw_combo_row_get_selected (row), ==, selected);
  language = config_get ("language");
  g_assert_cmpstr (language, ==, LANGUAGES[selected].value);
  g_assert_cmpint (g_rmdir (tmp), ==, 0);

  adw_dialog_force_close (dialog);
  on_settings_clicked (NULL, NULL);
  wait_for_layout ();
  dialog = adw_application_window_get_visible_dialog (
      ADW_APPLICATION_WINDOW (app->window));
  title = find_label (GTK_WIDGET (dialog), _ ("Language"));
  g_assert_nonnull (title);
  row = ADW_COMBO_ROW (gtk_widget_get_ancestor (title, ADW_TYPE_COMBO_ROW));
  g_assert_cmpuint (adw_combo_row_get_selected (row), ==, selected);
  g_unlink (path);
  g_unlink (bak);
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
  commit_message (text);
  JsonObject *msg = json_array_get_object_element (app->chat->messages, 2);
  g_assert_cmpstr (json_object_get_string_member (msg, "content"), ==, code);
  teardown ();
}

static void
literal_ui_text (void)
{
  if (!setup_ui ())
    return;
  const gchar *text = "HTTP error: <invalid> & details";
  show_window ();
  app_toast (text);
  wait_for_layout ();
  g_assert_nonnull (find_label (app->toasts, text));
  g_autoptr (GtkBuilder) builder = gtk_builder_new_from_resource (
      "/cc/arthur63/gchat/settings.ui");
  AdwActionRow *row = ADW_ACTION_ROW (
      gtk_builder_get_object (builder, "session_path"));
  adw_action_row_set_subtitle (row, "/tmp/<private>&data");
  g_assert_false (
      adw_preferences_row_get_use_markup (ADW_PREFERENCES_ROW (row)));
  g_assert_nonnull (find_label (GTK_WIDGET (row), "/tmp/<private>&data"));
  g_clear_object (&builder);
  teardown ();
}

static void
stale_retry (void)
{
  if (!setup_ui ())
    return;
  json_array_remove_element (app->chat->messages, 1);
  g_autoptr (GVariant) target =
      g_variant_ref_sink (g_variant_new_string ("another-chat"));
  on_retry_action (NULL, target, NULL);
  g_assert_null (adw_application_window_get_visible_dialog (
      ADW_APPLICATION_WINDOW (app->window)));
  g_assert_cmpuint (json_array_get_length (app->chat->messages), ==, 1);
  teardown ();
}

static void
theme_save_failure (void)
{
  if (!setup_ui ())
    return;
  g_assert_true (config_set ("theme", "dark"));
  apply_theme ("dark");
  show_window ();
  on_settings_clicked (NULL, NULL);
  wait_for_layout ();
  AdwDialog *dialog = adw_application_window_get_visible_dialog (
      ADW_APPLICATION_WINDOW (app->window));
  GtkWidget *title = find_label (GTK_WIDGET (dialog), _ ("Theme"));
  g_assert_nonnull (title);
  AdwComboRow *row =
      ADW_COMBO_ROW (gtk_widget_get_ancestor (title, ADW_TYPE_COMBO_ROW));
  g_assert_cmpuint (adw_combo_row_get_selected (row), ==, 2);
  g_autofree gchar *path =
      g_build_filename (test_dir, "gchat", "config.json", NULL);
  g_autofree gchar *tmp = g_strconcat (path, ".tmp", NULL);
  g_autofree gchar *bak = g_strconcat (path, ".bak", NULL);
  g_assert_cmpint (g_mkdir (tmp, 0700), ==, 0);
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "cannot write *");
  adw_combo_row_set_selected (row, 1);
  g_test_assert_expected_messages ();
  g_assert_cmpuint (adw_combo_row_get_selected (row), ==, 2);
  g_assert_cmpint (
      adw_style_manager_get_color_scheme (adw_style_manager_get_default ()), ==,
      ADW_COLOR_SCHEME_FORCE_DARK);
  g_assert_cmpint (g_rmdir (tmp), ==, 0);
  adw_combo_row_set_selected (row, 1);
  g_autofree gchar *saved = config_get ("theme");
  g_assert_cmpstr (saved, ==, "light");
  g_assert_cmpint (
      adw_style_manager_get_color_scheme (adw_style_manager_get_default ()), ==,
      ADW_COLOR_SCHEME_FORCE_LIGHT);
  apply_theme ("system");
  g_unlink (path);
  g_unlink (bak);
  teardown ();
}

static void
corrupt_preference (gconstpointer data)
{
  if (!setup_ui ())
    return;
  gboolean theme = g_str_equal (data, "theme");
  g_assert_true (config_set (data, theme ? "dark" : "en"));
  apply_theme ("dark");
  show_window ();
  on_settings_clicked (NULL, NULL);
  wait_for_layout ();
  AdwDialog *dialog = adw_application_window_get_visible_dialog (
      ADW_APPLICATION_WINDOW (app->window));
  GtkWidget *title =
      find_label (GTK_WIDGET (dialog), theme ? _ ("Theme") : _ ("Language"));
  g_assert_nonnull (title);
  AdwComboRow *row =
      ADW_COMBO_ROW (gtk_widget_get_ancestor (title, ADW_TYPE_COMBO_ROW));
  guint previous = adw_combo_row_get_selected (row);
  g_assert_cmpuint (previous, ==, theme ? 2 : 1);
  g_autofree gchar *path = store_config_path ();
  g_assert_true (g_file_set_contents (path, "{ broken", -1, NULL));
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "cannot write *");
  adw_combo_row_set_selected (row, theme ? 1 : 2);
  g_test_assert_expected_messages ();
  g_assert_cmpuint (adw_combo_row_get_selected (row), ==, previous);
  g_assert_cmpint (
      adw_style_manager_get_color_scheme (adw_style_manager_get_default ()), ==,
      ADW_COLOR_SCHEME_FORCE_DARK);
  g_autofree gchar *bytes = NULL;
  g_assert_true (g_file_get_contents (path, &bytes, NULL, NULL));
  g_assert_cmpstr (bytes, ==, "{ broken");
  g_unlink (path);
  apply_theme ("system");
  teardown ();
}

static void
styles_resource (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes =
      g_resources_lookup_data ("/cc/arthur63/gchat/style.css",
                               G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (bytes);
  g_assert_cmpuint (g_bytes_get_size (bytes), >, 0);
}

static void
styles_single_install (void)
{
  if (!have_display)
    {
      g_test_skip ("需要 GTK 显示后端");
      return;
    }
  load_css ();
  load_css ();
  AdwStyleManager *manager = adw_style_manager_get_default ();
  guint count =
      g_signal_handlers_block_matched (manager, G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
                                       G_CALLBACK (on_dark_changed), NULL);
  g_signal_handlers_unblock_matched (manager, G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
                                     G_CALLBACK (on_dark_changed), NULL);
  g_assert_cmpuint (count, ==, 1);
}

static void
corrupt_startup (void)
{
  g_autofree gchar *path = store_session_path ();
  g_autofree gchar *dir = g_path_get_dirname (path);
  g_autofree gchar *bak = g_strconcat (path, ".bak", NULL);
  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  g_assert_true (g_file_set_contents (path, "{}", -1, NULL));
  g_assert_true (g_file_set_contents (bak, "{ broken", -1, NULL));
  gchar *argv[] = { "gchat", NULL };
  g_assert_cmpint (gchat_main (1, argv), ==, 1);
  g_assert_null (app);
  g_autofree gchar *primary = NULL;
  g_autofree gchar *backup = NULL;
  g_assert_true (g_file_get_contents (path, &primary, NULL, NULL));
  g_assert_true (g_file_get_contents (bak, &backup, NULL, NULL));
  g_assert_cmpstr (primary, ==, "{}");
  g_assert_cmpstr (backup, ==, "{ broken");
  g_unlink (path);
  g_unlink (bak);
}

static void
edit_preserves_history (void)
{
  setup ();
  add_message ("user", "question two");
  add_message ("assistant", "answer two");
  app->chat->edit_from = 2;
  regenerate ();
  g_assert_cmpuint (json_array_get_length (app->chat->messages), ==, 4);
  g_assert_cmpuint (app->chat->edit_from, ==, 2);
  g_assert_cmpstr (
      json_object_get_string_member (
          json_array_get_object_element (app->chat->messages, 3), "content"),
      ==, "answer two");
  commit_message ("edited question two");
  g_assert_cmpuint (json_array_get_length (app->chat->messages), ==, 3);
  g_assert_cmpstr (
      json_object_get_string_member (
          json_array_get_object_element (app->chat->messages, 2), "content"),
      ==, "edited question two");
  g_assert_cmpuint (app->chat->edit_from, ==, G_MAXUINT);
  teardown ();
}

static void
assert_input (const gchar *expected)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (app->input));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds (buffer, &start, &end);
  g_autofree gchar *text =
      gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
  g_assert_cmpstr (text, ==, expected);
}

static void
fork_message (gconstpointer data)
{
  if (!setup_ui ())
    return;
  guint index = GPOINTER_TO_UINT (data);
  add_message ("user", "question two\n ");
  add_message ("assistant", "answer two");
  Chat *source = app->chat;
  JsonObject *reply = json_array_get_object_element (source->messages, 1);
  json_object_set_string_member (reply, "reasoning", "thought");
  json_object_set_double_member (reply, "duration", 2.5);
  g_free (source->draft);
  source->draft = g_strdup ("original draft");
  render_chat ();
  if (index == 1)
    {
      source->edit_from = 2;
      g_free (source->edit_text);
      source->edit_text = g_strdup ("editing");
      render_chat ();
    }
  GtkWidget *bubble = gtk_widget_get_first_child (app->messages);
  for (guint i = 0; i < index; i++)
    bubble = gtk_widget_get_next_sibling (bubble);
  GtkWidget *actions = g_object_get_data (G_OBJECT (bubble), "actions");
  GtkWidget *button = gtk_widget_get_first_child (actions);
  while (button != NULL &&
         g_strcmp0 (gtk_widget_get_tooltip_text (button),
                    _ ("Fork conversation")) != 0)
    {
      button = gtk_widget_get_next_sibling (button);
    }
  g_assert_nonnull (button);
  g_assert_true (GTK_IS_DRAWING_AREA (
      gtk_button_get_child (GTK_BUTTON (button))));
  app->busy = TRUE;
  g_signal_emit_by_name (button, "clicked");
  g_assert_true (app->chat == source);
  app->busy = FALSE;
  g_signal_emit_by_name (button, "clicked");

  Chat *fork = app->chat;
  g_assert_true (fork != source);
  g_assert_cmpuint (app->chats->len, ==, 2);
  g_assert_cmpuint (json_array_get_length (source->messages), ==, 4);
  g_assert_cmpstr (source->draft, ==, "original draft");
  g_assert_cmpuint (fork->edit_from, ==, G_MAXUINT);
  guint count = index + (index == 1);
  g_assert_cmpuint (json_array_get_length (fork->messages), ==, count);
  const gchar *drafts[] = { "question one", "", "question two\n " };
  g_assert_cmpstr (fork->draft, ==, drafts[index]);
  assert_input (fork->draft);
  if (index == 1)
    {
      g_assert_cmpuint (source->edit_from, ==, 2);
      g_assert_cmpstr (source->edit_text, ==, "editing");
      JsonObject *copied = json_array_get_object_element (fork->messages, 1);
      g_assert_cmpstr (json_object_get_string_member (copied, "reasoning"), ==,
                       "thought");
      g_assert_cmpfloat (json_object_get_double_member (copied, "duration"),
                         ==, 2.5);
      json_object_set_string_member (copied, "content", "changed");
      g_assert_cmpstr (json_object_get_string_member (reply, "content"), ==,
                       "answer one");
    }
  g_autofree gchar *path = store_session_path ();
  g_autofree gchar *current = NULL;
  g_autofree gchar *model = NULL;
  g_autofree gchar *effort = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) loaded =
      store_load (path, &current, &model, &effort, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (current, ==, fork->id);
  Chat *saved = g_ptr_array_index (loaded, 1);
  g_assert_cmpstr (saved->draft, ==, fork->draft);
  g_assert_cmpuint (json_array_get_length (saved->messages), ==, count);
  teardown ();
}

static void
fork_stream_reply (void)
{
  if (!setup_ui ())
    return;
  begin_reply (FALSE);
  g_string_append (app->answer, "streamed answer");
  GtkWidget *bubble = app->reply_bubble;
  end_stream ();
  GtkWidget *actions = g_object_get_data (G_OBJECT (bubble), "actions");
  GtkWidget *button = gtk_widget_get_first_child (actions);
  while (button != NULL &&
         g_strcmp0 (gtk_widget_get_tooltip_text (button),
                    _ ("Fork conversation")) != 0)
    {
      button = gtk_widget_get_next_sibling (button);
    }
  g_assert_nonnull (button);
  g_assert_true (GTK_IS_DRAWING_AREA (
      gtk_button_get_child (GTK_BUTTON (button))));
  g_signal_emit_by_name (button, "clicked");
  g_assert_cmpuint (app->chats->len, ==, 2);
  JsonObject *reply = json_array_get_object_element (app->chat->messages, 2);
  g_assert_cmpstr (json_object_get_string_member (reply, "content"), ==,
                   "streamed answer");
  teardown ();
}

static GtkListBoxRow *
sidebar_row (Chat *chat)
{
  for (GtkWidget *row = gtk_widget_get_first_child (app->sidebar_list);
       row != NULL; row = gtk_widget_get_next_sibling (row))
    if (g_object_get_data (G_OBJECT (row), "chat") == chat)
      return GTK_LIST_BOX_ROW (row);
  return NULL;
}

static void
sidebar_menu (void)
{
  if (!setup_ui ())
    return;
  Chat *source = app->chat;
  g_autoptr (Chat) owned_other = chat_new ("other", "Other");
  Chat *other = owned_other;
  other->created = source->created + 1000;
  g_ptr_array_add (app->chats, g_steal_pointer (&owned_other));
  g_free (source->draft);
  source->draft = g_strdup ("draft");
  source->edit_from = 0;
  g_free (source->edit_text);
  source->edit_text = g_strdup ("editing");
  rebuild_sidebar ();
  render_chat ();
  show_window ();

  GtkWidget *row = GTK_WIDGET (sidebar_row (source));
  on_row_menu (NULL, 1, 10, 10, row);
  rebuild_sidebar ();
  row = GTK_WIDGET (sidebar_row (source));
  on_row_menu (NULL, 1, 10, 10, row);
  GtkWidget *popover = gtk_widget_get_last_child (row);
  g_assert_true (GTK_IS_POPOVER (popover));
  GtkWidget *box = gtk_popover_get_child (GTK_POPOVER (popover));
  GtkWidget *pin = gtk_widget_get_first_child (box);
  g_assert_cmpstr (gtk_button_get_label (GTK_BUTTON (pin)), ==, _ ("Pin"));
  g_signal_emit_by_name (pin, "clicked");
  g_assert_true (source->pinned);
  g_assert_cmpint (gtk_list_box_row_get_index (sidebar_row (source)), <,
                   gtk_list_box_row_get_index (sidebar_row (other)));

  row = GTK_WIDGET (sidebar_row (source));
  on_row_menu (NULL, 1, 10, 10, row);
  popover = gtk_widget_get_last_child (row);
  box = gtk_popover_get_child (GTK_POPOVER (popover));
  pin = gtk_widget_get_first_child (box);
  g_assert_cmpstr (gtk_button_get_label (GTK_BUTTON (pin)), ==, _ ("Unpin"));
  GtkWidget *copy_button = gtk_widget_get_next_sibling (pin);
  g_signal_emit_by_name (copy_button, "clicked");
  Chat *copy = app->chat;
  g_assert_true (copy != source);
  g_assert_false (copy->pinned);
  g_assert_cmpstr (copy->id, !=, source->id);
  g_assert_cmpstr (copy->draft, ==, "draft");
  g_assert_cmpstr (copy->edit_text, ==, "editing");
  g_assert_cmpuint (copy->edit_from, ==, 0);
  g_assert_cmpuint (json_array_get_length (copy->messages), ==, 2);
  JsonObject *message = json_array_get_object_element (copy->messages, 0);
  json_object_set_string_member (message, "content", "changed");
  message = json_array_get_object_element (source->messages, 0);
  g_assert_cmpstr (json_object_get_string_member (message, "content"), ==,
                   "question one");

  g_autofree gchar *path = store_session_path ();
  g_autofree gchar *current = NULL;
  g_autofree gchar *model = NULL;
  g_autofree gchar *effort = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) loaded =
      store_load (path, &current, &model, &effort, &error);
  g_assert_no_error (error);
  g_assert_true (((Chat *) g_ptr_array_index (loaded, 0))->pinned);
  g_assert_false (((Chat *) g_ptr_array_index (loaded, 2))->pinned);
  g_assert_cmpstr (current, ==, copy->id);

  on_row_pin (NULL, source);
  g_assert_false (source->pinned);
  g_assert_cmpint (gtk_list_box_row_get_index (sidebar_row (source)), >,
                   gtk_list_box_row_get_index (sidebar_row (other)));
  row = GTK_WIDGET (sidebar_row (source));
  on_row_menu (NULL, 1, 10, 10, row);
  popover = gtk_widget_get_last_child (row);
  box = gtk_popover_get_child (GTK_POPOVER (popover));
  GtkWidget *rename = gtk_widget_get_last_child (box);
  g_signal_emit_by_name (rename, "clicked");
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG (
      adw_application_window_get_visible_dialog (
          ADW_APPLICATION_WINDOW (app->window)));
  g_assert_nonnull (dialog);
  gtk_editable_set_text (GTK_EDITABLE (
      adw_alert_dialog_get_extra_child (dialog)), "Renamed");
  g_signal_emit_by_name (dialog, "response", "save");
  g_assert_cmpstr (source->title, ==, "Renamed");
  adw_dialog_force_close (ADW_DIALOG (dialog));
  row = GTK_WIDGET (sidebar_row (source));
  on_row_menu (NULL, 1, 10, 10, row);
  teardown ();
}

static void
assert_sidebar_centered (Chat    *chat,
                         gboolean has_snippet)
{
  GtkListBoxRow *row = sidebar_row (chat);
  g_assert_nonnull (row);
  GtkWidget *overlay = gtk_list_box_row_get_child (row);
  GtkWidget *labels = gtk_overlay_get_child (GTK_OVERLAY (overlay));
  GtkWidget *first = gtk_widget_get_first_child (labels);
  GtkWidget *last = gtk_widget_get_last_child (labels);
  g_assert_cmpint (first != last, ==, has_snippet);
  g_assert_true (gtk_widget_get_mapped (first));
  graphene_rect_t first_bounds, last_bounds;
  g_assert_true (
      gtk_widget_compute_bounds (first, GTK_WIDGET (row), &first_bounds));
  g_assert_true (
      gtk_widget_compute_bounds (last, GTK_WIDGET (row), &last_bounds));
  int first_y, last_y;
  gtk_label_get_layout_offsets (GTK_LABEL (first), NULL, &first_y);
  gtk_label_get_layout_offsets (GTK_LABEL (last), NULL, &last_y);
  PangoRectangle first_text, last_text;
  pango_layout_get_pixel_extents (gtk_label_get_layout (GTK_LABEL (first)),
                                  NULL, &first_text);
  pango_layout_get_pixel_extents (gtk_label_get_layout (GTK_LABEL (last)), NULL,
                                  &last_text);
  gdouble top = first_bounds.origin.y + first_y + first_text.y;
  gdouble bottom =
      last_bounds.origin.y + last_y + last_text.y + last_text.height;
  g_assert_cmpfloat (fabs ((top + bottom) / 2 -
                           gtk_widget_get_height (GTK_WIDGET (row)) / 2.0),
                     <=, 1);
  g_assert_cmpfloat (gtk_label_get_xalign (GTK_LABEL (first)), ==, 0);
}

static void
sidebar_layout (gconstpointer data)
{
  if (!setup_ui ())
    return;
  guint mode = GPOINTER_TO_UINT (data);
  Chat *first = app->chat;
  g_free (first->title);
  first->title = g_strdup ("侧栏会话标题");
  g_autoptr (Chat) owned_second =
      chat_new ("sidebar-second", "Other conversation");
  Chat *second = owned_second;
  g_ptr_array_add (app->chats, g_steal_pointer (&owned_second));
  const gchar *queries[] = { "", "会话标题", "answer one" };
  gtk_editable_set_text (GTK_EDITABLE (app->search), queries[mode]);
  rebuild_sidebar ();
  render_chat ();
  show_window ();
  assert_sidebar_centered (first, mode == 2);
  if (!mode)
    assert_sidebar_centered (second, FALSE);
  teardown ();
}

static void
draft_lifecycle (void)
{
  if (!setup_ui ())
    return;
  Chat *first = app->chat;
  g_autoptr (Chat) owned_second = chat_new ("second", "第二个会话");
  Chat *second = owned_second;
  g_ptr_array_add (app->chats, g_steal_pointer (&owned_second));
  rebuild_sidebar ();
  render_chat ();
  show_window ();
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (app->input));
  gtk_text_buffer_set_text (buffer, "普通草稿\n  保留空白 ", -1);
  g_assert_cmpstr (first->draft, ==, "普通草稿\n  保留空白 ");
  g_assert_cmpuint (app->draft_save_source, >, 0);
  on_edit_message (NULL, gtk_widget_get_first_child (app->messages));
  assert_input ("question one");
  gtk_text_buffer_set_text (buffer, "编辑草稿", -1);
  g_assert_cmpstr (first->edit_text, ==, "编辑草稿");
  g_assert_cmpstr (first->draft, ==, "普通草稿\n  保留空白 ");
  on_row_selected (NULL, sidebar_row (second), NULL);
  assert_input ("");
  g_assert_false (gtk_widget_get_visible (app->edit_bar));
  gtk_text_buffer_set_text (buffer, "另一份草稿", -1);
  on_row_selected (NULL, sidebar_row (first), NULL);
  assert_input ("编辑草稿");
  wait_for_layout ();
  g_assert_true (gtk_widget_get_mapped (app->edit_bar));
  g_assert_cmpuint (first->edit_from, ==, 0);
  g_assert_cmpuint (json_array_get_length (first->messages), ==, 2);
  g_autofree gchar *path = store_session_path ();
  g_autofree gchar *current = NULL;
  g_autofree gchar *model = NULL;
  g_autofree gchar *effort = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) loaded =
      store_load (path, &current, &model, &effort, &error);
  g_assert_no_error (error);
  Chat *restored = g_ptr_array_index (loaded, 0);
  g_assert_cmpuint (restored->edit_from, ==, 0);
  g_assert_cmpstr (restored->edit_text, ==, "编辑草稿");
  g_assert_cmpstr (restored->draft, ==, first->draft);
  g_assert_cmpstr (((Chat *) g_ptr_array_index (loaded, 1))->draft, ==,
                   "另一份草稿");
  on_cancel_edit (NULL, NULL);
  assert_input ("普通草稿\n  保留空白 ");
  g_assert_cmpuint (json_array_get_length (first->messages), ==, 2);
  g_assert_false (gtk_widget_get_visible (app->edit_bar));
  on_edit_message (NULL, gtk_widget_get_first_child (app->messages));
  commit_message ("重新发送的正文");
  restore_input ();
  assert_input ("普通草稿\n  保留空白 ");
  g_assert_cmpuint (json_array_get_length (first->messages), ==, 1);
  g_assert_cmpuint (first->edit_from, ==, G_MAXUINT);
  commit_message ("普通发送");
  restore_input ();
  assert_input ("");
  g_assert_cmpuint (json_array_get_length (first->messages), ==, 2);
  on_row_selected (NULL, sidebar_row (second), NULL);
  on_new_chat ();
  g_assert_true (app->chat != second);
  g_assert_cmpstr (second->draft, ==, "另一份草稿");
  assert_input ("");
  teardown ();
}

static void
delete_confirmation (gconstpointer data)
{
  if (!setup_ui ())
    return;
  guint mode = GPOINTER_TO_UINT (data);
  g_autoptr (Chat) owned_target = chat_new ("delete-target", "待删除会话");
  Chat *target = owned_target;
  g_ptr_array_add (app->chats, g_steal_pointer (&owned_target));
  if (mode == 4)
    app->chat = target;
  rebuild_sidebar ();
  render_chat ();
  show_window ();
  on_delete_chat (NULL, target);
  AdwAlertDialog *dialog =
      ADW_ALERT_DIALOG (adw_application_window_get_visible_dialog (
          ADW_APPLICATION_WINDOW (app->window)));
  g_assert_nonnull (dialog);
  g_autoptr (AdwAlertDialog) dialog_ref = g_object_ref (dialog);
  g_assert_cmpstr (adw_alert_dialog_get_default_response (dialog), ==,
                   "cancel");
  g_assert_cmpstr (adw_alert_dialog_get_close_response (dialog), ==, "cancel");
  g_assert_cmpuint (app->chats->len, ==, 2);
  if (mode == 2)
    app->busy = TRUE;
  if (mode == 3)
    g_ptr_array_remove_index (app->chats, 1);
  g_signal_emit_by_name (dialog, "response", mode == 0 ? "cancel" : "delete");
  g_assert_cmpuint (app->chats->len, ==,
                    mode == 1 || mode == 3 || mode == 4 ? 1 : 2);
  g_assert_cmpstr (app->chat->id, ==, "42");
  app->busy = FALSE;
  adw_dialog_force_close (ADW_DIALOG (dialog));
  g_clear_object (&dialog_ref);
  wait_for_layout ();
  teardown ();
}

static void
draft_startup (gconstpointer data)
{
  if (!have_display)
    {
      g_test_skip ("需要 GTK 显示后端");
      return;
    }
  setup ();
  app->api_key = g_strdup ("test-key-not-sent");
  g_free (app->chat->draft);
  app->chat->draft = g_strdup ("恢复的普通草稿");
  if (data != NULL)
    {
      app->chat->edit_from = 0;
      g_free (app->chat->edit_text);
      app->chat->edit_text = g_strdup ("恢复的编辑草稿");
    }
  g_autoptr (GtkApplication) application =
      gtk_application_new ("cc.arthur63.gchat.Tests", G_APPLICATION_NON_UNIQUE);
  g_autoptr (GError) error = NULL;
  g_assert_true (
      g_application_register (G_APPLICATION (application), NULL, &error));
  g_assert_no_error (error);
  activate (application, NULL);
  wait_for_layout ();
  assert_input (data != NULL ? "恢复的编辑草稿" : "恢复的普通草稿");
  g_assert_cmpint (gtk_widget_get_height (app->input_frame), >=,
                   INPUT_MIN_HEIGHT);
  g_assert_cmpint (gtk_widget_get_mapped (app->edit_bar), ==, data != NULL);

  GtkWidget *window = app->window, *input = app->input;
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (input));
  gtk_text_buffer_set_text (buffer, "重复激活时保留的输入", -1);
  activate (application, NULL);
  g_assert_cmpuint (g_list_length (gtk_application_get_windows (application)),
                    ==, 1);
  g_assert_true (app->window == window);
  g_assert_true (app->input == input);
  assert_input ("重复激活时保留的输入");
  g_assert_cmpint (gtk_widget_get_mapped (app->edit_bar), ==, data != NULL);

  app->cancel = g_cancellable_new ();
  GCancellable *cancel = app->cancel;
  set_busy (TRUE);
  activate (application, NULL);
  g_assert_cmpuint (g_list_length (gtk_application_get_windows (application)),
                    ==, 1);
  g_assert_true (app->window == window);
  g_assert_true (app->busy);
  g_assert_true (app->cancel == cancel);
  assert_input ("重复激活时保留的输入");
  teardown ();
}

static void
draft_autosave (void)
{
  if (!setup_ui ())
    return;
  render_chat ();
  show_window ();
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (app->input));
  gtk_text_buffer_set_text (buffer, "自动保存的草稿", -1);
  g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  g_timeout_add (900, layout_timeout, loop);
  g_main_loop_run (loop);
  g_assert_cmpuint (app->draft_save_source, ==, 0);
  g_autofree gchar *path = store_session_path ();
  g_autofree gchar *current = NULL;
  g_autofree gchar *model = NULL;
  g_autofree gchar *effort = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) loaded =
      store_load (path, &current, &model, &effort, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (((Chat *) g_ptr_array_index (loaded, 0))->draft, ==,
                   "自动保存的草稿");
  gtk_text_buffer_set_text (buffer, "关窗前的输入", -1);
  g_assert_cmpuint (app->draft_save_source, >, 0);
  on_close (NULL, NULL);
  g_assert_cmpuint (app->draft_save_source, ==, 0);
  teardown ();
}

static void
history_search (void)
{
  if (!setup_ui ())
    return;
  Chat *first = app->chat;
  g_autoptr (Chat) owned_second = chat_new ("search-second", "SQLite notes");
  Chat *second = owned_second;
  g_ptr_array_add (app->chats, g_steal_pointer (&owned_second));
  g_autoptr (GString) long_text = g_string_new (NULL);
  for (guint i = 0; i < 200; i++)
    g_string_append (long_text, "很长的前文 ");
  g_string_append (long_text, "Needle [a.*] 中文\n后文");
  JsonObject *msg = json_array_get_object_element (first->messages, 1);
  json_object_set_string_member (msg, "content", long_text->str);
  json_object_set_string_member (msg, "reasoning", "not-in-body");
  rebuild_sidebar ();
  render_chat ();
  show_window ();
  gtk_editable_set_text (GTK_EDITABLE (app->search), "  NEEDLE  ");
  on_search_changed (NULL, NULL);
  GtkListBoxRow *row = sidebar_row (first);
  g_assert_nonnull (row);
  g_assert_null (sidebar_row (second));
  wait_for_layout ();
  GtkWidget *overlay = gtk_list_box_row_get_child (row);
  GtkWidget *labels = gtk_overlay_get_child (GTK_OVERLAY (overlay));
  GtkWidget *snippet = gtk_widget_get_last_child (labels);
  g_assert_true (
      g_str_has_prefix (gtk_label_get_text (GTK_LABEL (snippet)), "…Needle"));
  g_assert_true (gtk_widget_get_mapped (snippet));
  g_assert_cmpint (gtk_widget_get_width (snippet), >, 0);
  gtk_editable_set_text (GTK_EDITABLE (app->search), "[a.*]");
  on_search_changed (NULL, NULL);
  g_assert_nonnull (sidebar_row (first));
  g_assert_null (sidebar_row (second));
  gtk_editable_set_text (GTK_EDITABLE (app->search), "中文");
  on_search_changed (NULL, NULL);
  g_assert_nonnull (sidebar_row (first));
  gtk_editable_set_text (GTK_EDITABLE (app->search), "sqlite");
  on_search_changed (NULL, NULL);
  g_assert_null (sidebar_row (first));
  row = sidebar_row (second);
  g_assert_nonnull (row);
  on_row_selected (NULL, row, NULL);
  g_assert_true (app->chat == second);
  gtk_editable_set_text (GTK_EDITABLE (app->search), "not-in-body");
  on_search_changed (NULL, NULL);
  GtkWidget *empty = gtk_widget_get_first_child (app->sidebar_list);
  g_assert_true (GTK_IS_LABEL (empty));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (empty)), ==,
                   _ ("No conversations found"));
  wait_for_layout ();
  g_assert_true (gtk_widget_get_mapped (empty));
  on_search_stop (GTK_SEARCH_ENTRY (app->search), NULL);
  on_search_changed (NULL, NULL);
  g_assert_nonnull (sidebar_row (first));
  g_assert_nonnull (sidebar_row (second));
  g_assert_true (app->chat == second);
  wait_for_layout ();
  g_assert_false (gtk_widget_get_mapped (empty));
  teardown ();
}

static gboolean
focus_inside (GtkWidget *widget)
{
  GtkWidget *focus = gtk_root_get_focus (GTK_ROOT (app->window));
  return focus != NULL &&
         (focus == widget || gtk_widget_is_ancestor (focus, widget));
}

/* Return a new strong reference to the IM context; the caller releases it. */
static GtkIMContext *
search_context (GtkWidget *search)
{
  GtkWidget *text =
      GTK_WIDGET (gtk_editable_get_delegate (GTK_EDITABLE (search)));
  g_autoptr (GListModel) controllers = gtk_widget_observe_controllers (text);
  g_autoptr (GtkIMContext) context = NULL;
  for (guint i = 0; i < g_list_model_get_n_items (controllers); i++)
    {
      g_autoptr (GObject) controller = g_list_model_get_item (controllers, i);
      if (GTK_IS_EVENT_CONTROLLER_KEY (controller))
        {
          GtkIMContext *candidate = gtk_event_controller_key_get_im_context (
              GTK_EVENT_CONTROLLER_KEY (controller));
          if (candidate != NULL)
            context = g_object_ref (candidate);
        }
      if (context != NULL)
        break;
    }
  g_assert_nonnull (context);
  return g_steal_pointer (&context);
}

static void
count_preedit (GtkIMContext *context G_GNUC_UNUSED,
               gpointer      data)
{
  (*(guint *) data)++;
}

static void
search_ime_commit (gconstpointer data)
{
  if (!setup_ui ())
    return;
  guint mode = GPOINTER_TO_UINT (data);
  const gchar *initial[] = { "", "前缀：", "被替换的内容", "开头结尾", "", "" };
  const gchar *expected[] = { "你好",         "前缀：你好", "你好",
                              "开头你好结尾", "你好",       "你好" };
  g_free (app->chat->title);
  app->chat->title = g_strdup (expected[mode]);
  rebuild_sidebar ();
  render_chat ();
  g_autoptr (GtkIMContext) context = search_context (app->search);
  gtk_im_multicontext_set_context_id (GTK_IM_MULTICONTEXT (context),
                                      "gchat-test-ime");
  g_autofree gchar *preedit = NULL;
  gtk_im_context_get_preedit_string (context, &preedit, NULL, NULL);
  g_assert_nonnull (test_context);
  show_window ();
  gtk_widget_grab_focus (app->search);
  gtk_editable_set_text (GTK_EDITABLE (app->search), initial[mode]);
  gtk_editable_set_position (GTK_EDITABLE (app->search), mode == 3 ? 2 : -1);
  if (mode == 2)
    gtk_editable_select_region (GTK_EDITABLE (app->search), 0, -1);
  g_signal_emit_by_name (context, "preedit-start");
  test_context->preedit = "nihao";
  test_context->cursor = 5;
  g_signal_emit_by_name (context, "preedit-changed");
  wait_for_layout ();
  g_assert_nonnull (g_object_get_data (G_OBJECT (context), "search-preedit"));
  g_autoptr (GtkIMContext) other = gtk_im_context_simple_new ();
  g_signal_emit_by_name (other, "commit", "unrelated");
  g_assert_nonnull (g_object_get_data (G_OBJECT (context), "search-preedit"));
  guint notifications = 0;
  gulong handler = g_signal_connect (
      context, "preedit-changed", G_CALLBACK (count_preedit), &notifications);
  if (mode != 5)
    {
      test_context->preedit = "";
      test_context->cursor = 0;
    }
  if (mode == 4)
    {
      g_signal_emit_by_name (context, "preedit-changed");
      notifications = 0;
    }
  g_signal_emit_by_name (context, "commit", "你好");
  g_assert_cmpuint (notifications, ==, mode < 4 ? 1 : 0);
  test_context->preedit = "";
  test_context->cursor = 0;
  g_signal_emit_by_name (context, "preedit-changed");
  on_search_changed (NULL, NULL);
  wait_for_layout ();
  g_assert_cmpstr (gtk_editable_get_text (GTK_EDITABLE (app->search)), ==,
                   expected[mode]);
  g_assert_null (g_object_get_data (G_OBJECT (context), "search-preedit"));
  g_assert_nonnull (sidebar_row (app->chat));
  g_assert_true (focus_inside (app->search));
  gint cursor = gtk_editable_get_position (GTK_EDITABLE (app->search));
  g_assert_cmpint (cursor, ==,
                   mode == 3 ? 4 : g_utf8_strlen (expected[mode], -1));
  GtkText *text =
      GTK_TEXT (gtk_editable_get_delegate (GTK_EDITABLE (app->search)));
  graphene_rect_t caret;
  gtk_text_compute_cursor_extents (text, cursor, &caret, NULL);
  g_assert_cmpfloat (caret.origin.x, >=, 0);
  g_assert_cmpfloat (caret.origin.x, <=,
                     gtk_widget_get_width (GTK_WIDGET (text)) + 1);
  g_assert_cmpfloat (caret.size.height, >, 0);
  notifications = 0;
  g_signal_emit_by_name (context, "commit", "A");
  g_assert_cmpuint (notifications, ==, 0);
  g_signal_handler_disconnect (context, handler);
  GtkIMContext *weak_context = context;
  g_object_add_weak_pointer (G_OBJECT (context), (gpointer *) &weak_context);
  g_clear_object (&context);
  teardown ();
  g_assert_null (weak_context);
  test_context = NULL;
}

static void
search_ime_lifetime (void)
{
  if (!have_display)
    {
      g_test_skip ("需要 GTK 显示后端");
      return;
    }
  GtkWidget *searches[2];
  GtkIMContext *contexts[2];
  TestIMContext *modules[2];
  guint notifications[2] = { 0 };
  for (guint i = 0; i < G_N_ELEMENTS (searches); i++)
    {
      searches[i] = g_object_ref_sink (gtk_search_entry_new ());
      connect_search_ime (searches[i]);
      contexts[i] = search_context (searches[i]);
      gtk_im_multicontext_set_context_id (GTK_IM_MULTICONTEXT (contexts[i]),
                                          "gchat-test-ime");
      g_autofree gchar *preedit = NULL;
      gtk_im_context_get_preedit_string (contexts[i], &preedit, NULL, NULL);
      modules[i] = test_context;
      modules[i]->preedit = "nihao";
      modules[i]->cursor = 5;
      g_signal_emit_by_name (contexts[i], "preedit-changed");
      g_signal_connect (contexts[i], "preedit-changed",
                        G_CALLBACK (count_preedit), &notifications[i]);
    }
  g_assert_true (contexts[0] != contexts[1]);
  for (guint i = 0; i < G_N_ELEMENTS (searches); i++)
    {
      g_assert_nonnull (
          g_object_get_data (G_OBJECT (contexts[i]), "search-preedit"));
      modules[i]->preedit = "";
      modules[i]->cursor = 0;
      g_signal_emit_by_name (contexts[i], "commit", "你好");
      g_assert_cmpuint (notifications[i], ==, 1);
      g_assert_cmpstr (gtk_editable_get_text (GTK_EDITABLE (searches[i])), ==,
                       "你好");
      if (i == 0)
        {
          g_assert_cmpuint (notifications[1], ==, 0);
          g_assert_nonnull (
              g_object_get_data (G_OBJECT (contexts[1]), "search-preedit"));
        }
      g_object_add_weak_pointer (G_OBJECT (contexts[i]),
                                 (gpointer *) &contexts[i]);
      g_object_unref (contexts[i]);
      g_object_unref (searches[i]);
      g_assert_null (contexts[i]);
    }
  test_context = NULL;
}

static void
keyboard_actions (void)
{
  if (!setup_ui ())
    return;
  g_autoptr (GtkApplication) application =
      gtk_application_new ("cc.arthur63.gchat.Tests", G_APPLICATION_NON_UNIQUE);
  g_autoptr (GError) error = NULL;
  g_assert_true (
      g_application_register (G_APPLICATION (application), NULL, &error));
  g_assert_no_error (error);
  gtk_window_set_application (GTK_WINDOW (app->window), application);
  register_actions (application);
  rebuild_sidebar ();
  render_chat ();
  show_window ();
  const gchar *actions[] = { "new-chat", "focus-input", "search", "settings",
                             "stop" };
  const gchar *keys[] = { "n", "l", "f", "comma", "period" };
  for (guint i = 0; i < G_N_ELEMENTS (actions); i++)
    {
      g_autofree gchar *name = g_strconcat ("win.", actions[i], NULL);
      g_auto (GStrv) accels =
          gtk_application_get_accels_for_action (application, name);
      g_autofree gchar *primary = g_strconcat ("<Primary>", keys[i], NULL);
      guint expected_key;
      GdkModifierType expected_mods;
      g_assert_true (
          gtk_accelerator_parse (primary, &expected_key, &expected_mods));
      gboolean found = FALSE;
      for (guint j = 0; accels[j] != NULL; j++)
        {
          guint key;
          GdkModifierType mods;
          g_assert_true (gtk_accelerator_parse (accels[j], &key, &mods));
          found |= key == expected_key && mods == expected_mods;
        }
      g_assert_true (found);
    }
  adw_overlay_split_view_set_show_sidebar (ADW_OVERLAY_SPLIT_VIEW (app->split),
                                           FALSE);
  g_assert_true (gtk_widget_activate_action (app->window, "win.search", NULL));
  g_assert_true (adw_overlay_split_view_get_show_sidebar (
      ADW_OVERLAY_SPLIT_VIEW (app->split)));
  g_assert_true (focus_inside (app->search));
  g_assert_true (
      gtk_widget_activate_action (app->window, "win.focus-input", NULL));
  g_assert_true (focus_inside (app->input));
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (app->input));
  gtk_text_buffer_set_text (buffer, "保留草稿", -1);
  on_edit_message (NULL, gtk_widget_get_first_child (app->messages));
  app->preedit = TRUE;
  g_assert_false (on_input_key (NULL, GDK_KEY_Escape, 0, 0, NULL));
  g_assert_false (on_input_key (NULL, GDK_KEY_Return, 0, 0, NULL));
  g_assert_cmpuint (app->chat->edit_from, ==, 0);
  app->preedit = FALSE;
  g_assert_false (on_input_key (NULL, GDK_KEY_Return, 0, GDK_SHIFT_MASK, NULL));
  g_assert_true (on_input_key (NULL, GDK_KEY_Escape, 0, 0, NULL));
  assert_input ("保留草稿");
  g_assert_cmpuint (app->chat->edit_from, ==, G_MAXUINT);
  app->cancel = g_cancellable_new ();
  g_assert_true (gtk_widget_activate_action (app->window, "win.stop", NULL));
  g_assert_false (g_cancellable_is_cancelled (app->cancel));
  set_busy (TRUE);
  g_assert_true (gtk_widget_activate_action (app->window, "win.stop", NULL));
  g_assert_true (g_cancellable_is_cancelled (app->cancel));
  set_busy (FALSE);
  g_assert_true (
      gtk_widget_activate_action (app->window, "win.settings", NULL));
  AdwDialog *dialog = adw_application_window_get_visible_dialog (
      ADW_APPLICATION_WINDOW (app->window));
  g_assert_true (ADW_IS_PREFERENCES_DIALOG (dialog));
  g_assert_true (
      gtk_widget_activate_action (app->window, "win.new-chat", NULL));
  g_assert_cmpuint (app->chats->len, ==, 1);
  adw_dialog_force_close (dialog);
  wait_for_layout ();
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
  g_autofree gchar *body = request_body (4, &len);
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
assert_reasoning_saved (void)
{
  g_assert_cmpuint (json_array_get_length (app->chat->messages), ==, 3);
  JsonObject *msg = json_array_get_object_element (app->chat->messages, 2);
  g_assert_cmpstr (json_object_get_string_member (msg, "content"), ==, "");
  g_assert_cmpstr (json_object_get_string_member (msg, "reasoning"), ==,
                   "已收到的推理");
  g_assert_cmpfloat (json_object_get_double_member (msg, "duration"), >=, 1);
  g_assert_false (app->busy);

  g_autofree gchar *path = store_session_path ();
  g_autofree gchar *current = NULL;
  g_autofree gchar *model = NULL;
  g_autofree gchar *effort = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) chats =
      store_load (path, &current, &model, &effort, &error);
  g_assert_no_error (error);
  Chat *chat = g_ptr_array_index (chats, 0);
  msg = json_array_get_object_element (chat->messages, 2);
  g_assert_cmpstr (json_object_get_string_member (msg, "reasoning"), ==,
                   "已收到的推理");
}

static void
stream_snapshot (void)
{
  if (!setup_ui ())
    return;
  begin_reply (TRUE);
  g_string_append (app->answer, "partial answer");
  g_assert_true (save_session ());
  g_assert_true (app->busy);
  g_assert_false (g_cancellable_is_cancelled (app->cancel));
  g_assert_cmpstr (app->answer->str, ==, "partial answer");
  g_autofree gchar *path = store_session_path ();
  g_autofree gchar *current = NULL;
  g_autofree gchar *model = NULL;
  g_autofree gchar *effort = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) loaded =
      store_load (path, &current, &model, &effort, &error);
  g_assert_no_error (error);
  Chat *chat = g_ptr_array_index (loaded, 0);
  JsonObject *reply = json_array_get_object_element (chat->messages, 2);
  g_assert_cmpstr (json_object_get_string_member (reply, "content"), ==,
                   "partial answer");
  g_assert_cmpstr (json_object_get_string_member (reply, "reasoning"), ==,
                   "已收到的推理");
  end_stream ();
  teardown ();
}

static gboolean
quit_test_application (gpointer data)
{
  gtk_text_buffer_set_text (
      gtk_text_view_get_buffer (GTK_TEXT_VIEW (app->input)), "退出前的草稿",
      -1);
  if (GPOINTER_TO_INT (data) == 1)
    {
      begin_reply (TRUE);
      g_string_append (app->answer, "pending answer");
    }
  else if (GPOINTER_TO_INT (data) == 2)
    {
      g_autofree gchar *path = store_session_path ();
      g_autofree gchar *dir = g_path_get_dirname (path);
      g_autofree gchar *tmp = g_strconcat (path, ".tmp", NULL);
      g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
      g_assert_cmpint (g_mkdir (tmp, 0700), ==, 0);
    }
  g_application_quit (
      G_APPLICATION (gtk_window_get_application (GTK_WINDOW (app->window))));
  return G_SOURCE_REMOVE;
}

static void
shutdown_snapshot (gconstpointer data)
{
  if (!have_display)
    {
      g_test_skip ("需要 GTK 显示后端");
      return;
    }
  if (!g_test_subprocess ())
    {
      g_test_trap_subprocess (NULL, 10000000, G_TEST_SUBPROCESS_DEFAULT);
      g_test_trap_assert_passed ();
      return;
    }
  setup ();
  app->api_key = g_strdup ("test-key-not-sent");
  g_autoptr (AdwApplication) gtk_app = adw_application_new (
      "cc.arthur63.gchat.Tests.Shutdown", G_APPLICATION_NON_UNIQUE);
  g_signal_connect (gtk_app, "activate", G_CALLBACK (activate), NULL);
  g_signal_connect (gtk_app, "shutdown", G_CALLBACK (on_shutdown), NULL);
  if (GPOINTER_TO_INT (data) == 2)
    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "cannot write *");
  g_timeout_add (20, quit_test_application, (gpointer) data);
  gchar *argv[] = { "gchat-shutdown-test", NULL };
  g_assert_cmpint (g_application_run (G_APPLICATION (gtk_app), 1, argv), ==, 0);
  g_assert_cmpuint (app->draft_save_source, ==, 0);
  g_assert_cmpuint (fit_source, ==, 0);
  g_assert_cmpuint (sync_tick, ==, 0);
  g_autofree gchar *path = store_session_path ();
  if (GPOINTER_TO_INT (data) == 2)
    {
      g_test_assert_expected_messages ();
      g_assert_false (app->closing);
      g_autofree gchar *tmp = g_strconcat (path, ".tmp", NULL);
      g_assert_cmpint (g_rmdir (tmp), ==, 0);
    }
  else
    {
      g_assert_true (app->closing);
      g_autofree gchar *current = NULL;
      g_autofree gchar *model = NULL;
      g_autofree gchar *effort = NULL;
      g_autoptr (GError) error = NULL;
      g_autoptr (GPtrArray) loaded =
          store_load (path, &current, &model, &effort, &error);
      g_assert_no_error (error);
      Chat *chat = g_ptr_array_index (loaded, 0);
      g_assert_cmpstr (chat->draft, ==, "退出前的草稿");
      if (GPOINTER_TO_INT (data) == 1)
        {
          JsonObject *reply = json_array_get_object_element (chat->messages, 2);
          g_assert_cmpstr (json_object_get_string_member (reply, "content"), ==,
                           "pending answer");
          g_assert_cmpstr (json_object_get_string_member (reply, "reasoning"),
                           ==, "已收到的推理");
        }
      g_autofree gchar *bak = g_strconcat (path, ".bak", NULL);
      g_assert_false (g_file_test (bak, G_FILE_TEST_EXISTS));
      g_assert_false (on_close (NULL, NULL));
      on_shutdown (G_APPLICATION (gtk_app), NULL);
      g_assert_false (g_file_test (bak, G_FILE_TEST_EXISTS));
    }
  teardown ();
}

static void
reasoning_visibility (void)
{
  if (!setup_ui ())
    return;
  begin_reply (FALSE);

  const gchar *events =
      "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"first\"}}]}\n"
      "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\" second\"}}]}\n"
      "data: {\"choices\":[{\"delta\":{\"content\":\"answer\"}}]}\n"
      "data: [DONE]\n";
  g_autoptr (GInputStream) input =
      g_memory_input_stream_new_from_data (events, -1, NULL);
  g_autoptr (GDataInputStream) lines = g_data_input_stream_new (input);
  g_data_input_stream_read_line_async (lines, G_PRIORITY_DEFAULT, app->cancel,
                                       stream_line_cb,
                                       g_object_ref (app->cancel));
  /* The stream callback owns this reference until the read ends. */
  (void) g_steal_pointer (&lines);
  while (app->reason_box == NULL)
    g_main_context_iteration (NULL, TRUE);
  GtkWidget *root = app->reason_box;
  GtkWidget *content = gtk_widget_get_last_child (root);
  GtkWidget *toggle = gtk_widget_get_first_child (root);
  GtkWidget *chevron = g_object_get_data (G_OBJECT (toggle), "chevron");
  g_assert_true (gtk_widget_get_visible (content));
  g_assert_cmpstr (gtk_image_get_icon_name (GTK_IMAGE (chevron)), ==,
                   "pan-down-symbolic");

  g_signal_emit_by_name (toggle, "clicked");
  while (g_str_equal (app->reasoning->str, "first"))
    g_main_context_iteration (NULL, TRUE);
  g_assert_false (gtk_widget_get_visible (content));
  g_signal_emit_by_name (toggle, "clicked");
  while (app->answer->len == 0)
    g_main_context_iteration (NULL, TRUE);
  g_assert_false (gtk_widget_get_visible (content));
  g_assert_cmpstr (gtk_image_get_icon_name (GTK_IMAGE (chevron)), ==,
                   "pan-end-symbolic");

  g_signal_emit_by_name (toggle, "clicked");
  while (app->busy)
    g_main_context_iteration (NULL, TRUE);
  g_assert_true (gtk_widget_get_visible (content));
  g_autoptr (GtkWidget) history =
      g_object_ref_sink (thought_widget ("saved", 1, NULL, NULL));
  g_assert_false (gtk_widget_get_visible (gtk_widget_get_last_child (history)));
  teardown ();
}

static void
reasoning_stop (void)
{
  if (!setup_ui ())
    return;
  begin_reply (TRUE);
  GtkWidget *content = gtk_widget_get_last_child (app->reason_box);
  set_thought_expanded (app->reason_box, TRUE);
  end_stream ();
  g_assert_false (gtk_widget_get_visible (content));
  assert_reasoning_saved ();
  teardown ();
}

static void
reasoning_failure (void)
{
  if (!setup_ui ())
    return;
  begin_reply (TRUE);
  GtkWidget *content = gtk_widget_get_last_child (app->reason_box);
  set_thought_expanded (app->reason_box, TRUE);
  fail_stream ("模拟断网");
  g_assert_false (gtk_widget_get_visible (content));
  assert_reasoning_saved ();
  teardown ();
}

static void
reasoning_close (void)
{
  if (!setup_ui ())
    return;
  begin_reply (TRUE);
  on_close (NULL, NULL);
  assert_reasoning_saved ();
  end_stream ();
  g_assert_cmpuint (json_array_get_length (app->chat->messages), ==, 3);
  teardown ();
}

static void
close_save_failure (void)
{
  if (!setup_ui ())
    return;
  begin_reply (FALSE);
  g_autoptr (GInputStream) body =
      g_memory_input_stream_new_from_data ("data: [DONE]\n", -1, NULL);
  g_autoptr (GDataInputStream) lines = g_data_input_stream_new (body);
  g_data_input_stream_read_line_async (lines, G_PRIORITY_DEFAULT, app->cancel,
                                       stream_line_cb,
                                       g_object_ref (app->cancel));
  /* The stream callback owns this reference until the read ends. */
  (void) g_steal_pointer (&lines);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (app->input));
  gtk_text_buffer_set_text (buffer, "不能丢失的草稿", -1);
  g_autofree gchar *path = store_session_path ();
  g_autofree gchar *dir = g_path_get_dirname (path);
  g_autofree gchar *tmp = g_strconcat (path, ".tmp", NULL);
  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  g_assert_cmpint (g_mkdir (tmp, 0700), ==, 0);
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "cannot write *");
  g_assert_true (on_close (NULL, NULL));
  g_test_assert_expected_messages ();
  g_assert_false (app->closing);
  g_assert_false (app->busy);
  g_assert_cmpstr (app->chat->draft, ==, "不能丢失的草稿");
  g_assert_cmpint (g_rmdir (tmp), ==, 0);
  /* After a new request starts inside the window, the cancel callback of the
     previous request must not end it. */
  begin_reply (FALSE);
  while (g_main_context_iteration (NULL, FALSE))
    {
    }
  g_assert_true (app->busy);
  g_assert_false (on_close (NULL, NULL));
  g_assert_cmpuint (fit_source, ==, 0);
  g_assert_cmpuint (sync_tick, ==, 0);
  g_assert_cmpuint (fit_retry_timer, ==, 0);
  g_assert_cmpuint (sync_retry_timer, ==, 0);
  teardown ();
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

static void
empty_close (void)
{
  if (!setup_ui ())
    return;
  begin_reply (FALSE);
  on_close (NULL, NULL);
  end_stream ();
  g_assert_cmpuint (json_array_get_length (app->chat->messages), ==, 2);
  g_assert_false (app->busy);
  teardown ();
}

typedef struct
{
  GSocketListener *listener;
  GMainLoop       *loop;

  gint64           started, tick_time;
  gboolean         cancel, timed_out;

  GPtrArray       *events;
  GAsyncQueue     *first_event;
} HttpTest;

static void
http_test_clear (HttpTest *test)
{
  g_clear_pointer (&test->first_event, g_async_queue_unref);
  g_clear_pointer (&test->events, g_ptr_array_unref);
  g_clear_object (&test->listener);
  g_clear_pointer (&test->loop, g_main_loop_unref);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (HttpTest, http_test_clear)

static gpointer
http_server (gpointer data)
{
  HttpTest *test = data;
  g_autoptr (GSocketConnection) conn =
      g_socket_listener_accept (test->listener, NULL, NULL, NULL);
  g_assert_nonnull (conn);
  g_socket_set_timeout (g_socket_connection_get_socket (conn), 3);
  GInputStream *input = g_io_stream_get_input_stream (G_IO_STREAM (conn));
  gchar request[4096];
  g_assert_cmpint (
      g_input_stream_read (input, request, sizeof request, NULL, NULL), >, 0);
  GOutputStream *out = g_io_stream_get_output_stream (G_IO_STREAM (conn));
  const gchar *headers = test->events != NULL
                             ? "HTTP/1.1 200 OK\r\nContent-Type: "
                               "text/event-stream\r\nConnection: close\r\n\r\n"
                             : "HTTP/1.1 400 Bad Request\r\nContent-Length: "
                               "4\r\nConnection: close\r\n\r\n";
  g_assert_true (g_output_stream_write_all (out, headers, strlen (headers),
                                            NULL, NULL, NULL));
  if (test->events != NULL)
    {
      if (test->first_event != NULL)
        g_async_queue_pop (test->first_event);
      for (guint i = 0; i < test->events->len; i++)
        {
          const gchar *event = g_ptr_array_index (test->events, i);
          g_assert_true (g_output_stream_write_all (out, event, strlen (event),
                                                    NULL, NULL, NULL));
          g_usleep (20000);
        }
    }
  else
    {
      g_usleep (400000);
      g_output_stream_write_all (out, "oops", 4, NULL, NULL, NULL);
    }
  g_io_stream_close (G_IO_STREAM (conn), NULL, NULL);
  return NULL;
}

static gboolean
http_tick (gpointer data)
{
  HttpTest *test = data;
  test->tick_time = g_get_monotonic_time () - test->started;
  g_assert_true (app->busy);
  if (test->cancel)
    g_cancellable_cancel (app->cancel);
  return G_SOURCE_REMOVE;
}

static gboolean
http_finished (gpointer data)
{
  HttpTest *test = data;
  if (app->busy)
    return G_SOURCE_CONTINUE;
  g_main_loop_quit (test->loop);
  return G_SOURCE_REMOVE;
}

static gboolean
http_timeout (gpointer data)
{
  HttpTest *test = data;
  test->timed_out = TRUE;
  g_main_loop_quit (test->loop);
  return G_SOURCE_REMOVE;
}

static void
http_error (gconstpointer data)
{
  if (!setup_ui ())
    return;
  begin_reply (FALSE);
  g_auto (HttpTest) test = {
    .cancel = GPOINTER_TO_INT (data),
    .listener = g_socket_listener_new (),
    .loop = g_main_loop_new (NULL, FALSE),
  };
  g_autoptr (GInetAddress) ip =
      g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
  g_autoptr (GSocketAddress) addr = g_inet_socket_address_new (ip, 0);
  g_autoptr (GSocketAddress) effective = NULL;
  g_autoptr (GError) error = NULL;
  g_assert_true (g_socket_listener_add_address (
      test.listener, addr, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL,
      &effective, &error));
  g_assert_no_error (error);
  guint port =
      g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (effective));
  g_autoptr (GThread) server = g_thread_new ("http-test", http_server, &test);
  g_autofree gchar *url = g_strdup_printf ("http://127.0.0.1:%u/", port);
  g_autoptr (SoupMessage) msg = soup_message_new ("GET", url);
  app->session = soup_session_new ();
  test.started = g_get_monotonic_time ();
  send_request (g_steal_pointer (&msg));
  g_timeout_add (50, http_tick, &test);
  guint finished = g_timeout_add (10, http_finished, &test);
  guint timeout = g_timeout_add (3000, http_timeout, &test);
  g_main_loop_run (test.loop);
  g_assert_false (test.timed_out);
  g_assert_cmpint (test.tick_time, >, 0);
  g_assert_cmpint (test.tick_time, <, 300000);
  g_assert_cmpuint (json_array_get_length (app->chat->messages), ==, 2);
  g_source_remove (timeout);
  if (g_main_context_find_source_by_id (NULL, finished) != NULL)
    g_source_remove (finished);
  g_thread_join (g_steal_pointer (&server));
  http_test_clear (&test);
  teardown ();
}

static gchar *
sse_event (const gchar *name,
           const gchar *text)
{
  g_autoptr (JsonNode) node = json_node_new (JSON_NODE_VALUE);
  json_node_set_string (node, text);
  g_autoptr (JsonGenerator) gen = json_generator_new ();
  json_generator_set_root (gen, node);
  g_autofree gchar *encoded = json_generator_to_data (gen, NULL);
  return g_strdup_printf (
      "data: {\"choices\":[{\"delta\":{\"%s\":%s}}]}\n\n", name, encoded);
}

static void
assert_visible_tail (void)
{
  GtkWidget *last = gtk_widget_get_last_child (app->messages);
  GtkWidget *body = g_object_get_data (G_OBJECT (last), "body");
  GtkWidget *view = gtk_widget_get_last_child (body);
  g_assert_true (GTK_IS_TEXT_VIEW (view));
  g_assert_cmpint (gtk_widget_get_height (view), >, 0);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds (buffer, &start, &end);
  g_autofree gchar *text =
      gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
  g_assert_nonnull (strstr (text, "回复结束，末尾必须能看见。"));
  gtk_text_iter_backward_char (&end);
  GdkRectangle glyph;
  gtk_text_view_get_iter_location (GTK_TEXT_VIEW (view), &end, &glyph);
  g_assert_cmpint (glyph.y + glyph.height, <=,
                   gtk_widget_get_height (view) + 1);
  graphene_rect_t bounds;
  g_assert_true (gtk_widget_compute_bounds (last, app->scroller, &bounds));
  gdouble bottom = bounds.origin.y + bounds.size.height;
  g_assert_cmpfloat (bottom, <=, gtk_widget_get_height (app->scroller) + 1);
  g_assert_cmpfloat (bottom, >, 0);
  g_assert_cmpfloat (fabs (gtk_adjustment_get_value (chat_stick.adj) -
                           stick_bottom (&chat_stick)),
                     <, 1);
  /* In the native layout, the list number and the bold title must share one
     display line. */
  GtkWidget *prose = gtk_widget_get_first_child (body);
  while (prose != NULL && !GTK_IS_TEXT_VIEW (prose))
    prose = gtk_widget_get_next_sibling (prose);
  g_assert_nonnull (prose);
  buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (prose));
  gtk_text_buffer_get_start_iter (buffer, &start);
  g_assert_true (gtk_text_iter_forward_search (
      &start, "1. 问答与解释", GTK_TEXT_SEARCH_TEXT_ONLY, &start, &end, NULL));
  GdkRectangle marker, title;
  gtk_text_view_get_iter_location (GTK_TEXT_VIEW (prose), &start, &marker);
  gtk_text_iter_forward_chars (&start, 3);
  gtk_text_view_get_iter_location (GTK_TEXT_VIEW (prose), &start, &title);
  g_assert_cmpint (marker.y, ==, title.y);
}

static void
redirect_test_request (SoupSession *session G_GNUC_UNUSED,
                       SoupMessage *request,
                       gpointer     data)
{
  g_autoptr (GUri) uri = g_uri_parse (data, SOUP_HTTP_URI_FLAGS, NULL);
  g_assert_nonnull (uri);
  soup_message_set_uri (request, uri);
}

static void
stream_layout (gconstpointer data)
{
  if (!setup_ui ())
    return;
  gint mode = GPOINTER_TO_INT (data);
  g_autoptr (GString) text =
      g_string_new ("我可以帮你做这些事：\n\n");
  for (guint i = 1; i <= 10; i++)
    g_string_append_printf (
        text, "%u. **问答与解释**\n   解释概念，回答问题，梳理思路。\n\n", i);
  g_string_append (text, "```c\nint main(void) { return 0; }\n```\n\n"
                         "| 能力 | 示例 |\n| --- | --- |\n| 写作 | 邮件 |\n\n"
                         "**回复结束，末尾必须能看见。**\n");
  JsonObject *history = json_array_get_object_element (app->chat->messages, 1);
  json_object_set_string_member (history, "content", text->str);
  if (mode == 5)
    json_object_set_string_member (
        json_array_get_object_element (app->chat->messages, 0), "content",
        "C语言实现冒泡排序");
  render_history (2);
  gtk_stack_set_visible_child_name (GTK_STACK (app->stack), "chat");
  show_window ();
  if (mode != 5)
    {
      begin_reply (FALSE);
      gtk_label_set_wrap (GTK_LABEL (app->reply_label), TRUE);
      gtk_label_set_wrap_mode (GTK_LABEL (app->reply_label),
                               PANGO_WRAP_WORD_CHAR);
      scroll_bottom ();
    }
  wait_for_layout ();
  if (mode == 1)
    {
      gtk_adjustment_set_value (chat_stick.adj, 0);
      g_assert_false (chat_stick.follow);
    }
  if (mode == 2)
    {
      GtkTextBuffer *input =
          gtk_text_view_get_buffer (GTK_TEXT_VIEW (app->input));
      gtk_text_buffer_set_text (input, text->str, -1);
      wait_for_layout ();
      gtk_text_buffer_set_text (input, "", -1);
      wait_for_layout ();
    }

  g_auto (HttpTest) test = {
    .listener = g_socket_listener_new (),
    .loop = g_main_loop_new (NULL, FALSE),
    .events = g_ptr_array_new_with_free_func (g_free),
  };
  if (mode == 5)
    test.first_event = g_async_queue_new ();
  g_ptr_array_add (test.events,
                   sse_event ("reasoning_content", "已收到的推理"));
  const gchar *cursor = text->str;
  while (*cursor)
    {
      const gchar *end = g_utf8_offset_to_pointer (
          cursor, MIN (40, g_utf8_strlen (cursor, -1)));
      g_autofree gchar *part = g_strndup (cursor, end - cursor);
      g_ptr_array_add (test.events, sse_event ("content", part));
      cursor = end;
    }
  if (mode != 3)
    g_ptr_array_add (test.events, g_strdup ("data: [DONE]\n\n"));
  if (mode == 4)
    {
      for (guint i = 0; i < test.events->len; i++)
        {
          const gchar *event = g_ptr_array_index (test.events, i);
          g_auto (GStrv) parts = g_strsplit (event + 6, "\n", -1);
          g_autofree gchar *crlf = g_strjoinv ("\r\n", parts);
          g_free (g_ptr_array_index (test.events, i));
          g_ptr_array_index (test.events, i) =
              g_strconcat ("data:", crlf, NULL);
        }
    }
  g_autoptr (GInetAddress) ip =
      g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
  g_autoptr (GSocketAddress) addr = g_inet_socket_address_new (ip, 0);
  g_autoptr (GSocketAddress) effective = NULL;
  g_autoptr (GError) error = NULL;
  g_assert_true (g_socket_listener_add_address (
      test.listener, addr, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL,
      &effective, &error));
  g_assert_no_error (error);
  guint port =
      g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (effective));
  g_autoptr (GThread) server = g_thread_new ("sse-test", http_server, &test);
  g_autofree gchar *url = g_strdup_printf ("http://127.0.0.1:%u/", port);
  app->session = soup_session_new ();
  if (mode == 5)
    {
      /* Click Regenerate for real; redirect the request to a local SSE
         service and never touch the real API. */
      g_signal_connect (app->session, "request-queued",
                        G_CALLBACK (redirect_test_request), url);
      app->api_key = g_strdup ("test-key-not-sent-to-deepseek");
      GtkWidget *reply = gtk_widget_get_last_child (app->messages);
      GtkWidget *actions = g_object_get_data (G_OBJECT (reply), "actions");
      GtkWidget *button =
          gtk_widget_get_next_sibling (gtk_widget_get_first_child (actions));
      g_assert_cmpstr (gtk_widget_get_tooltip_text (button), ==,
                       _ ("Regenerate"));
      g_signal_emit_by_name (button, "clicked");
      wait_for_layout ();
      assert_user_bubble_layout ("C语言实现冒泡排序");
      g_async_queue_push (test.first_event, GINT_TO_POINTER (1));
    }
  else
    {
      send_request (soup_message_new ("GET", url));
    }
  guint finished = g_timeout_add (10, http_finished, &test);
  guint timeout = g_timeout_add (5000, http_timeout, &test);
  g_main_loop_run (test.loop);
  g_assert_false (test.timed_out);
  g_source_remove (timeout);
  if (g_main_context_find_source_by_id (NULL, finished) != NULL)
    g_source_remove (finished);
  g_thread_join (g_steal_pointer (&server));
  wait_for_layout ();

  guint count = json_array_get_length (app->chat->messages);
  g_assert_cmpstr (
      json_object_get_string_member (
          json_array_get_object_element (app->chat->messages, count - 1),
          "content"),
      ==, text->str);
  g_assert_false (app->busy);
  if (mode == 5)
    assert_user_bubble_layout ("C语言实现冒泡排序");
  if (mode == 1)
    {
      g_assert_false (chat_stick.follow);
      g_assert_cmpfloat (gtk_adjustment_get_value (chat_stick.adj), ==, 0);
      scroll_bottom ();
      wait_for_layout ();
    }
  assert_visible_tail ();
  /* After an input box change and a history re-render, the real widget is still
     at the bottom, not just the adjustment value. */
  GtkTextBuffer *input = gtk_text_view_get_buffer (GTK_TEXT_VIEW (app->input));
  gtk_text_buffer_set_text (input, text->str, -1);
  wait_for_layout ();
  assert_visible_tail ();
  gtk_text_buffer_set_text (input, "", -1);
  wait_for_layout ();
  assert_visible_tail ();
  clear_messages ();
  render_history (count);
  scroll_bottom ();
  wait_for_layout ();
  assert_visible_tail ();
  if (mode == 5)
    assert_user_bubble_layout ("C语言实现冒泡排序");
  g_signal_handlers_disconnect_by_data (app->session, url);
  http_test_clear (&test);
  teardown ();
}

static GLogWriterOutput
layout_log (GLogLevelFlags   level,
            const GLogField *fields,
            gsize            n_fields,
            gpointer         data G_GNUC_UNUSED)
{
  const gchar *domain = NULL, *message = NULL;
  for (gsize i = 0; i < n_fields; i++)
    {
      if (g_str_equal (fields[i].key, "GLIB_DOMAIN"))
        domain = fields[i].value;
      else if (g_str_equal (fields[i].key, "MESSAGE"))
        message = fields[i].value;
    }
  /* Skipped frames from the macOS compositor do not affect layout assertions;
     unexpected warnings and criticals still count as errors. */
  if (g_strcmp0 (domain, "Gdk") == 0 && message != NULL &&
      g_str_has_prefix (
          message, "gdk_frame_timings_presented() called on skipped frame"))
    {
      return G_LOG_WRITER_HANDLED;
    }
  return g_log_writer_default (level, fields, n_fields, NULL);
}

int
main (int    argc,
      char **argv)
{
  g_autoptr (GError) error = NULL;
  test_dir = g_dir_make_tmp ("gchat-app-test-XXXXXX", &error);
  g_assert_no_error (error);
  g_setenv ("XDG_DATA_HOME", test_dir, TRUE);
  g_setenv ("XDG_CONFIG_HOME", test_dir, TRUE);
  g_test_init (&argc, &argv, NULL);
  g_log_set_writer_func (layout_log, NULL, NULL);
  i18n_init (NULL);
  gtk_disable_setlocale ();
  have_display = gtk_init_check ();
  if (have_display)
    {
      adw_init ();
      g_io_extension_point_implement (
          "gtk-im-module", test_im_context_get_type (), "gchat-test-ime", 0);
      load_css ();
    }
  g_test_add_data_func ("/app/user-bubble/regenerate", "C语言实现冒泡排序",
                        user_bubble_layout);
  g_test_add_data_func ("/app/user-bubble/greeting", "你好",
                        user_bubble_layout);
  g_test_add_data_func ("/app/user-bubble/model", "你是什么模型?",
                        user_bubble_layout);
  g_test_add_data_func ("/app/user-bubble/features", "你有什么功能?",
                        user_bubble_layout);
  g_test_add_data_func ("/app/user-bubble/latin", "What model are you?",
                        user_bubble_layout);
  g_test_add_data_func ("/app/user-bubble/multiline", "第一行\n第二行更长一点",
                        user_bubble_layout);
  g_test_add_data_func_full ("/app/user-bubble/long", g_strnfill (200, 'W'),
                             user_bubble_layout, g_free);
  g_test_add_data_func ("/app/code-block/light", GINT_TO_POINTER (FALSE),
                        code_block_layout);
  g_test_add_data_func ("/app/code-block/dark", GINT_TO_POINTER (TRUE),
                        code_block_layout);
  g_test_add_func ("/app/action-visibility", action_visibility);
  g_test_add_func ("/app/sse-parse", sse_parser);
  g_test_add_func ("/app/api-key-prompt-path", api_key_prompt_path);
  g_test_add_func ("/app/choice-selection", choice_selection);
  g_test_add_func ("/app/choice-lookup", choice_lookup);
  g_test_add_data_func ("/app/api-key/environment", "env-key",
                        api_key_priority);
  g_test_add_data_func ("/app/api-key/local", NULL, api_key_priority);
  g_test_add_data_func ("/app/api-key/empty-environment", "", api_key_priority);
  g_test_add_func ("/app/message-whitespace", message_whitespace);
  g_test_add_func ("/app/literal-ui-text", literal_ui_text);
  g_test_add_func ("/app/stale-retry", stale_retry);
  g_test_add_data_func ("/app/preferences/corrupt-theme", "theme",
                        corrupt_preference);
  g_test_add_data_func ("/app/preferences/corrupt-language", "language",
                        corrupt_preference);
  g_test_add_func ("/app/styles-resource", styles_resource);
  g_test_add_func ("/app/styles-single-install", styles_single_install);
  g_test_add_func ("/app/theme-save-failure", theme_save_failure);
  g_test_add_func ("/app/content-builder", content_builder);
  g_test_add_func ("/app/sidebar-builder", sidebar_builder);
  g_test_add_func ("/app/language-settings", language_settings);
  g_test_add_func ("/app/corrupt-startup", corrupt_startup);
  g_test_add_func ("/app/edit-preserves-history", edit_preserves_history);
  g_test_add_data_func ("/app/fork/first-user", GUINT_TO_POINTER (0),
                        fork_message);
  g_test_add_data_func ("/app/fork/assistant", GUINT_TO_POINTER (1),
                        fork_message);
  g_test_add_data_func ("/app/fork/later-user", GUINT_TO_POINTER (2),
                        fork_message);
  g_test_add_func ("/app/fork/stream-reply", fork_stream_reply);
  g_test_add_func ("/app/draft-lifecycle", draft_lifecycle);
  g_test_add_func ("/app/draft-autosave", draft_autosave);
  g_test_add_data_func ("/app/draft-startup/normal", NULL, draft_startup);
  g_test_add_data_func ("/app/draft-startup/editing", GINT_TO_POINTER (1),
                        draft_startup);
  g_test_add_func ("/app/sidebar-menu", sidebar_menu);
  g_test_add_func ("/app/history-search", history_search);
  g_test_add_data_func ("/app/sidebar-layout/normal", GUINT_TO_POINTER (0),
                        sidebar_layout);
  g_test_add_data_func ("/app/sidebar-layout/title-search",
                        GUINT_TO_POINTER (1), sidebar_layout);
  g_test_add_data_func ("/app/sidebar-layout/body-search", GUINT_TO_POINTER (2),
                        sidebar_layout);
  g_test_add_func ("/app/keyboard-actions", keyboard_actions);
  g_test_add_data_func ("/app/search-ime/plain", GUINT_TO_POINTER (0),
                        search_ime_commit);
  g_test_add_data_func ("/app/search-ime/prefix", GUINT_TO_POINTER (1),
                        search_ime_commit);
  g_test_add_data_func ("/app/search-ime/selection", GUINT_TO_POINTER (2),
                        search_ime_commit);
  g_test_add_data_func ("/app/search-ime/middle", GUINT_TO_POINTER (3),
                        search_ime_commit);
  g_test_add_data_func ("/app/search-ime/ordered", GUINT_TO_POINTER (4),
                        search_ime_commit);
  g_test_add_data_func ("/app/search-ime/ongoing", GUINT_TO_POINTER (5),
                        search_ime_commit);
  g_test_add_func ("/app/search-ime/lifetime", search_ime_lifetime);
  g_test_add_data_func ("/app/delete/cancel", GUINT_TO_POINTER (0),
                        delete_confirmation);
  g_test_add_data_func ("/app/delete/confirm", GUINT_TO_POINTER (1),
                        delete_confirmation);
  g_test_add_data_func ("/app/delete/busy", GUINT_TO_POINTER (2),
                        delete_confirmation);
  g_test_add_data_func ("/app/delete/stale", GUINT_TO_POINTER (3),
                        delete_confirmation);
  g_test_add_data_func ("/app/delete/current", GUINT_TO_POINTER (4),
                        delete_confirmation);
  g_test_add_func ("/app/payload-filters-reasoning", payload_filters_reasoning);
  g_test_add_data_func ("/app/shutdown/draft", GINT_TO_POINTER (0),
                        shutdown_snapshot);
  g_test_add_data_func ("/app/shutdown/stream", GINT_TO_POINTER (1),
                        shutdown_snapshot);
  g_test_add_data_func ("/app/shutdown/save-failure", GINT_TO_POINTER (2),
                        shutdown_snapshot);
  g_test_add_func ("/app/stream-snapshot", stream_snapshot);
  g_test_add_func ("/app/reasoning/visibility", reasoning_visibility);
  g_test_add_func ("/app/reasoning/stop", reasoning_stop);
  g_test_add_func ("/app/reasoning/failure", reasoning_failure);
  g_test_add_func ("/app/reasoning/close", reasoning_close);
  g_test_add_func ("/app/close-save-failure", close_save_failure);
  g_test_add_func ("/app/locked-startup", locked_startup);
  g_test_add_func ("/app/empty-close", empty_close);
  g_test_add_data_func ("/app/http/error", GINT_TO_POINTER (FALSE), http_error);
  g_test_add_data_func ("/app/http/cancel", GINT_TO_POINTER (TRUE), http_error);
  g_test_add_data_func ("/app/stream/regenerate", GINT_TO_POINTER (5),
                        stream_layout);
  g_test_add_data_func ("/app/stream/follow", GINT_TO_POINTER (0),
                        stream_layout);
  g_test_add_data_func ("/app/stream/read-above", GINT_TO_POINTER (1),
                        stream_layout);
  g_test_add_data_func ("/app/stream/input-resize", GINT_TO_POINTER (2),
                        stream_layout);
  g_test_add_data_func ("/app/stream/crlf", GINT_TO_POINTER (4), stream_layout);
  g_test_add_data_func ("/app/stream/eof", GINT_TO_POINTER (3), stream_layout);
  int status = g_test_run ();
  g_autofree gchar *lock =
      g_build_filename (test_dir, "gchat", "session.json.lock", NULL);
  g_unlink (lock);
  g_autofree gchar *dir = g_build_filename (test_dir, "gchat", NULL);
  g_rmdir (dir);
  g_rmdir (test_dir);
  g_free (test_dir);
  return status;
}
