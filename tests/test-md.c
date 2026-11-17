/* SPDX-FileCopyrightText: 2026 Arthur <arthur63@163.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Include the implementation to check the buffer lifetime without adding a test
   interface to production code. */
#include "config.h"

#include "../md.c"

void
app_toast (const gchar *msg G_GNUC_UNUSED)
{
}

typedef struct
{
  const gchar *markdown, *text, *tag;
  gboolean     active;
} TagCase;

static void
clear_box (GtkWidget *box)
{
  while (g_main_context_iteration (NULL, FALSE))
    {
    }
  while (gtk_widget_get_first_child (box) != NULL)
    gtk_box_remove (GTK_BOX (box), gtk_widget_get_first_child (box));
  while (g_main_context_iteration (NULL, FALSE))
    {
    }
}

static void
text_tags (gconstpointer data)
{
  const TagCase *test = data;
  g_autoptr (GtkWidget) box =
      g_object_ref_sink (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  md_render (GTK_BOX (box), test->markdown);
  gboolean found = FALSE;
  for (GtkWidget *view = gtk_widget_get_first_child (box); view != NULL;
       view = gtk_widget_get_next_sibling (view))
    {
      if (!GTK_IS_TEXT_VIEW (view))
        continue;
      GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
      GtkTextIter start, end;
      gtk_text_buffer_get_bounds (buffer, &start, &end);
      g_autofree gchar *text =
          gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
      const gchar *at = strstr (text, test->text);
      if (at != NULL)
        {
          GtkTextTag *tag = gtk_text_tag_table_lookup (
              gtk_text_buffer_get_tag_table (buffer), test->tag);
          g_assert_nonnull (tag);
          gtk_text_buffer_get_iter_at_offset (
              buffer, &start, g_utf8_pointer_to_offset (text, at));
          g_assert_cmpint (gtk_text_iter_has_tag (&start, tag), ==,
                           test->active);
          found = TRUE;
        }
    }
  g_assert_true (found);
  clear_box (box);
  g_assert_cmpuint (code_buffers != NULL ? code_buffers->len : 0, ==, 0);
}

static void
code_buffers_released (void)
{
  g_autoptr (GtkWidget) box =
      g_object_ref_sink (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  for (guint i = 0; i < 10; i++)
    {
      md_render (GTK_BOX (box), "```c\nint x;\n```\n");
      g_assert_cmpuint (code_buffers->len, ==, 1);
      clear_box (box);
      g_assert_cmpuint (code_buffers->len, ==, 0);
    }
}

static void
open_scopes_released (void)
{
  g_autoptr (GtkWidget) box =
      g_object_ref_sink (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  GtkTextMark *mark = NULL;

  {
    g_auto (MdCtx) ctx = {
      .box = GTK_BOX (box),
      .spans = g_ptr_array_new (),
      .scopes = g_array_new (FALSE, FALSE, sizeof (Scope)),
      .ol_numbers = g_array_new (FALSE, FALSE, sizeof (gint64)),
      .cell = g_string_new (NULL),
      .code = g_string_new (NULL),
    };
    push_scope (&ctx, "list1");
    mark = g_array_index (ctx.scopes, Scope, 0).mark;
    g_object_add_weak_pointer (G_OBJECT (mark), (gpointer *) &mark);
  }

  g_assert_null (mark);
  clear_box (box);
}

typedef struct
{
  const gchar *markdown, *expected;
} TextCase;

static void
list_paragraphs (gconstpointer data)
{
  const TextCase *test = data;
  g_autoptr (GtkWidget) box =
      g_object_ref_sink (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  md_render (GTK_BOX (box), test->markdown);
  GtkWidget *view = gtk_widget_get_first_child (box);
  g_assert_true (GTK_IS_TEXT_VIEW (view));
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds (buffer, &start, &end);
  g_autofree gchar *text =
      gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
  g_assert_cmpstr (text, ==, test->expected);
  clear_box (box);
}

typedef struct
{
  const gchar *markdown, *header, *body, *body_markup;
} TableCase;

static void
table_cells (gconstpointer data)
{
  const TableCase *test = data;
  g_autoptr (GtkWidget) box =
      g_object_ref_sink (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  md_render (GTK_BOX (box), test->markdown);
  GtkWidget *card = gtk_widget_get_first_child (box);
  g_assert_true (GTK_IS_BOX (card));
  GtkWidget *grid = gtk_widget_get_first_child (card);
  g_assert_true (GTK_IS_GRID (grid));
  GtkWidget *header = gtk_grid_get_child_at (GTK_GRID (grid), 0, 0);
  GtkWidget *body = gtk_grid_get_child_at (GTK_GRID (grid), 0, 2);
  g_assert_true (GTK_IS_LABEL (header));
  g_assert_true (GTK_IS_LABEL (body));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (header)), ==, test->header);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (body)), ==, test->body);
  g_assert_nonnull (strstr (gtk_label_get_label (GTK_LABEL (header)), "<b>"));
  if (test->body_markup != NULL)
    g_assert_cmpstr (gtk_label_get_label (GTK_LABEL (body)), ==,
                     test->body_markup);
  clear_box (box);
}

static void
link_targets (gconstpointer data)
{
  const TextCase *test = data;
  g_autoptr (GtkWidget) box =
      g_object_ref_sink (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  md_render (GTK_BOX (box), test->markdown);
  GtkWidget *view = gtk_widget_get_first_child (box);
  g_assert_true (GTK_IS_TEXT_VIEW (view));
  GtkTextIter start;
  gtk_text_buffer_get_start_iter (
      gtk_text_view_get_buffer (GTK_TEXT_VIEW (view)), &start);
  g_autoptr (GSList) tags = gtk_text_iter_get_tags (&start);
  const gchar *href = NULL;
  for (GSList *it = tags; it != NULL; it = it->next)
    if (g_object_get_data (G_OBJECT (it->data), "href") != NULL)
      href = g_object_get_data (G_OBJECT (it->data), "href");
  g_assert_cmpstr (href, ==, test->expected);
  clear_box (box);
}

typedef struct
{
  const gchar *markdown, *content, *display;
} CodeBlockCase;

static void
code_block_content (gconstpointer data)
{
  const CodeBlockCase *test = data;
  g_autoptr (GtkWidget) box =
      g_object_ref_sink (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  md_render (GTK_BOX (box), test->markdown);
  GtkWidget *card = gtk_widget_get_first_child (box);
  g_assert_true (gtk_widget_has_css_class (card, "code-card"));
  GtkWidget *copy =
      gtk_widget_get_last_child (gtk_widget_get_first_child (card));
  g_assert_cmpstr (g_object_get_data (G_OBJECT (copy), "code"), ==,
                   test->content);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (
      GTK_TEXT_VIEW (gtk_widget_get_last_child (card)));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds (buffer, &start, &end);
  g_autofree gchar *text =
      gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
  g_assert_cmpstr (text, ==, test->display);
  clear_box (box);
}

static void
image_markup (void)
{
  g_autoptr (GtkWidget) box =
      g_object_ref_sink (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  md_render (GTK_BOX (box),
             "before ![an *image* &amp; text](file:///tmp/x) after "
             "![](file:///tmp/y)");
  GtkWidget *view = gtk_widget_get_first_child (box);
  g_assert_true (GTK_IS_TEXT_VIEW (view));
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
  GtkTextIter iter;
  gtk_text_buffer_get_iter_at_offset (buffer, &iter, 7);
  GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor (&iter);
  g_assert_nonnull (anchor);
  guint count = 0;
  GtkWidget **widgets = gtk_text_child_anchor_get_widgets (anchor, &count);
  g_assert_cmpuint (count, ==, 1);
  GtkWidget *label = gtk_widget_get_first_child (widgets[0]);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (label)), ==,
                   "[an image & text]");
  g_free (widgets);
  gtk_text_buffer_get_iter_at_offset (buffer, &iter, 15);
  anchor = gtk_text_iter_get_child_anchor (&iter);
  g_assert_nonnull (anchor);
  widgets = gtk_text_child_anchor_get_widgets (anchor, &count);
  g_assert_cmpuint (count, ==, 1);
  label = gtk_widget_get_first_child (widgets[0]);
  g_autofree gchar *empty_alt = g_strdup_printf ("[%s]", _ ("Image"));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (label)), ==, empty_alt);
  g_free (widgets);
  clear_box (box);
}

