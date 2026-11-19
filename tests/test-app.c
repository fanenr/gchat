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
      GtkWidget *bar = gtk_widget_get_first_child (card);
      g_assert_cmpint (gtk_widget_get_margin_top (bar), ==, 12);
      graphene_rect_t bar_bounds, label_bounds, copy_bounds;
      g_assert_true (gtk_widget_compute_bounds (bar, card, &bar_bounds));
      g_assert_true (gtk_widget_compute_bounds (
          gtk_widget_get_first_child (bar), card, &label_bounds));
      g_assert_true (gtk_widget_compute_bounds (
          gtk_widget_get_last_child (bar), card, &copy_bounds));
      g_assert_cmpfloat (bar_bounds.origin.y, >=, 12);
      g_assert_cmpfloat (bar_bounds.origin.y, <=, 14);
      g_assert_cmpfloat (label_bounds.origin.y, >=, bar_bounds.origin.y);
      g_assert_cmpfloat (copy_bounds.origin.y, >=, bar_bounds.origin.y);
      GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
      GtkTextIter end;
      gtk_text_buffer_get_end_iter (buffer, &end);
      gtk_text_iter_backward_char (&end);
      GdkRectangle glyph;
      gtk_text_view_get_iter_location (GTK_TEXT_VIEW (view), &end, &glyph);
      graphene_rect_t bounds;
      g_assert_true (gtk_widget_compute_bounds (view, card, &bounds));
      GtkTextIter start;
      gtk_text_buffer_get_start_iter (buffer, &start);
      GdkRectangle first;
      gtk_text_view_get_iter_location (GTK_TEXT_VIEW (view), &start, &first);
      g_assert_cmpfloat (label_bounds.origin.x, ==,
                         bounds.origin.x + first.x);
      gdouble bottom_gap = gtk_widget_get_height (card) - bounds.origin.y -
                           glyph.y - glyph.height;
      g_test_message ("code case %u: bottom gap %.1f px", i, bottom_gap);
      g_assert_cmpfloat (bottom_gap, >=, 16);
      g_assert_cmpfloat (bottom_gap, <=, 24);
      g_assert_cmpint (glyph.y + glyph.height, <=,
                       gtk_widget_get_height (view));
    }
  apply_theme ("system");
  teardown ();
}

static void
prose_layout (gconstpointer data)
{
  if (!setup_ui ())
    return;
  apply_theme (GPOINTER_TO_INT (data) ? "dark" : "light");
  static const gchar *prefixes[] = { "• ", "9. ", "10. ", "999999999. ",
                                     "☑ ", "☐ " };
  static const gchar *markers[] = { "-", "9.", "10.", "999999999.",
                                    "- [x]", "- [ ]" };
  static const gchar long_text[] =
      "Reading 中文长回复 should be comfortable. Each list item wraps "
      "onto several lines, and those lines must align with the first word "
      "rather than the number or checkbox at the start of the item.";
  for (guint i = 0; i < G_N_ELEMENTS (markers); i++)
    {
      g_autofree gchar *indent =
          g_strnfill (g_utf8_strlen (prefixes[i], -1), ' ');
      g_autofree gchar *markdown = g_strdup_printf (
          "%s %s\n\n%scontinuation\n\n%s- %s\n\n"
          "%s  nested continuation\n",
          markers[i], long_text, indent, indent, long_text, indent);
      JsonObject *msg = json_array_get_object_element (app->chat->messages, 1);
      json_object_set_string_member (msg, "content", markdown);
      render_chat ();
      show_window ();
      GtkWidget *bubble = gtk_widget_get_last_child (app->messages);
      GtkWidget *body = g_object_get_data (G_OBJECT (bubble), "body");
      GtkWidget *view = gtk_widget_get_first_child (body);
      g_assert_true (GTK_IS_TEXT_VIEW (view));
      g_assert_cmpint (gtk_text_view_get_pixels_inside_wrap (
                           GTK_TEXT_VIEW (view)),
                       ==, 0);
      const PangoFontDescription *font = pango_context_get_font_description (
          gtk_widget_get_pango_context (view));
      g_assert_cmpint (pango_font_description_get_size (font), ==,
                       14 * PANGO_SCALE);
      GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
      GtkTextIter start, end;
      gtk_text_buffer_get_start_iter (buffer, &start);
      g_autofree gchar *prefix =
          g_strdup_printf ("%sReading", prefixes[i]);
      g_assert_true (gtk_text_iter_forward_search (
          &start, prefix, GTK_TEXT_SEARCH_TEXT_ONLY, &start, &end, NULL));
      GdkRectangle marker, first, wrapped, paragraph, nested;
      gtk_text_view_get_iter_location (GTK_TEXT_VIEW (view), &start, &marker);
      gtk_text_iter_forward_chars (&start, g_utf8_strlen (prefixes[i], -1));
      gtk_text_view_get_iter_location (GTK_TEXT_VIEW (view), &start, &first);
      GtkTextIter next = start;
      g_assert_true (gtk_text_view_forward_display_line (GTK_TEXT_VIEW (view),
                                                         &next));
      gtk_text_view_get_iter_location (GTK_TEXT_VIEW (view), &next, &wrapped);
      g_assert_cmpint (marker.x, >=, 0);
      /* Checkbox fallback fonts can have different glyph extents. */
      g_assert_cmpint (marker.y, <, first.y + first.height);
      g_assert_cmpint (first.y, <, marker.y + marker.height);
      g_test_message ("list %s: marker %d, first %d, wrapped %d",
                      markers[i], marker.x, first.x, wrapped.x);
      g_assert_cmpint (ABS (wrapped.x - first.x), <=, 1);
      g_assert_cmpint (wrapped.y, >, first.y);
      g_assert_true (gtk_text_iter_forward_search (
          &next, "continuation", GTK_TEXT_SEARCH_TEXT_ONLY, &start, &end,
          NULL));
      gtk_text_view_get_iter_location (GTK_TEXT_VIEW (view), &start,
                                       &paragraph);
      g_assert_cmpint (ABS (paragraph.x - first.x), <=, 1);
      g_assert_true (gtk_text_iter_forward_search (
          &end, "• Reading", GTK_TEXT_SEARCH_TEXT_ONLY, &start, &end, NULL));
      gtk_text_iter_forward_chars (&start, 2);
      gtk_text_view_get_iter_location (GTK_TEXT_VIEW (view), &start, &nested);
      g_assert_cmpint (nested.x, >, first.x);
      next = start;
      g_assert_true (gtk_text_view_forward_display_line (GTK_TEXT_VIEW (view),
                                                         &next));
      gtk_text_view_get_iter_location (GTK_TEXT_VIEW (view), &next, &wrapped);
      g_assert_cmpint (ABS (wrapped.x - nested.x), <=, 1);
      g_assert_true (gtk_text_iter_forward_search (
          &next, "nested continuation", GTK_TEXT_SEARCH_TEXT_ONLY, &start,
          &end, NULL));
      gtk_text_view_get_iter_location (GTK_TEXT_VIEW (view), &start,
                                       &paragraph);
      g_assert_cmpint (ABS (paragraph.x - nested.x), <=, 1);
    }
  apply_theme ("system");
  teardown ();
}

static void
assert_line_height (GtkWidget *widget,
                    double     factor)
{
  GtkTextView *view = GTK_TEXT_VIEW (widget);
  GtkTextIter first, next;
  gtk_text_buffer_get_start_iter (gtk_text_view_get_buffer (view), &first);
  next = first;
  gtk_text_iter_forward_to_line_end (&next);
  g_autofree gchar *line = gtk_text_iter_get_text (&first, &next);
  g_autoptr (PangoLayout) layout =
      gtk_widget_create_pango_layout (widget, line);
  g_autoptr (PangoAttrList) attrs = pango_attr_list_new ();
  pango_attr_list_insert (attrs, pango_attr_line_height_new (factor));
  pango_layout_set_attributes (layout, attrs);
  int expected;
  pango_layout_get_pixel_size (layout, NULL, &expected);
  GdkRectangle a, b;
  gtk_text_view_get_iter_location (view, &first, &a);
  next = first;
  g_assert_true (gtk_text_view_forward_display_line (view, &next));
  gtk_text_view_get_iter_location (view, &next, &b);
  g_test_message ("line height: actual %d, expected %d", b.y - a.y, expected);
  g_assert_cmpint (ABS (b.y - a.y - expected), <=, 1);
}