typedef struct
{
  int width, height;
  int expected_width, expected_height;
} ThumbnailCase;

static void
image_thumbnail_size (gconstpointer data)
{
  const ThumbnailCase *test = data;
  g_autoptr (GdkPixbuf) original = gdk_pixbuf_new (
      GDK_COLORSPACE_RGB, TRUE, 8, test->width, test->height);
  gdk_pixbuf_fill (original, 0xff0000ff);
  g_autoptr (GdkPixbuf) thumbnail = image_thumbnail (original);
  g_assert_nonnull (thumbnail);
  g_assert_cmpint (gdk_pixbuf_get_width (thumbnail), ==, test->expected_width);
  g_assert_cmpint (gdk_pixbuf_get_height (thumbnail), ==,
                   test->expected_height);
}

static void
image_urls (void)
{
  g_assert_true (image_url_allowed ("https://example.com/image.png"));
  g_assert_true (image_url_allowed ("HTTPS://example.com/a?x=1&y=2"));
  g_assert_false (image_url_allowed ("http://example.com/image.png"));
  g_assert_false (image_url_allowed ("file:///tmp/image.png"));
  g_assert_false (image_url_allowed ("data:image/png;base64,abc"));
  g_assert_false (image_url_allowed ("/image.png"));
  g_assert_false (image_url_allowed ("https://user:pass@example.com/x"));
}

typedef struct
{
  GMainLoop *loop;
  GdkPixbuf *pixbuf;
  gboolean   done;
} ImageResult;

static void
image_test_done (GObject      *source G_GNUC_UNUSED,
                 GAsyncResult *result,
                 gpointer      data)
{
  ImageResult *state = data;
  state->pixbuf = g_task_propagate_pointer (G_TASK (result), NULL);
  state->done = TRUE;
  g_main_loop_quit (state->loop);
}

static gboolean
image_test_timeout (gpointer data)
{
  g_main_loop_quit (data);
  return G_SOURCE_REMOVE;
}

typedef struct
{
  GMainLoop *loop;
  GtkWidget *widget;
  gboolean   done;
} ImageWidgetResult;

static gboolean
image_widget_ready (gpointer data)
{
  ImageWidgetResult *state = data;
  if (!GTK_IS_PICTURE (gtk_widget_get_first_child (state->widget)))
    return G_SOURCE_CONTINUE;
  state->done = TRUE;
  g_main_loop_quit (state->loop);
  return G_SOURCE_REMOVE;
}

static void
image_server_response (SoupServer        *server G_GNUC_UNUSED,
                       SoupServerMessage *message,
                       const char        *path,
                       GHashTable        *query G_GNUC_UNUSED,
                       gpointer           data)
{
  GBytes *png = data;
  if (g_str_equal (path, "/redirect"))
    {
      soup_server_message_set_redirect (message, SOUP_STATUS_FOUND, "/ok");
      return;
    }
  soup_server_message_set_status (message, SOUP_STATUS_OK, NULL);
  if (g_str_equal (path, "/invalid"))
    {
      soup_server_message_set_response (message, "image/svg+xml",
                                        SOUP_MEMORY_STATIC, "<svg/>", 6);
      return;
    }
  if (g_str_equal (path, "/truncated"))
    {
      soup_server_message_set_response (message, "image/png",
                                        SOUP_MEMORY_STATIC,
                                        "\x89PNG\r\n\x1a\n", 8);
      return;
    }
  if (g_str_equal (path, "/large"))
    {
      g_autofree gchar *large = g_malloc0 (IMAGE_MAX_BYTES + 1);
      soup_server_message_set_response (message, "image/png", SOUP_MEMORY_COPY,
                                        large, IMAGE_MAX_BYTES + 1);
      return;
    }
  gsize len;
  const gchar *bytes = g_bytes_get_data (png, &len);
  soup_server_message_set_response (message, "image/png", SOUP_MEMORY_COPY,
                                    bytes, len);
}