static void
assert_paragraph_gap (GtkWidget   *widget,
                      const gchar *text,
                      int          gap)
{
  GtkTextView *view = GTK_TEXT_VIEW (widget);
  GtkTextIter start, end;
  gtk_text_buffer_get_start_iter (gtk_text_view_get_buffer (view), &start);
  g_assert_true (gtk_text_iter_forward_search (
      &start, text, GTK_TEXT_SEARCH_TEXT_ONLY, &start, &end, NULL));
  int actual;
  gtk_text_view_get_line_yrange (view, &start, NULL, &actual);
  g_autoptr (PangoLayout) layout =
      gtk_widget_create_pango_layout (widget, text);
  g_autoptr (PangoAttrList) attrs = pango_attr_list_new ();
  pango_attr_list_insert (attrs, pango_attr_line_height_new (1.3));
  pango_layout_set_attributes (layout, attrs);
  int line_height;
  pango_layout_get_pixel_size (layout, NULL, &line_height);
  g_test_message ("gap after %s: actual %d, expected %d", text,
                  actual - line_height, gap);
  g_assert_cmpint (ABS (actual - line_height - gap), <=, 1);
}

static void
reading_rhythm (gconstpointer data)
{
  if (!setup_ui ())
    return;
  apply_theme (GPOINTER_TO_INT (data) ? "dark" : "light");
  JsonObject *msg = json_array_get_object_element (app->chat->messages, 1);
  json_object_set_string_member (
      msg, "content", "Reading 中文\nReading 中文\n\n"
                      "paragraph\n\n#### Heading\n\n- first\n- second\n\n"
                      "```c\nint value;\nint value;\n```\n\n"
                      "| Header |\n| --- |\n| body |\n");
  render_chat ();
  show_window ();
  GtkWidget *bubble = gtk_widget_get_last_child (app->messages);
  GtkWidget *body = g_object_get_data (G_OBJECT (bubble), "body");
  GtkWidget *prose = gtk_widget_get_first_child (body);
  GtkWidget *card = gtk_widget_get_next_sibling (prose);
  GtkWidget *code = gtk_widget_get_last_child (card);
  for (guint i = 0; i < 2; i++)
    {
      if (i == 1)
        {
          g_assert_true (config_set ("message_font_size", "28"));
          g_assert_true (config_set ("code_font_size", "26"));
          refresh_font_css ();
          wait_for_layout ();
        }
      assert_line_height (prose, 1.3);
      assert_line_height (code, 1.6);
      assert_paragraph_gap (prose, "paragraph", 16);
      assert_paragraph_gap (prose, "Heading", 8);
      assert_paragraph_gap (prose, "first", 6);
      g_assert_cmpint (gtk_text_view_get_pixels_inside_wrap (
                           GTK_TEXT_VIEW (prose)),
                       ==, 0);
      g_assert_cmpint (gtk_widget_get_margin_top (code), ==, 16);
      g_assert_cmpint (gtk_widget_get_margin_bottom (code), ==, 16);
      g_assert_cmpint (gtk_widget_get_margin_start (code), ==, 16);
      g_assert_cmpint (gtk_widget_get_margin_end (code), ==, 16);
    }
  GtkWidget *table = gtk_widget_get_next_sibling (card);
  GtkWidget *grid = gtk_widget_get_first_child (table);
  GtkWidget *header = gtk_grid_get_child_at (GTK_GRID (grid), 0, 0);
  GtkWidget *cell = gtk_grid_get_child_at (GTK_GRID (grid), 0, 2);
  g_assert_false (gtk_widget_has_css_class (header, "dimmed"));
  g_assert_cmpint (gtk_widget_get_margin_top (cell), ==, 10);
  g_assert_cmpint (gtk_widget_get_margin_bottom (cell), ==, 10);
  g_assert_cmpint (gtk_widget_get_margin_start (cell), ==, 16);
  g_assert_cmpint (gtk_widget_get_margin_end (cell), ==, 16);
  g_assert_true (config_set ("message_font_size", NULL));
  g_assert_true (config_set ("code_font_size", NULL));
  refresh_font_css ();
  apply_theme ("system");
  teardown ();
}