static void
image_network (void)
{
  g_autoptr (GdkPixbuf) original =
      gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, 640, 480);
  gdk_pixbuf_fill (original, 0xff0000ff);
  gchar *png_data = NULL;
  gsize png_len = 0;
  g_assert_true (gdk_pixbuf_save_to_buffer (original, &png_data, &png_len,
                                            "png", NULL, NULL));
  g_autoptr (GBytes) png = g_bytes_new_take (png_data, png_len);
  g_autoptr (SoupServer) server = soup_server_new (NULL, NULL);
  soup_server_add_handler (server, NULL, image_server_response, png, NULL);
  g_assert_true (soup_server_listen_local (server, 0,
                                           SOUP_SERVER_LISTEN_IPV4_ONLY, NULL));
  GSList *uris = soup_server_get_uris (server);
  g_assert_nonnull (uris);
  g_autofree gchar *base = g_uri_to_string (uris->data);
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);

  const gchar *paths[] = { "ok", "redirect", "invalid", "large", "truncated" };
  for (guint i = 0; i < G_N_ELEMENTS (paths); i++)
    {
      ImageRequest *request = g_new0 (ImageRequest, 1);
      g_weak_ref_init (&request->widget, NULL);
      request->url = g_strconcat (base, paths[i], NULL);
      ImageResult state = { .loop = g_main_loop_new (NULL, FALSE) };
      g_autoptr (GTask) task = g_task_new (NULL, NULL, image_test_done, &state);
      g_task_set_task_data (task, request, image_request_free);
      guint timeout = g_timeout_add_seconds (5, image_test_timeout, state.loop);
      g_task_run_in_thread (task, image_load_thread);
      g_main_loop_run (state.loop);
      g_assert_true (state.done);
      g_source_remove (timeout);
      g_main_loop_unref (state.loop);
      if (i == 0)
        {
          g_assert_nonnull (state.pixbuf);
          g_assert_cmpint (gdk_pixbuf_get_width (state.pixbuf), ==, 320);
          g_assert_cmpint (gdk_pixbuf_get_height (state.pixbuf), ==, 240);
        }
      else
        g_assert_null (state.pixbuf);
      g_clear_object (&state.pixbuf);
    }
  g_autoptr (GtkWidget) container =
      g_object_ref_sink (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  md_render (GTK_BOX (container),
             "before ![description](file:///not-loaded) after");
  GtkWidget *view = gtk_widget_get_first_child (container);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
  GtkTextIter iter;
  gtk_text_buffer_get_iter_at_offset (buffer, &iter, 7);
  GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor (&iter);
  g_assert_nonnull (anchor);
  guint count = 0;
  GtkWidget **widgets = gtk_text_child_anchor_get_widgets (anchor, &count);
  g_assert_cmpuint (count, ==, 1);
  GtkWidget *widget = widgets[0];
  g_free (widgets);
  g_autofree gchar *url = g_strconcat (base, "ok", NULL);
  ImageWidgetResult display = {
    .loop = g_main_loop_new (NULL, FALSE),
    .widget = widget,
  };
  guint timeout = g_timeout_add_seconds (5, image_test_timeout, display.loop);
  g_timeout_add (5, image_widget_ready, &display);
  start_image_request (widget, url);
  g_main_loop_run (display.loop);
  g_assert_true (display.done);
  g_source_remove (timeout);
  g_main_loop_unref (display.loop);
  GtkWidget *picture = gtk_widget_get_first_child (widget);
  g_assert_true (GTK_IS_PICTURE (picture));
  int minimum_width = 0;
  gtk_widget_measure (view, GTK_ORIENTATION_HORIZONTAL, -1, &minimum_width,
                      NULL, NULL, NULL);
  g_assert_cmpint (minimum_width, >=, 320);
  g_assert_cmpstr (gtk_picture_get_alternative_text (GTK_PICTURE (picture)),
                   ==, "description");
  GdkPaintable *paintable = gtk_picture_get_paintable (GTK_PICTURE (picture));
  g_assert_nonnull (paintable);
  g_assert_cmpint (gdk_paintable_get_intrinsic_width (paintable), ==, 320);
  g_assert_cmpint (gdk_paintable_get_intrinsic_height (paintable), ==, 240);
  g_assert_cmpint (gtk_picture_get_content_fit (GTK_PICTURE (picture)), ==,
                   GTK_CONTENT_FIT_SCALE_DOWN);

  gtk_widget_allocate (picture, 800, 600, -1, NULL);
  GtkSnapshot *snapshot = gtk_snapshot_new ();
  GTK_WIDGET_GET_CLASS (picture)->snapshot (picture, snapshot);
  g_autoptr (GskRenderNode) node = gtk_snapshot_free_to_node (snapshot);
  g_assert_nonnull (node);
  graphene_rect_t bounds;
  gsk_render_node_get_bounds (node, &bounds);
  g_assert_cmpfloat (bounds.size.width, ==, 320);
  g_assert_cmpfloat (bounds.size.height, ==, 240);

  gtk_widget_allocate (picture, 160, 120, -1, NULL);
  snapshot = gtk_snapshot_new ();
  GTK_WIDGET_GET_CLASS (picture)->snapshot (picture, snapshot);
  g_clear_pointer (&node, gsk_render_node_unref);
  node = gtk_snapshot_free_to_node (snapshot);
  g_assert_nonnull (node);
  gsk_render_node_get_bounds (node, &bounds);
  g_assert_cmpfloat (bounds.size.width, ==, 160);
  g_assert_cmpfloat (bounds.size.height, ==, 120);

  GtkTextIter start, end;
  gtk_text_buffer_get_bounds (buffer, &start, &end);
  g_autofree gchar *text = gtk_text_buffer_get_text (buffer, &start, &end,
                                                     FALSE);
  g_assert_cmpstr (text, ==, "before  after");
  clear_box (container);
  soup_server_disconnect (server);
}