static void
selection_spacing (gconstpointer data)
{
  if (!setup_ui ())
    return;
  apply_theme (GPOINTER_TO_INT (data) ? "dark" : "light");
  static const struct
  {
    const gchar *markdown;
    const gchar *selected;
    int          gap;
  } cases[] = {
    { "Paragraph 中文\n\n## Next", "Paragraph 中文", 24 },
    { "Wrapped paragraph 中文 with enough text to cross the available "
      "width. The selection must stop after the last line of text rather "
      "than filling the paragraph spacing before the next heading.\n\n"
      "## Next",
      "Wrapped paragraph", 24 },
    { "First line\nSecond line\n\nNext", "Second line", 16 },
    { "#### Heading\n\n- item", "Heading", 8 },
    { "- first\n- second", "first", 6 },
    { "- [x] first\n- [ ] second", "first", 6 },
    { "First paragraph\n\nSecond paragraph\n\nNext",
      "First paragraph\nSecond paragraph", 16 },
    { "שלום עברית\n\nNext", "שלום עברית", 16 },
  };
  JsonObject *msg = json_array_get_object_element (app->chat->messages, 1);
  for (guint size = 0; size < 2; size++)
    {
      if (size == 1)
        {
          g_assert_true (config_set ("message_font_size", "28"));
          refresh_font_css ();
        }
      for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
        {
          json_object_set_string_member (msg, "content", cases[i].markdown);
          render_chat ();
          show_window ();
          GtkWidget *bubble = gtk_widget_get_last_child (app->messages);
          GtkWidget *body = g_object_get_data (G_OBJECT (bubble), "body");
          GtkWidget *view = gtk_widget_get_first_child (body);
          GtkTextBuffer *buffer =
              gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
          GtkTextIter start, end, last;
          gtk_text_buffer_get_start_iter (buffer, &start);
          g_assert_true (gtk_text_iter_forward_search (
              &start, cases[i].selected, GTK_TEXT_SEARCH_TEXT_ONLY,
              &start, &end, NULL));
          if (!gtk_text_iter_ends_line (&end))
            gtk_text_iter_forward_to_line_end (&end);
          last = end;
          gtk_text_iter_backward_char (&last);
          gtk_text_buffer_select_range (buffer, &start, &end);
          gtk_widget_grab_focus (view);
          wait_for_layout ();

          int y, line_height;
          gtk_text_view_get_line_yrange (GTK_TEXT_VIEW (view), &last,
                                         &y, &line_height);
          GdkRectangle glyph;
          gtk_text_view_get_iter_location (GTK_TEXT_VIEW (view), &last,
                                           &glyph);
          int x = glyph.x + MAX (1, glyph.width / 2);
          int gap_top = y + line_height - cases[i].gap;
          gtk_text_view_buffer_to_window_coords (
              GTK_TEXT_VIEW (view), GTK_TEXT_WINDOW_WIDGET, x, gap_top,
              &x, &gap_top);
          int width = gtk_widget_get_width (view);
          int height = gtk_widget_get_height (view);
          GtkSnapshot *snapshot = gtk_snapshot_new ();
          GTK_WIDGET_GET_CLASS (view)->snapshot (view, snapshot);
          g_autoptr (GskRenderNode) node = gtk_snapshot_free_to_node (snapshot);
          g_assert_nonnull (node);
          graphene_rect_t bounds = GRAPHENE_RECT_INIT (0, 0, width, height);
          g_autoptr (GdkTexture) texture = gsk_renderer_render_texture (
              gtk_native_get_renderer (GTK_NATIVE (app->window)), node,
              &bounds);
          gsize stride = width * 4;
          g_autofree guchar *pixels = g_new0 (guchar, stride * height);
          gdk_texture_download (texture, pixels, stride);
          g_assert_cmpint (x, >=, 0);
          g_assert_cmpint (x, <, width);
          g_assert_cmpint (gap_top, >, 0);
          g_assert_cmpint (gap_top + cases[i].gap, <=, height);
          g_test_message ("selection size %u case %u: x %d, gap %d..%d",
                          size, i, x, gap_top, gap_top + cases[i].gap);
          g_assert_cmpuint (pixels[(gap_top - 1) * stride + x * 4 + 3], >, 0);
          for (int dy = 0; dy < cases[i].gap; dy++)
            g_assert_cmpuint (pixels[(gap_top + dy) * stride + x * 4 + 3],
                              ==, 0);
          if (strchr (cases[i].selected, '\n') != NULL)
            {
              gtk_text_view_get_line_yrange (GTK_TEXT_VIEW (view), &start,
                                             &y, &line_height);
              gtk_text_view_buffer_to_window_coords (
                  GTK_TEXT_VIEW (view), GTK_TEXT_WINDOW_WIDGET,
                  0, y + line_height - cases[i].gap, NULL, &gap_top);
              for (int dy = 0; dy < cases[i].gap; dy++)
                g_assert_cmpuint (
                    pixels[(gap_top + dy) * stride + 3], ==, 0);
            }
          g_assert_true (gtk_text_buffer_get_selection_bounds (buffer,
                                                               &start, &end));
          g_autofree gchar *selected =
              gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
          g_assert_true (g_str_has_prefix (selected, cases[i].selected));
        }
    }
  g_assert_true (config_set ("message_font_size", NULL));
  refresh_font_css ();
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

  g_assert_null (sse_data (": keep-alive"));
  g_assert_null (sse_data ("datax: ignored"));
  g_assert_cmpstr (sse_data ("data"), ==, "");
  g_assert_cmpstr (sse_data ("data:"), ==, "");
  g_assert_cmpstr (sse_data ("data:  value"), ==, " value");

  g_assert_false (sse_parse ("[DONE]", &content, &reasoning));
  g_assert_false (sse_parse ("not json", &content, &reasoning));
  g_assert_false (sse_parse ("{\"choices\":[]}", &content, &reasoning));

  g_assert_true (
      sse_parse ("{\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}",
                 &content, &reasoning));
  g_assert_cmpstr (content, ==, "hi");
  g_assert_null (reasoning);
  g_clear_pointer (&content, g_free);

  g_assert_true (sse_parse ("{\"choices\":[{\"delta\":{\"reasoning_content\":"
                            "\"think\",\"content\":\"a\"}}]}",
                            &content, &reasoning));
  g_assert_cmpstr (content, ==, "a");
  g_assert_cmpstr (reasoning, ==, "think");
  g_clear_pointer (&content, g_free);
  g_clear_pointer (&reasoning, g_free);

  static const gchar *invalid[] = {
    "null",
    "[]",
    "{}",
    "{\"choices\":null}",
    "{\"choices\":{}}",
    "{\"choices\":[null]}",
    "{\"choices\":[{\"delta\":null}]}",
    "{\"choices\":[{\"delta\":42}]}",
  };
  for (guint i = 0; i < G_N_ELEMENTS (invalid); i++)
    {
      g_assert_false (sse_parse (invalid[i], &content, &reasoning));
      g_assert_null (content);
      g_assert_null (reasoning);
    }
  g_assert_true (
      sse_parse ("{\"choices\":\n[{\"delta\":{\"content\":\"hi\"}}]}",
                 &content, &reasoning));
  g_assert_cmpstr (content, ==, "hi");
  g_clear_pointer (&content, g_free);
  g_assert_true (
      sse_parse ("{\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}",
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
  g_autofree gchar *current = NULL;
  g_autofree gchar *model = NULL;
  g_autofree gchar *effort = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) loaded =
      store_load (app->db, &current, &model, &effort, &error);
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

/* Return the borrowed button with the matching tooltip. */
static GtkWidget *
find_button (GtkWidget   *root,
             const gchar *tooltip)
{
  if (GTK_IS_BUTTON (root) &&
      g_strcmp0 (gtk_widget_get_tooltip_text (root), tooltip) == 0)
    {
      return root;
    }
  for (GtkWidget *child = gtk_widget_get_first_child (root); child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = find_button (child, tooltip);
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

  int requested_width, requested_height;
  gtk_widget_get_size_request (app->window, &requested_width, &requested_height);
  g_assert_cmpint (requested_width, ==, 640);
  g_assert_cmpint (requested_height, ==, 480);
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
content_width_layout (void)
{
  if (!setup_ui ())
    return;

  g_assert_cmpint (chat_width_for (0), ==, 0);
  g_assert_cmpint (chat_width_for (32), ==, 32);
  g_assert_cmpint (chat_width_for (400), ==, 372);
  g_assert_cmpint (chat_width_for (640), ==, 576);
  g_assert_cmpint (chat_width_for (760), ==, 678);
  g_assert_cmpint (chat_width_for (800), ==, 704);
  g_assert_cmpint (chat_width_for (1000), ==, 827);
  g_assert_cmpint (chat_width_for (1200), ==, 932);
  g_assert_cmpint (chat_width_for (2000), ==, 1532);
  for (int width = 1; width <= 2400; width++)
    {
      int difference = chat_width_for (width) - chat_width_for (width - 1);
      g_assert_cmpint (difference, >=, 0);
      g_assert_cmpint (difference, <=, 1);
    }
  JsonObject *reply = json_array_get_object_element (app->chat->messages, 1);
  json_object_set_string_member (
      reply, "content", "Example text.\n\n```c\nint main(void) { return 0; }"
                        "\n```\n\n| Column | Value |\n| --- | --- |\n"
                        "| Example | `code` |\n");
  render_history (2);
  GtkWidget *bubble = gtk_widget_get_last_child (app->messages);
  GtkWidget *body = g_object_get_data (G_OBJECT (bubble), "body");
  GtkWidget *code = NULL;
  GtkWidget *table = NULL;
  for (GtkWidget *child = gtk_widget_get_first_child (body); child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      if (gtk_widget_has_css_class (child, "code-card"))
        code = child;
      if (gtk_widget_has_css_class (child, "table-card"))
        table = child;
    }
  g_assert_nonnull (code);
  g_assert_nonnull (table);
  gtk_stack_set_visible_child_name (GTK_STACK (app->stack), "chat");
  adw_overlay_split_view_set_show_sidebar (ADW_OVERLAY_SPLIT_VIEW (app->split),
                                           FALSE);
  show_window ();
  const int widths[] = { 400, 639, 640, 760, 800, 1000,
                         1199, 1200, 1201, 1600 };
  for (guint sidebar = 0; sidebar < 2; sidebar++)
    {
      adw_overlay_split_view_set_show_sidebar (
          ADW_OVERLAY_SPLIT_VIEW (app->split), sidebar != 0);
      if (sidebar)
        for (guint frame = 0; frame < 6; frame++)
          wait_for_layout ();
      for (guint i = 0; i < G_N_ELEMENTS (widths); i++)
        {
          gtk_window_set_default_size (GTK_WINDOW (app->window), widths[i],
                                       800);
          wait_for_layout ();
          if (widths[i] < 640)
            {
              for (guint retry = 0;
                   retry < 20 && gtk_widget_get_width (app->window) != 640;
                   retry++)
                wait_for_layout ();
              g_assert_cmpint (gtk_widget_get_width (app->window), ==, 640);
            }
          int window_width = gtk_widget_get_width (app->window);
          g_assert_cmpint (window_width, >=, 640);
          int available = gtk_widget_get_width (app->scroller);
          int messages = gtk_widget_get_width (app->messages);
          int input = gtk_widget_get_width (app->input_frame);
          g_test_message ("sidebar=%u window=%d available=%d content=%d",
                          sidebar, widths[i], available, messages);
          g_assert_cmpint (ABS (messages - input), <=, 2);
          double progress = CLAMP ((available - 760) / 440.0, 0.0, 1.0);
          double fraction = 0.85 - 0.10 * progress;
          int expected = MIN (available - 32, (int) (available * fraction));
          g_assert_cmpint (ABS (messages - expected), <=, 2);
          g_assert_cmpint (ABS (gtk_widget_get_width (code) - messages), <=, 2);
          g_assert_cmpint (ABS (gtk_widget_get_width (table) - messages), <=,
                           2);
          graphene_rect_t message_bounds, input_bounds;
          g_assert_true (gtk_widget_compute_bounds (app->messages, app->window,
                                                    &message_bounds));
          g_assert_true (gtk_widget_compute_bounds (app->input_frame,
                                                    app->window,
                                                    &input_bounds));
          g_assert_cmpfloat (fabs (message_bounds.origin.x -
                                   input_bounds.origin.x), <=, 1);
        }
    }
  teardown ();
}

static void
minimum_window_layout (void)
{
  if (!setup_ui ())
    return;

  render_history (2);
  gtk_stack_set_visible_child_name (GTK_STACK (app->stack), "chat");
  show_window ();
  const int sizes[][2] = { { 400, 300 }, { 639, 479 }, { 640, 480 } };
  for (guint sidebar = 0; sidebar < 2; sidebar++)
    {
      adw_overlay_split_view_set_show_sidebar (
          ADW_OVERLAY_SPLIT_VIEW (app->split), sidebar != 0);
      for (guint frame = 0; frame < 6; frame++)
        wait_for_layout ();
      g_assert_cmpint (adw_overlay_split_view_get_show_sidebar (
                           ADW_OVERLAY_SPLIT_VIEW (app->split)), ==,
                       sidebar != 0);
      for (guint i = 0; i < G_N_ELEMENTS (sizes); i++)
        {
          gtk_window_set_default_size (GTK_WINDOW (app->window), sizes[i][0],
                                       sizes[i][1]);
          for (guint retry = 0; retry < 20; retry++)
            {
              wait_for_layout ();
              if (gtk_widget_get_width (app->window) == 640 &&
                  gtk_widget_get_height (app->window) == 480)
                break;
            }
          g_assert_cmpint (gtk_widget_get_width (app->window), ==, 640);
          g_assert_cmpint (gtk_widget_get_height (app->window), ==, 480);
          g_assert_cmpint (gtk_widget_get_height (app->scroller), >=, 100);
          g_assert_cmpint (gtk_widget_get_height (app->input_frame), >=,
                           INPUT_MIN_HEIGHT);
          g_assert_true (gtk_widget_get_mapped (app->scroller));
          g_assert_true (gtk_widget_get_mapped (app->input_frame));
          graphene_rect_t bounds;
          g_assert_true (gtk_widget_compute_bounds (app->input_frame,
                                                    app->window, &bounds));
          g_assert_cmpfloat (bounds.origin.x, >=, 0);
          g_assert_cmpfloat (bounds.origin.y, >=, 0);
          g_assert_cmpfloat (bounds.origin.x + bounds.size.width, <=, 640);
          g_assert_cmpfloat (bounds.origin.y + bounds.size.height, <=, 480);
        }
    }
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

static GtkFontDialogButton *
settings_font_button (AdwDialog   *dialog,
                      const gchar *title)
{
  GtkWidget *label = find_label (GTK_WIDGET (dialog), title);
  g_assert_nonnull (label);
  AdwActionRow *row =
      ADW_ACTION_ROW (gtk_widget_get_ancestor (label, ADW_TYPE_ACTION_ROW));
  return GTK_FONT_DIALOG_BUTTON (adw_action_row_get_activatable_widget (row));
}

static AdwSpinRow *
settings_font_size (AdwDialog   *dialog,
                    const gchar *title)
{
  GtkWidget *label = find_label (GTK_WIDGET (dialog), title);
  g_assert_nonnull (label);
  return ADW_SPIN_ROW (gtk_widget_get_ancestor (label, ADW_TYPE_SPIN_ROW));
}

static void
assert_font_size (GtkWidget *widget,
                  double     size)
{
  const PangoFontDescription *font = pango_context_get_font_description (
      gtk_widget_get_pango_context (widget));
  g_assert_cmpint (pango_font_description_get_size (font), ==,
                   (int) (size * PANGO_SCALE));
}

static void
font_settings (void)
{
  if (!setup_ui ())
    return;
  g_assert_true (config_set ("api_key", "font-test-key"));
  g_assert_true (config_set ("theme", "dark"));
  JsonObject *msg = json_array_get_object_element (app->chat->messages, 1);
  const gchar *markdown =
      "- Reading a long reply should be comfortable. Wrapped lines align "
      "with the first word, even after the font size changes.\n\n"
      "  ```c\n  int value = 42;\n  ```\n\n  after code\n\n"
      "  - Nested lines also wrap at the correct indentation, without "
      "replacing the original text buffer or losing its selection.\n";
  json_object_set_string_member (msg, "content", markdown);
  render_chat ();
  rebuild_sidebar ();
  show_window ();
  GtkWidget *bubble = gtk_widget_get_last_child (app->messages);
  GtkWidget *body = g_object_get_data (G_OBJECT (bubble), "body");
  GtkWidget *view = gtk_widget_get_first_child (body);
  GtkWidget *card = gtk_widget_get_next_sibling (view);
  g_assert_true (gtk_widget_has_css_class (card, "code-card"));
  GtkWidget *code = gtk_widget_get_last_child (card);
  assert_font_size (view, 14);
  assert_font_size (code, 13);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
  GtkTextIter start, end;
  gtk_text_buffer_get_iter_at_offset (buffer, &start, 2);
  gtk_text_buffer_get_iter_at_offset (buffer, &end, 9);
  gtk_text_buffer_select_range (buffer, &start, &end);

  on_settings_clicked (NULL, NULL);
  wait_for_layout ();
  AdwDialog *dialog = adw_application_window_get_visible_dialog (
      ADW_APPLICATION_WINDOW (app->window));
  const gchar *titles[] = { _ ("Message font"), _ ("Code block font") };
  const gchar *size_titles[] = { _ ("Message font size"),
                                 _ ("Code block font size") };
  const gchar *families[] = { "Serif", "Monospace" };
  const double sizes[] = { 24, 18 };
  for (guint i = 0; i < G_N_ELEMENTS (FONT_SETTINGS); i++)
    {
      GtkFontDialogButton *button = settings_font_button (dialog, titles[i]);
      AdwSpinRow *row = settings_font_size (dialog, size_titles[i]);
      g_assert_cmpint (gtk_font_dialog_button_get_level (button), ==,
                       GTK_FONT_LEVEL_FAMILY);
      g_assert_cmpfloat (adw_spin_row_get_value (row), ==,
                         FONT_SETTINGS[i].default_size);
      g_assert_true (gtk_widget_get_mapped (GTK_WIDGET (button)));
      g_assert_cmpstr (adw_action_row_get_subtitle (ADW_ACTION_ROW (row)), ==,
                       "");
      GtkWidget *reset = gtk_widget_get_next_sibling (GTK_WIDGET (button));
      GtkWidget *size_reset =
          find_button (GTK_WIDGET (row), _ ("Use default font size"));
      g_assert_nonnull (size_reset);
      g_assert_false (gtk_widget_get_visible (reset));
      g_assert_false (gtk_widget_get_visible (size_reset));
      g_autofree gchar *default_family = g_ascii_strdown (
          pango_font_description_get_family (
              gtk_font_dialog_button_get_font_desc (button)),
          -1);
      g_autoptr (PangoFontDescription) fallback =
          pango_font_description_from_string (default_family);
      g_autoptr (PangoFontDescription) font =
          pango_font_description_from_string (families[i]);
      gtk_font_dialog_button_set_font_desc (button, font);
      adw_spin_row_set_value (row, sizes[i]);
      g_assert_true (gtk_widget_get_visible (reset));
      g_assert_true (gtk_widget_get_visible (size_reset));
      gtk_font_dialog_button_set_font_desc (button, fallback);
      g_assert_false (gtk_widget_get_visible (reset));
      g_assert_true (gtk_widget_get_visible (size_reset));
      gtk_font_dialog_button_set_font_desc (button, font);
      adw_spin_row_set_value (row, FONT_SETTINGS[i].default_size);
      g_assert_true (gtk_widget_get_visible (reset));
      g_assert_false (gtk_widget_get_visible (size_reset));
      adw_spin_row_set_value (row, sizes[i]);
      g_assert_true (gtk_widget_get_visible (size_reset));
      g_autofree gchar *saved = config_get (FONT_SETTINGS[i].key);
      g_assert_cmpstr (saved, ==, families[i]);
    }
  wait_for_layout ();
  g_assert_true (gtk_widget_get_first_child (body) == view);
  g_assert_true (gtk_widget_get_next_sibling (view) == card);
  g_assert_true (gtk_text_view_get_buffer (GTK_TEXT_VIEW (view)) == buffer);
  g_assert_true (gtk_text_buffer_get_selection_bounds (buffer, &start, &end));
  g_assert_cmpint (gtk_text_iter_get_offset (&start), ==, 2);
  g_assert_cmpint (gtk_text_iter_get_offset (&end), ==, 9);
  assert_font_size (view, 24);
  assert_font_size (code, 18);
  assert_font_size (app->input, 14);
  GtkWidget *new_chat = find_label (
      adw_overlay_split_view_get_sidebar (ADW_OVERLAY_SPLIT_VIEW (app->split)),
      _ ("New chat"));
  assert_font_size (new_chat, 14);
  assert_user_bubble_layout ("question one");
  gtk_text_buffer_get_iter_at_offset (buffer, &start, 2);
  GdkRectangle first, wrapped, later;
  gtk_text_view_get_iter_location (GTK_TEXT_VIEW (view), &start, &first);
  g_assert_true (gtk_text_view_forward_display_line (GTK_TEXT_VIEW (view),
                                                     &start));
  gtk_text_view_get_iter_location (GTK_TEXT_VIEW (view), &start, &wrapped);
  g_assert_cmpint (ABS (wrapped.x - first.x), <=, 1);
  GtkWidget *tail = gtk_widget_get_next_sibling (card);
  GtkTextBuffer *tail_buffer =
      gtk_text_view_get_buffer (GTK_TEXT_VIEW (tail));
  gtk_text_buffer_get_start_iter (tail_buffer, &start);
  gtk_text_view_get_iter_location (GTK_TEXT_VIEW (tail), &start, &later);
  g_assert_cmpint (ABS (later.x - first.x), <=, 1);

  g_autofree gchar *path = store_config_path ();
  g_autofree gchar *tmp = g_strconcat (path, ".tmp", NULL);
  g_assert_cmpint (g_mkdir (tmp, 0700), ==, 0);
  for (guint i = 0; i < G_N_ELEMENTS (FONT_SETTINGS); i++)
    {
      GtkFontDialogButton *button = settings_font_button (dialog, titles[i]);
      AdwSpinRow *row = settings_font_size (dialog, size_titles[i]);
      g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "cannot write *");
      adw_spin_row_set_value (row, 30);
      g_test_assert_expected_messages ();
      g_assert_cmpfloat (adw_spin_row_get_value (row), ==, sizes[i]);
      g_autoptr (PangoFontDescription) font =
          pango_font_description_from_string ("Sans");
      g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "cannot write *");
      gtk_font_dialog_button_set_font_desc (button, font);
      g_test_assert_expected_messages ();
      g_assert_cmpstr (pango_font_description_get_family (
                           gtk_font_dialog_button_get_font_desc (button)),
                       ==, families[i]);
      GtkWidget *reset = gtk_widget_get_next_sibling (GTK_WIDGET (button));
      GtkWidget *size_reset =
          find_button (GTK_WIDGET (row), _ ("Use default font size"));
      g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "cannot write *");
      g_signal_emit_by_name (reset, "clicked");
      g_test_assert_expected_messages ();
      g_assert_true (gtk_widget_get_visible (reset));
      g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "cannot write *");
      g_signal_emit_by_name (size_reset, "clicked");
      g_test_assert_expected_messages ();
      g_assert_cmpfloat (adw_spin_row_get_value (row), ==, sizes[i]);
      g_assert_true (gtk_widget_get_visible (size_reset));
    }
  g_assert_cmpint (g_rmdir (tmp), ==, 0);
  assert_font_size (view, 24);
  assert_font_size (code, 18);
  g_autofree gchar *key = config_get ("api_key");
  g_autofree gchar *theme = config_get ("theme");
  g_assert_cmpstr (key, ==, "font-test-key");
  g_assert_cmpstr (theme, ==, "dark");

  adw_dialog_force_close (dialog);
  on_settings_clicked (NULL, NULL);
  wait_for_layout ();
  dialog = adw_application_window_get_visible_dialog (
      ADW_APPLICATION_WINDOW (app->window));
  for (guint i = 0; i < G_N_ELEMENTS (FONT_SETTINGS); i++)
    {
      GtkFontDialogButton *button = settings_font_button (dialog, titles[i]);
      AdwSpinRow *row = settings_font_size (dialog, size_titles[i]);
      g_assert_cmpfloat (adw_spin_row_get_value (row), ==, sizes[i]);
      g_assert_cmpstr (pango_font_description_get_family (
                           gtk_font_dialog_button_get_font_desc (button)),
                       ==, families[i]);
      GtkWidget *reset = gtk_widget_get_next_sibling (GTK_WIDGET (button));
      GtkWidget *size_reset =
          find_button (GTK_WIDGET (row), _ ("Use default font size"));
      g_assert_true (GTK_IS_BUTTON (reset));
      g_assert_true (gtk_widget_get_visible (reset));
      g_assert_true (gtk_widget_get_visible (size_reset));
      g_signal_emit_by_name (reset, "clicked");
      g_autofree gchar *saved = config_get (FONT_SETTINGS[i].key);
      g_assert_null (saved);
      g_assert_false (gtk_widget_get_visible (reset));
      g_assert_true (gtk_widget_get_visible (size_reset));
      g_assert_cmpfloat (adw_spin_row_get_value (row), ==, sizes[i]);
      g_signal_emit_by_name (size_reset, "clicked");
      g_assert_cmpfloat (adw_spin_row_get_value (row), ==,
                         FONT_SETTINGS[i].default_size);
      g_assert_false (gtk_widget_get_visible (size_reset));
      const gchar *family = pango_font_description_get_family (
          gtk_font_dialog_button_get_font_desc (button));
      g_assert_true (config_set (FONT_SETTINGS[i].key, family));
    }
  adw_dialog_force_close (dialog);
  on_settings_clicked (NULL, NULL);
  wait_for_layout ();
  dialog = adw_application_window_get_visible_dialog (
      ADW_APPLICATION_WINDOW (app->window));
  for (guint i = 0; i < G_N_ELEMENTS (FONT_SETTINGS); i++)
    {
      GtkFontDialogButton *button = settings_font_button (dialog, titles[i]);
      AdwSpinRow *row = settings_font_size (dialog, size_titles[i]);
      GtkWidget *reset = gtk_widget_get_next_sibling (GTK_WIDGET (button));
      GtkWidget *size_reset =
          find_button (GTK_WIDGET (row), _ ("Use default font size"));
      g_assert_false (gtk_widget_get_visible (reset));
      g_assert_false (gtk_widget_get_visible (size_reset));
    }
  g_unlink (path);
  refresh_font_css ();
  teardown ();
}

static void
font_values (void)
{
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
  if (setup_ui ())
    {
      g_assert_true (config_set (
          "message_font", "Bogus\\\"; } .chat-input { font-size: 72px; } /*"));
      refresh_font_css ();
      show_window ();
      assert_font_size (app->input, 14);
      teardown ();
    }
  g_autofree gchar *path = store_config_path ();
  g_unlink (path);
  if (have_display)
    refresh_font_css ();
}

static void
live_font_change (void)
{
  if (!setup_ui ())
    return;
  render_chat ();
  begin_reply (FALSE);
  gtk_widget_add_css_class (app->reply_label, "msg-text");
  GtkWidget *label = app->reply_label;
  GtkWidget *body = app->reply_body;
  GtkWidget *bubble = app->reply_bubble;
  GCancellable *cancel = app->cancel;
  GtkTextBuffer *input = gtk_text_view_get_buffer (GTK_TEXT_VIEW (app->input));
  gtk_text_buffer_set_text (input, "draft while streaming", -1);
  app->chat->edit_from = 0;
  g_string_append (app->answer, "partial answer");
  gtk_label_set_text (GTK_LABEL (label), app->answer->str);
  show_window ();
  g_assert_true (config_set ("message_font_size", "22"));
  apply_font_setting (&FONT_SETTINGS[0]);
  wait_for_layout ();
  g_assert_true (app->busy);
  g_assert_true (app->reply_label == label);
  g_assert_true (app->reply_body == body);
  g_assert_true (app->reply_bubble == bubble);
  g_assert_true (app->cancel == cancel);
  g_assert_cmpuint (app->chat->edit_from, ==, 0);
  g_assert_cmpstr (app->answer->str, ==, "partial answer");
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (label)), ==,
                   "partial answer");
  assert_font_size (label, 22);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds (input, &start, &end);
  g_autofree gchar *draft =
      gtk_text_buffer_get_text (input, &start, &end, FALSE);
  g_assert_cmpstr (draft, ==, "draft while streaming");
  g_autofree gchar *path = store_config_path ();
  g_unlink (path);
  refresh_font_css ();
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
  g_autofree gchar *current = NULL;
  g_autofree gchar *model = NULL;
  g_autofree gchar *effort = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) loaded =
      store_load (app->db, &current, &model, &effort, &error);
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

  g_autofree gchar *current = NULL;
  g_autofree gchar *model = NULL;
  g_autofree gchar *effort = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) loaded =
      store_load (app->db, &current, &model, &effort, &error);
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
  g_autofree gchar *current = NULL;
  g_autofree gchar *model = NULL;
  g_autofree gchar *effort = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) loaded =
      store_load (app->db, &current, &model, &effort, &error);
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
  g_assert_true (save_session ());
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
  if (mode == 1 || mode == 4)
    {
      g_autofree gchar *current = NULL;
      g_autofree gchar *model = NULL;
      g_autofree gchar *effort = NULL;
      g_autoptr (GError) error = NULL;
      g_autoptr (GPtrArray) chats =
          store_load (app->db, &current, &model, &effort, &error);
      g_assert_no_error (error);
      g_assert_cmpuint (chats->len, ==, 1);
      g_assert_cmpstr (current, ==, "42");
    }
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
  g_autofree gchar *current = NULL;
  g_autofree gchar *model = NULL;
  g_autofree gchar *effort = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) loaded =
      store_load (app->db, &current, &model, &effort, &error);
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
  g_autofree gchar *body = request_body (4, NULL, &len, NULL);
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
  g_autofree gchar *body = request_body (2, NULL, &len, &error);
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
  g_autofree gchar *image_only = request_body (0, extra, &len, &error);
  g_assert_no_error (error);
  g_assert_true (json_parser_load_from_data (parser, image_only, len, &error));
  root = json_node_get_object (json_parser_get_root (parser));
  messages = json_object_get_array_member (root, "messages");
  parts = json_object_get_array_member (
      json_array_get_object_element (messages, 0), "content");
  g_assert_cmpuint (json_array_get_length (parts), ==, 2);
  g_free (app->model);
  app->model = g_strdup ("deepseek-v4-pro");
  g_autofree gchar *unsupported = request_body (2, NULL, &len, &error);
  g_assert_null (unsupported);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error (&error);
  g_free (app->model);
  app->model = g_strdup ("deepseek-flash");
  g_assert_cmpint (g_unlink (path), ==, 0);
  g_autofree gchar *missing = request_body (2, NULL, &len, &error);
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
      request_body (text_turns + 12, extra, &len, &error);
  g_assert_no_error (error);
  g_assert_nonnull (below_threshold);
  g_autofree gchar *too_wide =
      request_body (text_turns + 13, extra, &len, &error);
  g_assert_null (too_wide);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);

  json_object_set_array_member (first, "images", image_ids (edge_id, NULL));
  g_autofree gchar *at_limit =
      request_body (text_turns + 600, NULL, &len, &error);
  g_assert_no_error (error);
  g_assert_nonnull (at_limit);
  g_autofree gchar *too_many =
      request_body (text_turns + 599, extra, &len, &error);
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
wait_for_image_import (void)
{
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  while (app->image_cancel != NULL && g_get_monotonic_time () < deadline)
    {
      while (g_main_context_iteration (NULL, FALSE))
        {
        }
      g_usleep (1000);
    }
  g_assert_null (app->image_cancel);
}

static void
clipboard_image (void)
{
  if (!setup_ui ())
    return;
  g_autoptr (GApplication) application =
      g_application_new (NULL, G_APPLICATION_NON_UNIQUE);
  g_application_set_default (application);
  g_autoptr (GdkClipboard) clipboard = g_object_new (
      GDK_TYPE_CLIPBOARD, "display", gtk_widget_get_display (app->input), NULL);
  gulong handler = g_signal_handler_find (app->input, G_SIGNAL_MATCH_FUNC, 0, 0,
                                          NULL, on_input_paste, NULL);
  g_assert_cmpuint (handler, >, 0);
  gdk_clipboard_set_text (clipboard, "plain text");
  g_assert_false (paste_image (clipboard));
  g_assert_null (app->chat->draft_images);

  const guint8 rgba[] = {
    255, 0, 0, 255, 0, 255, 0, 255,
    0, 0, 255, 255, 255, 255, 255, 255,
  };
  g_autoptr (GBytes) pixels = g_bytes_new_static (rgba, sizeof rgba);
  g_autoptr (GdkTexture) texture = gdk_memory_texture_new (
      2, 2, GDK_MEMORY_R8G8B8A8, pixels, 8);
  GdkContentProvider *providers[] = {
    gdk_content_provider_new_typed (GDK_TYPE_TEXTURE, texture),
    gdk_content_provider_new_typed (G_TYPE_STRING, "also text"),
  };
  g_autoptr (GdkContentProvider) combined =
      gdk_content_provider_new_union (providers, G_N_ELEMENTS (providers));
  g_assert_true (gdk_clipboard_set_content (clipboard, combined));
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (app->input));
  gtk_text_buffer_set_text (buffer, "existing draft", -1);
  g_assert_true (paste_image (clipboard));
  wait_for_image_import ();
  JsonArray *images = app->chat->draft_images;
  g_assert_nonnull (images);
  g_assert_cmpuint (json_array_get_length (images), ==, 1);
  const gchar *attached = json_array_get_string_element (images, 0);
  g_assert_true (gtk_widget_get_visible (app->image_bar));
  g_assert_cmpstr (app->chat->draft, ==, "existing draft");
  g_autofree gchar *current = NULL;
  g_autofree gchar *model = NULL;
  g_autofree gchar *effort = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) loaded =
      store_load (app->db, &current, &model, &effort, &error);
  g_assert_no_error (error);
  Chat *saved = g_ptr_array_index (loaded, 0);
  g_assert_cmpstr (json_array_get_string_element (saved->draft_images, 0), ==,
                   attached);
  g_autofree gchar *path = image_path (attached);
  g_autoptr (GFile) file = g_file_new_for_path (path);
  g_autoptr (GBytes) bytes = read_image_bytes (file, NULL, &error);
  g_assert_no_error (error);
  const gchar *mime = NULL;
  g_autoptr (GdkPixbuf) preview = decode_image (bytes, &mime, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (preview);
  g_assert_cmpstr (mime, ==, "image/png");
  g_assert_true (paste_image (clipboard));
  restore_input ();
  while (g_main_context_iteration (NULL, FALSE))
    {
    }
  g_assert_cmpuint (json_array_get_length (images), ==, 1);
  g_autoptr (GdkContentProvider) png =
      gdk_content_provider_new_for_bytes ("image/png", bytes);
  g_assert_true (gdk_clipboard_set_content (clipboard, png));
  g_assert_true (paste_image (clipboard));
  wait_for_image_import ();
  g_assert_true (app->chat->draft_images == images);
  g_assert_cmpuint (json_array_get_length (images), ==, 2);
  g_assert_cmpstr (json_array_get_string_element (images, 0), ==, attached);
  const gchar *second = json_array_get_string_element (images, 1);
  g_assert_cmpstr (second, !=, attached);
  g_autofree gchar *second_path = image_path (second);
  g_autofree guint8 *wide_pixels = g_malloc0 (8193 * 4);
  g_autoptr (GBytes) wide_bytes = g_bytes_new_take (
      g_steal_pointer (&wide_pixels), 8193 * 4);
  g_autoptr (GdkTexture) wide = gdk_memory_texture_new (
      8193, 1, GDK_MEMORY_R8G8B8A8, wide_bytes, 8193 * 4);
  g_autoptr (GTask) task = g_task_new (wide, NULL, NULL, NULL);
  import_texture_thread (task, wide, NULL, NULL);
  g_autofree gchar *invalid = g_task_propagate_pointer (task, &error);
  g_assert_null (invalid);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);
  ImagePick *pick = image_pick_new ();
  g_assert_true (image_pick_current (pick));
  restore_input ();
  g_assert_false (image_pick_current (pick));
  image_pick_free (pick);
  g_assert_cmpint (g_unlink (path), ==, 0);
  g_assert_cmpint (g_unlink (second_path), ==, 0);
  g_autofree gchar *dir = g_path_get_dirname (path);
  g_rmdir (dir);
  teardown ();
  g_application_set_default (NULL);
}