static void
image_pixel_limit (void)
{
  g_autoptr (GdkPixbufLoader) loader = gdk_pixbuf_loader_new ();
  gboolean too_large = FALSE;
  image_size_prepared (loader, 4000, 4000, &too_large);
  g_assert_false (too_large);
  image_size_prepared (loader, 4001, 4000, &too_large);
  g_assert_true (too_large);
  gdk_pixbuf_loader_close (loader, NULL);
}

static void
image_timeout (void)
{
  g_autoptr (GCancellable) cancel = g_cancellable_new ();
  ImageRequest request = { .cancel = cancel, .timeout = 1 };
  g_assert_false (image_timed_out (&request));
  g_assert_true (g_cancellable_is_cancelled (cancel));
  g_assert_cmpuint (request.timeout, ==, 0);
}

static void
image_cancel_on_destroy (void)
{
  GtkWidget *widget = g_object_ref_sink (
      image_widget ("file:///not-loaded", "alt"));
  start_image_request (widget, "http://127.0.0.1:1/image.png");
  GCancellable *cancel = g_object_ref (
      g_object_get_data (G_OBJECT (widget), "image-cancel"));
  g_object_unref (widget);
  g_assert_true (g_cancellable_is_cancelled (cancel));
  g_object_unref (cancel);
}

static void
language_aliases (void)
{
  static const gchar *const names[][2] = {
    { "js", "js" },      { "javascript", "js" },   { "ts", "typescript" },
    { "py", "python" },  { "python3", "python" },  { "csharp", "c-sharp" },
    { "c#", "c-sharp" }, { "c-sharp", "c-sharp" },
  };
  GtkSourceLanguageManager *manager =
      gtk_source_language_manager_get_default ();
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    {
      GtkSourceLanguage *expected =
          gtk_source_language_manager_get_language (manager, names[i][1]);
      g_assert_nonnull (expected);
      g_assert_true (lookup_language (names[i][0]) == expected);
    }
  g_assert_null (lookup_language (NULL));
  g_assert_null (lookup_language (""));
  g_assert_null (
      lookup_language ("gchat-unknown-language-that-does-not-exist"));
}