static void
clipboard_image_file (void)
{
  if (!setup_ui ())
    return;
  g_autoptr (GApplication) application =
      g_application_new (NULL, G_APPLICATION_NON_UNIQUE);
  g_application_set_default (application);
  g_autoptr (GdkClipboard) clipboard = g_object_new (
      GDK_TYPE_CLIPBOARD, "display", gtk_widget_get_display (app->input), NULL);
  g_autofree gchar *input = g_build_filename (test_dir, "image.png", NULL);
  g_autoptr (GdkPixbuf) pixbuf =
      gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, 2, 2);
  gdk_pixbuf_fill (pixbuf, 0x33aa55ff);
  g_assert_true (gdk_pixbuf_save (pixbuf, input, "png", NULL, NULL));
  g_autoptr (GFile) file = g_file_new_for_path (input);
  g_autofree gchar *uri = g_file_get_uri (file);
  g_autofree gchar *uris = g_strconcat (uri, "\r\n", uri, "\r\n", NULL);
  g_autoptr (GBytes) uri_bytes = g_bytes_new (uris, strlen (uris));
  g_autoptr (GdkTexture) texture = gdk_texture_new_from_file (file, NULL);
  g_assert_nonnull (texture);
  GdkContentProvider *local_formats[] = {
    gdk_content_provider_new_for_bytes ("text/uri-list", uri_bytes),
    gdk_content_provider_new_typed (GDK_TYPE_TEXTURE, texture),
  };
  g_autoptr (GdkContentProvider) local = gdk_content_provider_new_union (
      local_formats, G_N_ELEMENTS (local_formats));
  g_assert_true (gdk_clipboard_set_content (clipboard, local));
  g_assert_true (paste_image (clipboard));
  wait_for_image_import ();
  JsonArray *images = app->chat->draft_images;
  g_assert_nonnull (images);
  g_assert_cmpuint (json_array_get_length (images), ==, 2);
  const gchar *first_id = json_array_get_string_element (images, 0);
  const gchar *second_id = json_array_get_string_element (images, 1);
  g_autofree gchar *path = image_path (first_id);
  g_autofree gchar *second = image_path (second_id);
  g_assert_cmpstr (path, !=, second);
  g_autofree gchar *original = NULL;
  g_autofree gchar *copy = NULL;
  gsize original_len, copy_len;
  g_assert_true (g_file_get_contents (input, &original, &original_len, NULL));
  g_assert_true (g_file_get_contents (path, &copy, &copy_len, NULL));
  g_assert_cmpmem (original, original_len, copy, copy_len);

  g_autofree gchar *bad = g_build_filename (test_dir, "bad.png", NULL);
  g_assert_true (g_file_set_contents (bad, "not an image", -1, NULL));
  g_autoptr (GFile) bad_file = g_file_new_for_path (bad);
  g_autofree gchar *bad_uri = g_file_get_uri (bad_file);
  g_autofree gchar *mixed = g_strconcat (uri, "\r\n", bad_uri, "\r\n", NULL);
  g_autoptr (GBytes) mixed_bytes = g_bytes_new (mixed, strlen (mixed));
  g_autoptr (GdkContentProvider) mixed_provider =
      gdk_content_provider_new_for_bytes ("text/uri-list", mixed_bytes);
  g_assert_true (gdk_clipboard_set_content (clipboard, mixed_provider));
  g_assert_true (paste_image (clipboard));
  wait_for_image_import ();
  g_assert_cmpuint (json_array_get_length (app->chat->draft_images), ==, 2);

  const gchar *url = "https://example.org/image.png";
  g_autofree gchar *remote_uris = g_strconcat (url, "\r\n", NULL);
  g_autoptr (GBytes) remote = g_bytes_new (remote_uris, strlen (remote_uris));
  GdkContentProvider *providers[] = {
    gdk_content_provider_new_for_bytes ("text/uri-list", remote),
    gdk_content_provider_new_typed (G_TYPE_STRING, url),
  };
  g_autoptr (GdkContentProvider) combined =
      gdk_content_provider_new_union (providers, G_N_ELEMENTS (providers));
  g_assert_true (gdk_clipboard_set_content (clipboard, combined));
  set_busy (TRUE);
  g_assert_true (paste_image (clipboard));
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  while (!*app->chat->draft && g_get_monotonic_time () < deadline)
    {
      while (g_main_context_iteration (NULL, FALSE))
        {
        }
      g_usleep (1000);
    }
  g_assert_cmpstr (app->chat->draft, ==, url);
  set_busy (FALSE);
  g_assert_cmpint (g_unlink (bad), ==, 0);
  g_assert_cmpint (g_unlink (input), ==, 0);
  g_assert_cmpint (g_unlink (path), ==, 0);
  g_assert_cmpint (g_unlink (second), ==, 0);
  g_autofree gchar *dir = g_path_get_dirname (path);
  g_assert_cmpint (g_rmdir (dir), ==, 0);
  teardown ();
  g_application_set_default (NULL);
}