int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);
  i18n_init (NULL);
  gtk_disable_setlocale ();
  if (!gtk_init_check ())
    {
      g_test_message ("需要 GTK 显示后端");
      return 77;
    }
  adw_init ();
  static const TagCase cases[] = {
    { "- before\n\n  ```c\n  int x;\n  ```\n\n  after\n", "after", "list1",
      TRUE },
    { "- before\n\n  ```c\n  int x;\n  ```\n", "before", "list1", TRUE },
    { "- before\n\n  ---\n\n  after\n", "after", "list1", TRUE },
    { "- before\n\n  | head |\n  | --- |\n  | body |\n\n  after\n", "after",
      "list1", TRUE },
    { "- outer\n  - inner\n\n    ```c\n    int x;\n    ```\n\n    after\n",
      "after", "list2", TRUE },
    { "> before\n>\n> ```c\n> int x;\n> ```\n>\n> after\n", "after", "quote1",
      TRUE },
    { "- before\n\n  ```c\n  int x;\n  ```\n\n  after\n\n  ```c\n  int y;\n  "
      "```\n\n  last\n",
      "last", "list1", TRUE },
    { "**bold** plain", "bold", "bold", TRUE },
    { "**bold** plain", "plain", "bold", FALSE },
    { "*italic* plain", "italic", "italic", TRUE },
    { "~~deleted~~ plain", "deleted", "strike", TRUE },
    { "`code` plain", "code", "code", TRUE },
    { "**outer *inner* tail** plain", "tail", "bold", TRUE },
    { "**outer *inner* tail** plain", "tail", "italic", FALSE },
  };
  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree gchar *name =
          g_strdup_printf ("/markdown/text-tag/%u", i);
      g_test_add_data_func (name, &cases[i], text_tags);
    }
  static const TextCase lists[] = {
    { "1. **问答与解释**\n   解释概念，分析问题。\n\n2. **写作与润色**\n   "
      "写文章，修改表达。\n",
      "1. 问答与解释\n解释概念，分析问题。\n2. "
      "写作与润色\n写文章，修改表达。" },
    { "- first\n\n- second\n", "• first\n• second" },
    { "- [x] done\n\n- [ ] pending\n", "☑ done\n☐ pending" },
    { "1. first\n2. second\n", "1. first\n2. second" },
    { "1. first\n\n   another paragraph\n\n2. second\n",
      "1. first\nanother paragraph\n2. second" },
    { "- first\n- second\n\noutside\n", "• first\n• second\noutside" },
    { "- outer\n\n  1. inner\n  2. next\n\n  after inner\n\n- last\n",
      "• outer\n1. inner\n2. next\nafter inner\n• last" },
    { "&#0; &#xD800; &#x110000;", "� � �" },
    { "0. first\n1. second\n", "0. first\n1. second" },
    { "3. first\n\n4. second\n", "3. first\n4. second" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (lists); i++)
    {
      g_autofree gchar *name =
          g_strdup_printf ("/markdown/list-paragraphs/%u", i);
      g_test_add_data_func (name, &lists[i], list_paragraphs);
    }
  static const TableCase tables[] = {
    { "| header |\n| --- |\n| **bold *both* tail** ~~old~~ `code` |\n",
      "header", "bold both tail old code",
      "<b>bold <i>both</i> tail</b> <s>old</s> <tt>code</tt>" },
    { "| header |\n| --- |\n| ![alt](https://example.com/a.png) `code` |\n",
      "header", "[alt] code", "[alt] <tt>code</tt>" },
    { "| header |\n| --- |\n| [local](file:///tmp/example) |\n", "header",
      "local", "<span>local</span>" },
    { "| header |\n| --- |\n| [link](https://example.com/?a=1&amp;b=2) |\n",
      "header", "link",
      "<a href=\"https://example.com/?a=1&amp;b=2\">link</a>" },
    { "| **Name** *label* ~~old~~ `code` &amp; <b>literal</b> |\n| --- |\n| "
      "body & <i>literal</i> |\n",
      "Name label old code & <b>literal</b>", "body & <i>literal</i>", NULL },
    { "| header |\n| --- |\n| [link](<https://example.com/\"quoted\"?x=1&y=2>) "
      "|\n",
      "header", "link",
      "<a "
      "href=\"https://example.com/&quot;quoted&quot;?x=1&amp;y=2\">link</a>" },
    { "| [标题](<https://example.com/\"中文\">) |\n| --- |\n| "
      "[链接](<https://example.com/\"中文\">) |\n",
      "标题", "链接",
      "<a href=\"https://example.com/&quot;中文&quot;\">链接</a>" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (tables); i++)
    {
      g_autofree gchar *name =
          g_strdup_printf ("/markdown/table/%u", i);
      g_test_add_data_func (name, &tables[i], table_cells);
    }
  static const TextCase links[] = {
    { "[web](https://example.com/?a=1&amp;b=2)",
      "https://example.com/?a=1&b=2" },
    { "[web](HTTP://example.com)", "HTTP://example.com" },
    { "[mail](mailto:user@example.com)", "mailto:user@example.com" },
    { "[local](file:///tmp/example)", NULL },
    { "[app](custom-app://open)", NULL },
    { "[script](javascript:alert)", NULL },
    { "[empty]()", NULL },
    { "[relative](/path)", NULL },
  };
  for (guint i = 0; i < G_N_ELEMENTS (links); i++)
    {
      g_autofree gchar *name =
          g_strdup_printf ("/markdown/link/%u", i);
      g_test_add_data_func (name, &links[i], link_targets);
    }
  static const CodeBlockCase code_blocks[] = {
    { "```text\nline\n```\n", "line\n", "line" },
    { "```text\nline  \n\n```\n", "line  \n\n", "line  \n" },
    { "```text\nfirst\n\nlast  \n```\n", "first\n\nlast  \n",
      "first\n\nlast  " },
    { "```text\nline  ", "line  \n", "line  " },
    { "```text\r\nline\r\n```\r\n", "line\n", "line" },
    { "```text\n```\n", "", "" },
    { "```text\n\n```\n", "\n", "" },
    { "```text\n\n\n```\n", "\n\n", "\n" },
    { "    line  \r\n", "line  \n", "line  " },
    { "<div>\r\ntext\r\n</div>\r\n", "<div>\ntext\n</div>\n",
      "<div>\ntext\n</div>" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (code_blocks); i++)
    {
      g_autofree gchar *name =
          g_strdup_printf ("/markdown/code-block/content/%u", i);
      g_test_add_data_func (name, &code_blocks[i], code_block_content);
    }
  static const ThumbnailCase thumbnails[] = {
    { 1200, 600, 320, 160 },
    { 600, 1200, 120, 240 },
    { 100, 50, 100, 50 },
    { 320, 240, 320, 240 },
    { 1, 16000, 1, 240 },
    { 16000, 1, 320, 1 },
  };
  for (guint i = 0; i < G_N_ELEMENTS (thumbnails); i++)
    {
      g_autofree gchar *name =
          g_strdup_printf ("/markdown/image/thumbnail/%u", i);
      g_test_add_data_func (name, &thumbnails[i], image_thumbnail_size);
    }
  g_test_add_func ("/markdown/image/markup", image_markup);
  g_test_add_func ("/markdown/image/urls", image_urls);
  g_test_add_func ("/markdown/image/network", image_network);
  g_test_add_func ("/markdown/image/pixel-limit", image_pixel_limit);
  g_test_add_func ("/markdown/image/timeout", image_timeout);
  g_test_add_func ("/markdown/image/cancel", image_cancel_on_destroy);
  g_test_add_func ("/markdown/language-aliases", language_aliases);
  g_test_add_func ("/markdown/code-buffers-released", code_buffers_released);
  g_test_add_func ("/markdown/open-scopes-released", open_scopes_released);
  return g_test_run ();
}