static void
image_lifecycle (void)
{
  if (!setup_ui ())
    return;
  g_autofree gchar *draft = g_uuid_string_random ();
  g_autofree gchar *original = g_uuid_string_random ();
  g_autofree gchar *replacement = g_uuid_string_random ();
  g_autofree gchar *draft_path = image_path (draft);
  g_autofree gchar *original_path = image_path (original);
  g_autofree gchar *replacement_path = image_path (replacement);
  g_autofree gchar *dir = g_path_get_dirname (draft_path);
  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  g_autoptr (GdkPixbuf) pixbuf =
      gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, 2, 2);
  gdk_pixbuf_fill (pixbuf, 0xff0000ff);
  g_assert_true (gdk_pixbuf_save (pixbuf, draft_path, "png", NULL, NULL));
  g_assert_true (gdk_pixbuf_save (pixbuf, original_path, "png", NULL, NULL));
  g_assert_true (gdk_pixbuf_save (pixbuf, replacement_path, "png", NULL, NULL));
  Chat *source = app->chat;
  source->draft_images = image_ids (draft, NULL);
  JsonObject *first = json_array_get_object_element (source->messages, 0);
  json_object_set_array_member (first, "images",
                                image_ids (original, replacement));
  render_chat ();
  g_assert_true (gtk_widget_get_visible (app->image_bar));
  GtkWidget *tile = gtk_widget_get_first_child (app->image_preview);
  g_assert_true (GTK_IS_OVERLAY (tile));
  g_assert_true (GTK_IS_PICTURE (gtk_overlay_get_child (GTK_OVERLAY (tile))));
  GtkWidget *bubble = gtk_widget_get_first_child (app->messages);
  on_edit_message (NULL, bubble);
  g_assert_cmpuint (json_array_get_length (source->edit_images), ==, 2);
  tile = gtk_widget_get_first_child (app->image_preview);
  GtkWidget *remove = gtk_widget_get_last_child (tile);
  g_assert_true (GTK_IS_BUTTON (remove));
  g_signal_emit_by_name (remove, "clicked");
  g_assert_cmpuint (json_array_get_length (source->edit_images), ==, 1);
  g_assert_cmpstr (json_array_get_string_element (source->edit_images, 0), ==,
                   replacement);
  JsonArray *images = image_array_member (first, "images");
  g_assert_cmpuint (json_array_get_length (images), ==, 2);
  on_cancel_edit (NULL, NULL);
  g_assert_null (source->edit_images);
  g_assert_true (*current_images () == source->draft_images);

  bubble = gtk_widget_get_first_child (app->messages);
  on_edit_message (NULL, bubble);
  commit_message ("edited");
  first = json_array_get_object_element (source->messages, 0);
  images = image_array_member (first, "images");
  g_assert_cmpuint (json_array_get_length (images), ==, 2);
  g_assert_cmpstr (json_array_get_string_element (source->draft_images, 0), ==,
                   draft);
  render_chat ();
  bubble = gtk_widget_get_first_child (app->messages);
  on_fork_message (NULL, bubble);
  Chat *fork = app->chat;
  g_assert_cmpuint (json_array_get_length (fork->draft_images), ==, 2);
  on_row_copy (NULL, fork);
  on_remove_image (NULL, GUINT_TO_POINTER (0));
  g_assert_cmpuint (json_array_get_length (fork->draft_images), ==, 2);
  g_assert_cmpuint (json_array_get_length (app->chat->draft_images), ==, 1);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (app->input));
  gtk_text_buffer_set_text (buffer, "", -1);
  g_assert_true (gtk_widget_get_sensitive (app->send));
  app->api_key = g_strdup ("unused");
  g_assert_cmpint (g_unlink (replacement_path), ==, 0);
  guint count = json_array_get_length (app->chat->messages);
  send_message ();
  g_assert_cmpuint (json_array_get_length (app->chat->messages), ==, count);
  on_remove_image (NULL, GUINT_TO_POINTER (0));
  g_assert_null (app->chat->draft_images);
  g_assert_false (gtk_widget_get_visible (app->image_bar));
  g_assert_false (gtk_widget_get_sensitive (app->send));
  g_unlink (draft_path);
  g_unlink (original_path);
  g_rmdir (dir);
  teardown ();
}

static void
image_preview_layout (void)
{
  if (!setup_ui ())
    return;
  g_autofree gchar *id = g_uuid_string_random ();
  app->chat->draft_images = json_array_new ();
  for (guint i = 0; i < 12; i++)
    json_array_add_string_element (app->chat->draft_images, id);
  refresh_images ();
  gtk_window_set_default_size (GTK_WINDOW (app->window), 640, 480);
  fit_input_height ();
  gtk_window_present (GTK_WINDOW (app->window));
  wait_for_layout ();

  GtkScrolledWindow *rail = GTK_SCROLLED_WINDOW (app->image_bar);
  GtkAdjustment *scroll = gtk_scrolled_window_get_hadjustment (rail);
  g_assert_true (gtk_widget_get_mapped (app->image_bar));
  g_assert_cmpint (gtk_widget_get_width (app->window), ==, 640);
  g_assert_cmpfloat (gtk_adjustment_get_page_size (scroll), >, 0);
  g_assert_cmpfloat (gtk_adjustment_get_upper (scroll), >,
                     gtk_adjustment_get_page_size (scroll));
  GtkWidget *overlay = gtk_widget_get_parent (app->input_frame);
  g_assert_true (gtk_widget_get_parent (app->image_bar) ==
                 gtk_widget_get_parent (overlay));
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
  g_autofree gchar *current = NULL;
  g_autofree gchar *model = NULL;
  g_autofree gchar *effort = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) loaded =
      store_load (app->db, &current, &model, &effort, &error);
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
      g_assert_cmpint (sqlite3_exec (app->db, "PRAGMA query_only = ON",
                                     NULL, NULL, NULL), ==, SQLITE_OK);
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
    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING,
                           "cannot save conversations: *");
  g_timeout_add (20, quit_test_application, (gpointer) data);
  gchar *argv[] = { "gchat-shutdown-test", NULL };
  g_assert_cmpint (g_application_run (G_APPLICATION (gtk_app), 1, argv), ==, 0);
  g_assert_cmpuint (app->draft_save_source, ==, 0);
  g_assert_cmpuint (fit_source, ==, 0);
  g_assert_cmpuint (sync_tick, ==, 0);
  if (GPOINTER_TO_INT (data) == 2)
    {
      g_test_assert_expected_messages ();
      g_assert_false (app->closing);
    }
  else
    {
      g_assert_true (app->closing);
      g_autofree gchar *current = NULL;
      g_autofree gchar *model = NULL;
      g_autofree gchar *effort = NULL;
      g_autoptr (GError) error = NULL;
      g_autoptr (GPtrArray) loaded =
          store_load (app->db, &current, &model, &effort, &error);
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
      /* A second close must not attempt another database write. */
      g_assert_cmpint (sqlite3_exec (app->db, "PRAGMA query_only = ON",
                                     NULL, NULL, NULL), ==, SQLITE_OK);
      g_assert_false (on_close (NULL, NULL));
      on_shutdown (G_APPLICATION (gtk_app), NULL);
    }
  teardown ();
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
reasoning_visibility (void)
{
  if (!setup_ui ())
    return;
  begin_reply (FALSE);

  const gchar *events =
      "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"first\"}}]}\n\n"
      "data: {\"choices\":[{\"delta\":{\"reasoning_content\":"
      "\" second\"}}]}\n\n"
      "data: {\"choices\":[{\"delta\":{\"content\":\"answer\"}}]}\n\n"
      "data: [DONE]\n\n";
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
      g_memory_input_stream_new_from_data ("data: [DONE]\n\n", -1, NULL);
  g_autoptr (GDataInputStream) lines = g_data_input_stream_new (body);
  g_data_input_stream_read_line_async (lines, G_PRIORITY_DEFAULT, app->cancel,
                                       stream_line_cb,
                                       g_object_ref (app->cancel));
  /* The stream callback owns this reference until the read ends. */
  (void) g_steal_pointer (&lines);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (app->input));
  gtk_text_buffer_set_text (buffer, "不能丢失的草稿", -1);
  g_assert_cmpint (sqlite3_exec (app->db, "PRAGMA query_only = ON",
                                 NULL, NULL, NULL), ==, SQLITE_OK);
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING,
                         "cannot save conversations: *");
  g_assert_true (on_close (NULL, NULL));
  g_test_assert_expected_messages ();
  g_assert_false (app->closing);
  g_assert_false (app->busy);
  g_assert_cmpstr (app->chat->draft, ==, "不能丢失的草稿");
  g_assert_cmpint (sqlite3_exec (app->db, "PRAGMA query_only = OFF",
                                 NULL, NULL, NULL), ==, SQLITE_OK);
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
  g_test_add_data_func ("/app/prose-layout/light", GINT_TO_POINTER (FALSE),
                        prose_layout);
  g_test_add_data_func ("/app/prose-layout/dark", GINT_TO_POINTER (TRUE),
                        prose_layout);
  g_test_add_func ("/app/action-visibility", action_visibility);
  g_test_add_func ("/app/sse-parse", sse_parser);
  g_test_add_data_func ("/app/sse-stream/multiline", GINT_TO_POINTER (0),
                        sse_stream);
  g_test_add_data_func ("/app/sse-stream/incomplete", GINT_TO_POINTER (1),
                        sse_stream);
  g_test_add_func ("/app/sse-stream/cancel-pending", sse_cancel_pending);
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
  g_test_add_func ("/app/content-width", content_width_layout);
  g_test_add_func ("/app/minimum-window", minimum_window_layout);
  g_test_add_func ("/app/content-builder", content_builder);
  g_test_add_func ("/app/sidebar-builder", sidebar_builder);
  g_test_add_func ("/app/language-settings", language_settings);
  g_test_add_func ("/app/font-settings", font_settings);
  g_test_add_func ("/app/font-values", font_values);
  g_test_add_func ("/app/live-font-change", live_font_change);
  g_test_add_data_func ("/app/selection-spacing/light", GINT_TO_POINTER (0),
                        selection_spacing);
  g_test_add_data_func ("/app/selection-spacing/dark", GINT_TO_POINTER (1),
                        selection_spacing);
  g_test_add_data_func ("/app/reading-rhythm/light", GINT_TO_POINTER (0),
                        reading_rhythm);
  g_test_add_data_func ("/app/reading-rhythm/dark", GINT_TO_POINTER (1),
                        reading_rhythm);
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
  g_test_add_func ("/app/image-payload", image_payload);
  g_test_add_func ("/app/image-payload-limits", image_payload_limits);
  g_test_add_func ("/app/image-import", image_import);
  g_test_add_func ("/app/clipboard-image", clipboard_image);
  g_test_add_func ("/app/clipboard-image-file", clipboard_image_file);
  g_test_add_func ("/app/image-lifecycle", image_lifecycle);
  g_test_add_func ("/app/image-preview-layout", image_preview_layout);
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
      g_build_filename (test_dir, "gchat", "session.db.lock", NULL);
  g_unlink (lock);
  g_autofree gchar *dir = g_build_filename (test_dir, "gchat", NULL);
  g_rmdir (dir);
  g_rmdir (test_dir);
  g_free (test_dir);
  return status;
}
