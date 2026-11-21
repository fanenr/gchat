/* SPDX-FileCopyrightText: 2026 Arthur <arthur63@163.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "md.h"
#include "i18n.h"

#include <adwaita.h>
#include <gtksourceview/gtksource.h>
#include <libsoup/soup.h>
#include <math.h>
#include <md4c.h>

static void
escape_into (GString     *out,
             const gchar *s,
             gsize        n)
{
  for (gsize i = 0; i < n; i++)
    {
      switch (s[i])
        {
        case '&':
          g_string_append (out, "&amp;");
          break;
        case '<':
          g_string_append (out, "&lt;");
          break;
        case '>':
          g_string_append (out, "&gt;");
          break;
        default:
          g_string_append_c (out, s[i]);
        }
    }
}

/* md4c hands entities over as they are; output an unknown one literally. */
static const struct
{
  const gchar *name;
  const gchar *utf8;
} ENTITIES[] = {
  { "amp", "&" },    { "lt", "<" },     { "gt", ">" },
  { "quot", "\"" },  { "apos", "'" },   { "nbsp", "\xc2\xa0" },
  { "hellip", "…" }, { "mdash", "—" },  { "ndash", "–" },
  { "laquo", "«" },  { "raquo", "»" },  { "times", "×" },
  { "divide", "÷" }, { "middot", "·" }, { "deg", "°" },
  { "plusmn", "±" }, { "ldquo", "“" },  { "rdquo", "”" },
  { "lsquo", "‘" },  { "rsquo", "’" },  { "bull", "•" },
  { "copy", "©" },   { "reg", "®" },    { "trade", "™" },
  { "dagger", "†" }, { "check", "✓" },
};

static void
entity_text (GString     *out,
             const gchar *s,
             gsize        n)
{
  if (n > 3 && s[0] == '&' && s[n - 1] == ';')
    {
      g_autofree gchar *body = g_strndup (s + 1, n - 2);
      guint32 codepoint = 0;
      if (body[0] == '#')
        {
          if (body[1] == 'x' || body[1] == 'X')
            codepoint = (guint32) g_ascii_strtoull (body + 2, NULL, 16);
          else
            codepoint = (guint32) g_ascii_strtoull (body + 1, NULL, 10);
        }
      if (codepoint && g_unichar_validate (codepoint))
        {
          gchar utf8[8];
          gint len = g_unichar_to_utf8 (codepoint, utf8);
          g_string_append_len (out, utf8, len);
          return;
        }
      if (body[0] == '#')
        {
          g_string_append (out, "\xef\xbf\xbd");
          return;
        }
      for (guint i = 0; i < G_N_ELEMENTS (ENTITIES); i++)
        {
          if (g_str_equal (body, ENTITIES[i].name))
            {
              g_string_append (out, ENTITIES[i].utf8);
              return;
            }
        }
    }
  g_string_append_len (out, s, (gssize) n);
}

static gchar *
attribute_text (const MD_ATTRIBUTE *attribute)
{
  g_autoptr (GString) href = g_string_new (NULL);
  for (guint i = 0;
       attribute->size && attribute->substr_offsets[i] < attribute->size; i++)
    {
      guint start = attribute->substr_offsets[i];
      guint size = attribute->substr_offsets[i + 1] - start;
      if (attribute->substr_types[i] == MD_TEXT_ENTITY)
        entity_text (href, attribute->text + start, size);
      else if (attribute->substr_types[i] == MD_TEXT_NULLCHAR)
        g_string_append (href, "\xef\xbf\xbd");
      else
        g_string_append_len (href, attribute->text + start, size);
    }
  return g_string_free (g_steal_pointer (&href), FALSE);
}

/* Markdown must not launch a local file or an arbitrary app scheme. */
static gchar *
link_href (const MD_ATTRIBUTE *attribute)
{
  g_autofree gchar *href = attribute_text (attribute);
  const gchar *scheme = g_uri_peek_scheme (href);
  if (scheme != NULL && (g_ascii_strcasecmp (scheme, "https") == 0 ||
                         g_ascii_strcasecmp (scheme, "http") == 0 ||
                         g_ascii_strcasecmp (scheme, "mailto") == 0))
    return g_steal_pointer (&href);
  return NULL;
}

/* ---------- code rendering ---------- */

/* The style hook and weak callbacks keep this registry for process lifetime. */
static GPtrArray *code_buffers;

static GtkSourceStyleScheme *
current_scheme (void)
{
  const gchar *id =
      adw_style_manager_get_dark (adw_style_manager_get_default ())
          ? "Adwaita-dark"
          : "Adwaita";
  return gtk_source_style_scheme_manager_get_scheme (
      gtk_source_style_scheme_manager_get_default (), id);
}

static void
on_code_buffer_gone (gpointer data G_GNUC_UNUSED,
                     GObject *buffer)
{
  g_ptr_array_remove_fast (code_buffers, buffer);
}

static void
on_dark_changed (GObject    *manager G_GNUC_UNUSED,
                 GParamSpec *pspec G_GNUC_UNUSED,
                 gpointer    data G_GNUC_UNUSED)
{
  GtkSourceStyleScheme *scheme = current_scheme ();
  for (guint i = 0; i < code_buffers->len; i++)
    gtk_source_buffer_set_style_scheme (g_ptr_array_index (code_buffers, i),
                                        scheme);
}

static void
ensure_style_hook (void)
{
  static gboolean installed = FALSE;
  if (installed)
    return;
  installed = TRUE;
  code_buffers = g_ptr_array_new ();
  g_signal_connect (adw_style_manager_get_default (), "notify::dark",
                    G_CALLBACK (on_dark_changed), NULL);
}

/* The language name in a fence is not always a GtkSourceView id. */
static const gchar *
language_id (const gchar *lang)
{
  static const gchar *const ALIASES[][2] = {
    { "py", "python" },     { "python3", "python" }, { "bash", "sh" },
    { "shell", "sh" },      { "zsh", "sh" },         { "yml", "yaml" },
    { "c++", "cpp" },       { "golang", "go" },      { "javascript", "js" },
    { "ts", "typescript" }, { "csharp", "c-sharp" }, { "c#", "c-sharp" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (ALIASES); i++)
    if (g_str_equal (lang, ALIASES[i][0]))
      return ALIASES[i][1];
  return lang;
}

static GtkSourceLanguage *
lookup_language (const gchar *lang)
{
  if (lang == NULL || !*lang)
    return NULL;
  GtkSourceLanguageManager *manager =
      gtk_source_language_manager_get_default ();
  GtkSourceLanguage *found =
      gtk_source_language_manager_get_language (manager, language_id (lang));
  if (found == NULL)
    found = gtk_source_language_manager_get_language (manager, lang);
  return found;
}

typedef struct
{
  GtkAdjustment *adjustment;
  gboolean horizontal;
} HorizontalScroll;

static void
horizontal_scroll_begin (GtkEventControllerScroll *controller G_GNUC_UNUSED,
                         HorizontalScroll *scroll)
{
  scroll->horizontal = FALSE;
}

static void
horizontal_scroll_end (GtkEventControllerScroll *controller G_GNUC_UNUSED,
                       HorizontalScroll *scroll)
{
  scroll->horizontal = FALSE;
}

static gboolean
horizontal_scroll (GtkEventControllerScroll *controller G_GNUC_UNUSED,
                   double delta_x,
                   double delta_y,
                   HorizontalScroll *scroll)
{
  if (!scroll->horizontal && fabs (delta_x) > fabs (delta_y) * 1.2)
    scroll->horizontal = TRUE;
  if (!scroll->horizontal)
    return GDK_EVENT_PROPAGATE;

  gtk_adjustment_set_value (
      scroll->adjustment,
      gtk_adjustment_get_value (scroll->adjustment) + delta_x);
  return GDK_EVENT_STOP;
}

static GtkWidget *
horizontal_scroll_area (GtkWidget *child)
{
  GtkWidget *area = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkScrolledWindow *scroller =
      GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  gtk_scrolled_window_set_policy (scroller, GTK_POLICY_EXTERNAL,
                                  GTK_POLICY_NEVER);
  gtk_scrolled_window_set_child (scroller, child);
  gtk_box_append (GTK_BOX (area), GTK_WIDGET (scroller));

  HorizontalScroll *scroll = g_new0 (HorizontalScroll, 1);
  scroll->adjustment = gtk_scrolled_window_get_hadjustment (scroller);
  GtkEventController *controller = gtk_event_controller_scroll_new (
      GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
  g_signal_connect (controller, "scroll-begin",
                    G_CALLBACK (horizontal_scroll_begin), scroll);
  g_signal_connect (controller, "scroll", G_CALLBACK (horizontal_scroll),
                    scroll);
  g_signal_connect (controller, "scroll-end",
                    G_CALLBACK (horizontal_scroll_end), scroll);
  g_object_set_data_full (G_OBJECT (area), "horizontal-scroll", scroll, g_free);
  gtk_widget_add_controller (area, controller);
  return area;
}

static GtkWidget *
code_view (const gchar *lang,
           const gchar *code)
{
  ensure_style_hook ();

  GtkSourceLanguage *language = lookup_language (lang);
  g_autoptr (GtkSourceBuffer) buffer =
      language != NULL ? gtk_source_buffer_new_with_language (language)
                       : gtk_source_buffer_new (NULL);
  gtk_source_buffer_set_highlight_syntax (buffer, language != NULL);
  gtk_source_buffer_set_highlight_matching_brackets (buffer, FALSE);
  gtk_source_buffer_set_style_scheme (buffer, current_scheme ());
  /* md4c emits LF only; drop one trailing newline in the display and keep the
     full text for copy. */
  gsize display_length = strlen (code);
  if (display_length && code[display_length - 1] == '\n')
    display_length--;
  gtk_text_buffer_set_text (GTK_TEXT_BUFFER (buffer), code,
                            (gint) display_length);
  g_object_weak_ref (G_OBJECT (buffer), on_code_buffer_gone, NULL);
  g_ptr_array_add (code_buffers, buffer);

  GtkWidget *view = gtk_source_view_new_with_buffer (buffer);
  gtk_text_view_set_editable (GTK_TEXT_VIEW (view), FALSE);
  gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (view), FALSE);
  gtk_source_view_set_show_line_numbers (GTK_SOURCE_VIEW (view), FALSE);
  gtk_source_view_set_highlight_current_line (GTK_SOURCE_VIEW (view), FALSE);
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (view), TRUE);
  gtk_source_view_set_tab_width (GTK_SOURCE_VIEW (view), 4);
  gtk_widget_add_css_class (view, "source-code");
  g_object_set (view, "left-margin", 16, "right-margin", 16, "top-margin", 16,
                "bottom-margin", 16, NULL);
  return view;
}

static void
on_copy_code (GtkButton *button,
              gpointer   data G_GNUC_UNUSED)
{
  const gchar *code = g_object_get_data (G_OBJECT (button), "code");
  if (code != NULL)
    {
      gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (button)),
                              code);
      app_toast (_ ("Code copied"));
    }
}

static GtkWidget *
code_card (const gchar *lang,
           const gchar *code)
{
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (card, "code-card");

  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class (bar, "code-bar");

  GtkWidget *lang_label = gtk_label_new (lang != NULL && *lang ? lang : "code");
  gtk_widget_add_css_class (lang_label, "caption");
  gtk_widget_set_hexpand (lang_label, TRUE);
  gtk_label_set_xalign (GTK_LABEL (lang_label), 0);

  GtkWidget *copy = gtk_button_new_from_icon_name ("edit-copy-symbolic");
  gtk_widget_add_css_class (copy, "flat");
  gtk_widget_add_css_class (copy, "compact-action");
  gtk_widget_set_tooltip_text (copy, _ ("Copy code"));
  gtk_image_set_pixel_size (
      GTK_IMAGE (gtk_button_get_child (GTK_BUTTON (copy))), 13);
  gtk_widget_set_valign (copy, GTK_ALIGN_CENTER);
  g_object_set_data_full (G_OBJECT (copy), "code", g_strdup (code), g_free);
  g_signal_connect (copy, "clicked", G_CALLBACK (on_copy_code), NULL);

  gtk_box_append (GTK_BOX (bar), lang_label);
  gtk_box_append (GTK_BOX (bar), copy);

  gtk_box_append (GTK_BOX (card), bar);
  GtkWidget *view = code_view (lang, code);
  gtk_box_append (GTK_BOX (card), horizontal_scroll_area (view));
  return card;
}

/* ---------- remote images ---------- */

#define IMAGE_MAX_BYTES (8 * 1024 * 1024)
#define IMAGE_MAX_PIXELS (16 * 1000 * 1000)
#define IMAGE_DISPLAY_WIDTH 320
#define IMAGE_DISPLAY_HEIGHT 240

typedef struct
{
  GWeakRef      widget;
  gchar        *url;
  GCancellable *cancel;
  guint         timeout;
} ImageRequest;

static void
image_request_free (gpointer data)
{
  ImageRequest *request = data;
  g_weak_ref_clear (&request->widget);
  g_free (request->url);
  g_clear_object (&request->cancel);
  g_free (request);
}

static void
cancel_image (gpointer data)
{
  GCancellable *cancel = data;
  g_cancellable_cancel (cancel);
  g_object_unref (cancel);
}

static gboolean
image_timed_out (gpointer data)
{
  ImageRequest *request = data;
  request->timeout = 0;
  g_cancellable_cancel (request->cancel);
  return G_SOURCE_REMOVE;
}

static gboolean
image_url_allowed (const gchar *url)
{
  g_autoptr (GUri) uri = g_uri_parse (url, G_URI_FLAGS_NONE, NULL);
  return uri != NULL && g_uri_get_scheme (uri) != NULL &&
         g_ascii_strcasecmp (g_uri_get_scheme (uri), "https") == 0 &&
         g_uri_get_host (uri) != NULL && *g_uri_get_host (uri) != '\0' &&
         g_uri_get_userinfo (uri) == NULL;
}

static gboolean
image_format_allowed (const guchar *data,
                      gsize         len)
{
  return (len >= 8 && memcmp (data, "\x89PNG\r\n\x1a\n", 8) == 0) ||
         (len >= 3 && memcmp (data, "\xff\xd8\xff", 3) == 0) ||
         (len >= 6 && (memcmp (data, "GIF87a", 6) == 0 ||
                       memcmp (data, "GIF89a", 6) == 0)) ||
         (len >= 12 && memcmp (data, "RIFF", 4) == 0 &&
          memcmp (data + 8, "WEBP", 4) == 0);
}

static void
image_size_prepared (GdkPixbufLoader *loader,
                     gint             width,
                     gint             height,
                     gpointer         data)
{
  gboolean *too_large = data;
  if (width <= 0 || height <= 0 ||
      (guint64) width * (guint64) height > IMAGE_MAX_PIXELS)
    {
      *too_large = TRUE;
      gdk_pixbuf_loader_set_size (loader, 1, 1);
    }
}

/* Return an owned thumbnail without enlarging the source. */
static GdkPixbuf *
image_thumbnail (GdkPixbuf *pixbuf)
{
  int width = gdk_pixbuf_get_width (pixbuf);
  int height = gdk_pixbuf_get_height (pixbuf);
  double scale = MIN (1.0, MIN ((double) IMAGE_DISPLAY_WIDTH / width,
                                (double) IMAGE_DISPLAY_HEIGHT / height));
  if (scale == 1.0)
    return g_object_ref (pixbuf);
  return gdk_pixbuf_scale_simple (pixbuf, MAX (1, (int) (width * scale)),
                                  MAX (1, (int) (height * scale)),
                                  GDK_INTERP_BILINEAR);
}

/* Decode outside the GTK thread and bound both the response and image size. */
static void
image_load_thread (GTask        *task,
                   gpointer      source G_GNUC_UNUSED,
                   gpointer      task_data,
                   GCancellable *cancel)
{
  ImageRequest *request = task_data;
  g_autoptr (SoupSession) session = soup_session_new ();
  g_autoptr (SoupMessage) message =
      soup_message_new (SOUP_METHOD_GET, request->url);
  g_autoptr (GInputStream) stream = NULL;
  g_autoptr (GByteArray) bytes = g_byte_array_new ();
  g_autoptr (GdkPixbufLoader) loader = NULL;
  GdkPixbuf *pixbuf = NULL;
  gboolean too_large = FALSE;
  guchar chunk[16384];
  gssize count;

  if (message == NULL)
    goto done;
  soup_session_set_timeout (session, 15);
  soup_message_set_flags (message, SOUP_MESSAGE_NO_REDIRECT |
                                       SOUP_MESSAGE_DO_NOT_USE_AUTH_CACHE);
  stream = soup_session_send (session, message, cancel, NULL);
  if (stream == NULL || !SOUP_STATUS_IS_SUCCESSFUL (
                            soup_message_get_status (message)))
    goto done;
  if (soup_message_headers_get_content_length (
          soup_message_get_response_headers (message)) > IMAGE_MAX_BYTES)
    goto done;
  while ((count = g_input_stream_read (stream, chunk, sizeof chunk, cancel,
                                       NULL)) > 0)
    {
      if (bytes->len + (gsize) count > IMAGE_MAX_BYTES)
        goto done;
      g_byte_array_append (bytes, chunk, (guint) count);
    }
  if (count < 0 || !image_format_allowed (bytes->data, bytes->len))
    goto done;
  loader = gdk_pixbuf_loader_new ();
  g_signal_connect (loader, "size-prepared", G_CALLBACK (image_size_prepared),
                    &too_large);
  gboolean written =
      gdk_pixbuf_loader_write (loader, bytes->data, bytes->len, NULL);
  gboolean closed = gdk_pixbuf_loader_close (loader, NULL);
  if (!written || !closed || too_large)
    goto done;
  GdkPixbuf *original = gdk_pixbuf_loader_get_pixbuf (loader);
  if (original != NULL)
    pixbuf = image_thumbnail (original);

done:
  g_task_return_pointer (task, pixbuf, g_object_unref);
}

static void
image_loaded (GObject      *source G_GNUC_UNUSED,
              GAsyncResult *result,
              gpointer      data G_GNUC_UNUSED)
{
  GTask *task = G_TASK (result);
  ImageRequest *request = g_task_get_task_data (task);
  g_clear_handle_id (&request->timeout, g_source_remove);
  g_autoptr (GdkPixbuf) pixbuf = g_task_propagate_pointer (task, NULL);
  g_autoptr (GtkWidget) box = g_weak_ref_get (&request->widget);
  if (pixbuf == NULL || box == NULL)
    return;

  g_autoptr (GBytes) pixels = gdk_pixbuf_read_pixel_bytes (pixbuf);
  GdkMemoryFormat format = gdk_pixbuf_get_has_alpha (pixbuf)
                               ? GDK_MEMORY_R8G8B8A8
                               : GDK_MEMORY_R8G8B8;
  int width = gdk_pixbuf_get_width (pixbuf);
  int height = gdk_pixbuf_get_height (pixbuf);
  g_autoptr (GdkTexture) texture = gdk_memory_texture_new (
      width, height, format, pixels, gdk_pixbuf_get_rowstride (pixbuf));
  GtkWidget *picture = gtk_picture_new_for_paintable (GDK_PAINTABLE (texture));
  const gchar *alt = g_object_get_data (G_OBJECT (box), "image-alt");
  gtk_picture_set_alternative_text (GTK_PICTURE (picture), alt);
  gtk_picture_set_can_shrink (GTK_PICTURE (picture), TRUE);
  gtk_picture_set_content_fit (GTK_PICTURE (picture),
                               GTK_CONTENT_FIT_SCALE_DOWN);
  gtk_widget_set_size_request (picture, width, height);
  gtk_box_remove (GTK_BOX (box), gtk_widget_get_first_child (box));
  gtk_box_append (GTK_BOX (box), picture);
  gtk_widget_queue_resize (box);
}

static void
start_image_request (GtkWidget   *box,
                     const gchar *url)
{
  GCancellable *cancel = g_cancellable_new ();
  ImageRequest *request = g_new0 (ImageRequest, 1);
  g_weak_ref_init (&request->widget, box);
  request->url = g_strdup (url);
  request->cancel = g_object_ref (cancel);
  request->timeout = g_timeout_add_seconds (20, image_timed_out, request);
  g_object_set_data_full (G_OBJECT (box), "image-cancel", cancel,
                          cancel_image);
  g_autoptr (GTask) task = g_task_new (NULL, cancel, image_loaded, NULL);
  g_task_set_task_data (task, request, image_request_free);
  g_task_run_in_thread (task, image_load_thread);
}

static GtkWidget *
image_widget (const gchar *url,
              const gchar *alt)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  const gchar *description = *alt != '\0' ? alt : _ ("Image");
  g_autofree gchar *fallback = g_strdup_printf ("[%s]", description);
  gtk_box_append (GTK_BOX (box), gtk_label_new (fallback));
  gtk_widget_set_tooltip_text (box, description);
  g_object_set_data_full (G_OBJECT (box), "image-alt", g_strdup (description),
                          g_free);
  if (image_url_allowed (url))
    start_image_request (box, url);
  return box;
}

/* ---------- table cell ---------- */

static GtkWidget *
cell_label (const gchar *markup,
            MD_ALIGN     align,
            gboolean     header)
{
  g_autofree gchar *text =
      header ? g_strdup_printf ("<span weight=\"medium\">%s</span>", markup)
             : g_strdup (markup);
  GtkWidget *label = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (label), text);
  gtk_label_set_xalign (GTK_LABEL (label), align == MD_ALIGN_CENTER  ? 0.5
                                           : align == MD_ALIGN_RIGHT ? 1
                                                                     : 0);
  gtk_label_set_selectable (GTK_LABEL (label), TRUE);
  gtk_widget_add_css_class (label, "msg-text");
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_halign (label, GTK_ALIGN_FILL);
  g_object_set (label, "margin-start", 16, "margin-end", 16, "margin-top", 10,
                "margin-bottom", 10, NULL);
  return label;
}

/* Prose lives in one read-only GtkTextView per message, so a mouse drag
   selects across paragraphs. Code cards and tables are separate widgets
   between views. */

typedef struct
{
  GtkTextView parent_instance;
} MdTextView;

typedef GtkTextViewClass MdTextViewClass;

typedef struct
{
  gboolean         selecting;
  graphene_point_t pointer;

  guint            scroll_tick;
  gint64           scroll_time;
} MdTextViewPrivate;

static GType md_text_view_get_type (void);

G_DEFINE_TYPE_WITH_PRIVATE (MdTextView, md_text_view, GTK_TYPE_TEXT_VIEW)

static gboolean
scroll_selection (GtkTextView            *view,
                  GtkWidget              *viewport,
                  const graphene_point_t *pointer,
                  double                  distance)
{
  int height = gtk_widget_get_height (viewport);
  if (pointer->y >= 0 && pointer->y < height)
    return FALSE;

  graphene_rect_t bounds;
  graphene_point_t local;
  if (!gtk_widget_compute_bounds (GTK_WIDGET (view), viewport, &bounds) ||
      !gtk_widget_compute_point (viewport, GTK_WIDGET (view), pointer, &local))
    {
      return FALSE;
    }

  gboolean above = pointer->y < 0;
  double remaining = above ? -bounds.origin.y
                           : bounds.origin.y + bounds.size.height - height;
  /* Leave a small margin beyond the text block. */
  remaining += 48;
  if (remaining <= 0)
    return FALSE;

  GtkAdjustment *adj =
      gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (viewport));
  double old = gtk_adjustment_get_value (adj);
  double delta = MIN (distance, remaining) * (above ? -1 : 1);
  gtk_adjustment_set_value (adj, old + delta);
  delta = gtk_adjustment_get_value (adj) - old;
  if (delta == 0)
    return FALSE;

  /* The viewport applies the new offset on its next allocation. */
  int x, y;
  gtk_text_view_window_to_buffer_coords (view, GTK_TEXT_WINDOW_WIDGET,
                                         (int) local.x, (int) (local.y + delta),
                                         &x, &y);
  GtkTextIter end;
  gtk_text_view_get_iter_at_location (view, &end, x, y);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (view);
  gtk_text_buffer_move_mark (buffer, gtk_text_buffer_get_insert (buffer), &end);
  return TRUE;
}

static gboolean
selection_scroll_tick (GtkWidget     *widget,
                       GdkFrameClock *clock,
                       gpointer       data G_GNUC_UNUSED)
{
  MdTextViewPrivate *priv = md_text_view_get_instance_private (
      (MdTextView *) widget);
  GtkWidget *viewport = gtk_widget_get_ancestor (widget, GTK_TYPE_VIEWPORT);
  gint64 now = gdk_frame_clock_get_frame_time (clock);
  gint64 elapsed = MIN (now - priv->scroll_time, 50 * G_TIME_SPAN_MILLISECOND);
  double distance = elapsed * (600.0 / G_USEC_PER_SEC);
  priv->scroll_time = now;
  if (gtk_widget_has_focus (widget) && viewport != NULL &&
      (distance == 0 || scroll_selection (GTK_TEXT_VIEW (widget), viewport,
                                          &priv->pointer, distance)))
    {
      return G_SOURCE_CONTINUE;
    }

  priv->scroll_tick = 0;
  return G_SOURCE_REMOVE;
}

static void
stop_selection_scroll (MdTextView *view)
{
  MdTextViewPrivate *priv = md_text_view_get_instance_private (view);
  priv->selecting = FALSE;
  if (priv->scroll_tick != 0)
    {
      gtk_widget_remove_tick_callback (GTK_WIDGET (view), priv->scroll_tick);
      priv->scroll_tick = 0;
    }
}

static gboolean
selection_event_point (GdkEvent         *event,
                       GtkWidget        *widget,
                       graphene_point_t *point)
{
  GtkNative *native = gtk_widget_get_native (widget);
  GdkSurface *surface = gdk_event_get_surface (event);
  double x, y, nx, ny;
  if (native == NULL || gtk_native_get_surface (native) != surface ||
      !gdk_event_get_position (event, &x, &y))
    {
      return FALSE;
    }

  gtk_native_get_surface_transform (native, &nx, &ny);
  graphene_point_t pos = GRAPHENE_POINT_INIT (x - nx, y - ny);
  return gtk_widget_compute_point (GTK_WIDGET (native), widget, &pos, point);
}

static gboolean
starts_selection (GtkTextView *view,
                  GdkEvent    *event)
{
  if (gdk_event_triggers_context_menu (event))
    return FALSE;

  GtkTextBuffer *buffer = gtk_text_view_get_buffer (view);
  GtkTextIter start, end;
  if ((gdk_event_get_modifier_state (event) & GDK_SHIFT_MASK) ||
      !gtk_text_buffer_get_selection_bounds (buffer, &start, &end))
    {
      return TRUE;
    }

  /* GTK drags the selected text rather than extending it from this press. */
  graphene_point_t point;
  if (!selection_event_point (event, GTK_WIDGET (view), &point))
    return FALSE;
  int x, y;
  gtk_text_view_window_to_buffer_coords (view, GTK_TEXT_WINDOW_WIDGET,
                                         (int) point.x, (int) point.y, &x, &y);
  GtkTextIter iter;
  gtk_text_view_get_iter_at_location (view, &iter, x, y);
  return !gtk_text_iter_in_range (&iter, &start, &end);
}

static gboolean
on_selection_event (GtkEventControllerLegacy *controller G_GNUC_UNUSED,
                    GdkEvent                 *event,
                    MdTextView               *view)
{
  GtkWidget *widget = GTK_WIDGET (view);
  MdTextViewPrivate *priv = md_text_view_get_instance_private (view);
  GdkEventType type = gdk_event_get_event_type (event);
  if (type == GDK_GRAB_BROKEN ||
      (type == GDK_BUTTON_RELEASE &&
       gdk_button_event_get_button (event) == GDK_BUTTON_PRIMARY))
    {
      stop_selection_scroll (view);
    }
  else if (type == GDK_BUTTON_PRESS &&
           gdk_button_event_get_button (event) == GDK_BUTTON_PRIMARY)
    {
      stop_selection_scroll (view);
      priv->selecting = starts_selection (GTK_TEXT_VIEW (view), event);
    }
  else if (type == GDK_MOTION_NOTIFY && priv->selecting)
    {
      GtkWidget *viewport = gtk_widget_get_ancestor (widget, GTK_TYPE_VIEWPORT);
      if (!(gdk_event_get_modifier_state (event) & GDK_BUTTON1_MASK))
        stop_selection_scroll (view);
      else if (viewport != NULL &&
               selection_event_point (event, viewport, &priv->pointer) &&
               (priv->pointer.y < 0 ||
                priv->pointer.y >= gtk_widget_get_height (viewport)) &&
               priv->scroll_tick == 0)
        {
          GdkFrameClock *clock = gtk_widget_get_frame_clock (widget);
          priv->scroll_time = gdk_frame_clock_get_frame_time (clock);
          priv->scroll_tick = gtk_widget_add_tick_callback (
              widget, selection_scroll_tick, NULL, NULL);
        }
    }
  return GDK_EVENT_PROPAGATE;
}

static gboolean
on_extend_selection (GtkTextView            *view,
                     GtkTextExtendSelection  granularity G_GNUC_UNUSED,
                     const GtkTextIter      *location G_GNUC_UNUSED,
                     GtkTextIter            *start G_GNUC_UNUSED,
                     GtkTextIter            *end G_GNUC_UNUSED,
                     gpointer                data G_GNUC_UNUSED)
{
  /* Leave word and line selection to GTK. */
  stop_selection_scroll ((MdTextView *) view);
  return FALSE;
}

static void
md_text_view_unmap (GtkWidget *widget)
{
  stop_selection_scroll ((MdTextView *) widget);
  GTK_WIDGET_CLASS (md_text_view_parent_class)->unmap (widget);
}

static void
snapshot_slice (GtkSnapshot   *snapshot,
                GskRenderNode *node,
                int            width,
                int            top,
                int            bottom)
{
  if (bottom <= top)
    return;
  graphene_rect_t clip = GRAPHENE_RECT_INIT (0, top, width, bottom - top);
  gtk_snapshot_push_clip (snapshot, &clip);
  gtk_snapshot_append_node (snapshot, node);
  gtk_snapshot_pop (snapshot);
}

static void
md_text_view_snapshot (GtkWidget   *widget,
                       GtkSnapshot *snapshot)
{
  GtkTextView *view = GTK_TEXT_VIEW (widget);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (view);
  GtkWidgetClass *parent = GTK_WIDGET_CLASS (md_text_view_parent_class);
  GtkTextIter start, end;
  if (!gtk_text_buffer_get_selection_bounds (buffer, &start, &end))
    {
      parent->snapshot (widget, snapshot);
      return;
    }

  GtkSnapshot *original = gtk_snapshot_new ();
  parent->snapshot (widget, original);
  g_autoptr (GskRenderNode) node = gtk_snapshot_free_to_node (original);
  if (node == NULL)
    return;

  int width = gtk_widget_get_width (widget);
  int height = gtk_widget_get_height (widget);
  int top = 0;
  int last_line = gtk_text_iter_get_line (&end);
  gtk_text_iter_set_line_offset (&start, 0);
  do
    {
      int gap = gtk_text_view_get_pixels_below_lines (view);
      g_autoptr (GSList) tags = gtk_text_iter_get_tags (&start);
      for (GSList *it = tags; it != NULL; it = it->next)
        {
          gboolean set;
          int pixels;
          g_object_get (it->data, "pixels-below-lines-set", &set,
                        "pixels-below-lines", &pixels, NULL);
          if (set)
            gap = pixels;
        }
      if (gap == 0)
        continue;

      int y, line_height;
      gtk_text_view_get_line_yrange (view, &start, &y, &line_height);
      gtk_text_view_buffer_to_window_coords (view, GTK_TEXT_WINDOW_WIDGET,
                                             0, y, NULL, &y);
      /* GTK includes paragraph spacing in the last line's selection. */
      int gap_top = CLAMP (y + line_height - gap, 0, height);
      int gap_bottom = CLAMP (y + line_height, 0, height);
      snapshot_slice (snapshot, node, width, top, gap_top);
      top = gap_bottom;
    }
  while (gtk_text_iter_forward_line (&start) &&
         gtk_text_iter_get_line (&start) <= last_line);
  snapshot_slice (snapshot, node, width, top, height);
}

static void
md_text_view_class_init (MdTextViewClass *klass)
{
  GTK_WIDGET_CLASS (klass)->snapshot = md_text_view_snapshot;
  GTK_WIDGET_CLASS (klass)->unmap = md_text_view_unmap;
}

static void
md_text_view_init (MdTextView *view)
{
  /* Observe the drag without competing with GTK's selection gesture. */
  GtkEventController *events = gtk_event_controller_legacy_new ();
  gtk_event_controller_set_propagation_phase (events, GTK_PHASE_CAPTURE);
  g_signal_connect (events, "event", G_CALLBACK (on_selection_event), view);
  gtk_widget_add_controller (GTK_WIDGET (view), events);
  g_signal_connect (view, "extend-selection", G_CALLBACK (on_extend_selection),
                    NULL);
}

typedef struct
{
  gchar       *name;
  GtkTextMark *mark;
} Scope;

typedef struct
{
  GtkBox         *box;
  GtkTextView    *view;
  GtkTextBuffer  *buffer;

  GPtrArray      *spans;  /* Track open inline tags. */
  GArray         *scopes; /* Track open paragraph scopes for list items,
                             headings, and quotes. */
  GArray         *ol_numbers;
  int             list_depth;
  guint           item_number;
  int             quote_depth;
  int             p_quote_depth;
  int             last_block;

  gboolean        in_cell;
  GString        *cell;
  GString        *image_alt;
  gchar          *image_src;
  guint           image_depth;
  GString        *code;
  gchar          *code_lang;
  MD_ALIGN        cell_align;
  gboolean        cell_header;

  GtkGrid        *grid;
  unsigned        col_count;
  int             row_slot;
  int             col;
} MdCtx;

static void pop_scope (MdCtx *ctx);

static void
md_ctx_clear (MdCtx *ctx)
{
  while (ctx->scopes->len)
    pop_scope (ctx);
  g_ptr_array_unref (ctx->spans);
  g_array_unref (ctx->scopes);
  g_array_unref (ctx->ol_numbers);
  g_string_free (ctx->cell, TRUE);
  if (ctx->image_alt != NULL)
    g_string_free (ctx->image_alt, TRUE);
  g_free (ctx->image_src);
  g_string_free (ctx->code, TRUE);
  g_free (ctx->code_lang);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (MdCtx, md_ctx_clear)

static void
on_link_clicked (GtkGestureClick *gesture G_GNUC_UNUSED,
                 int              n_press G_GNUC_UNUSED,
                 double           x,
                 double           y,
                 gpointer         data)
{
  GtkTextView *view = data;
  /* Releasing a drag selection that started on a link must not open the
     browser. */
  if (gtk_text_buffer_get_has_selection (gtk_text_view_get_buffer (view)))
    return;
  int bx, by;
  GtkTextIter iter;
  gtk_text_view_window_to_buffer_coords (view, GTK_TEXT_WINDOW_WIDGET, (int) x,
                                         (int) y, &bx, &by);
  gtk_text_view_get_iter_at_location (view, &iter, bx, by);

  g_autoptr (GSList) tags = gtk_text_iter_get_tags (&iter);
  for (GSList *l = tags; l != NULL; l = l->next)
    {
      const gchar *href = g_object_get_data (G_OBJECT (l->data), "href");
      if (href == NULL)
        continue;
      GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (view));
      g_autoptr (GtkUriLauncher) launcher = gtk_uri_launcher_new (href);
      gtk_uri_launcher_launch (launcher,
                               GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL,
                               NULL, NULL, NULL);
      break;
    }
}

static gboolean
resize_widget_cb (gpointer widget)
{
  gtk_widget_queue_resize (GTK_WIDGET (widget));
  return G_SOURCE_REMOVE;
}

/* The content height can change during size allocation; requeue the layout so
   the measure cache really invalidates. */
static void
defer_resize (GtkWidget *widget)
{
  g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, resize_widget_cb,
                   g_object_ref (widget), g_object_unref);
}

static GtkTextTag *get_tag (MdCtx       *ctx,
                            const gchar *name);

static void
ensure_view (MdCtx *ctx)
{
  if (ctx->view != NULL)
    return;

  GtkWidget *view = g_object_new (md_text_view_get_type (), NULL);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_editable (GTK_TEXT_VIEW (view), FALSE);
  gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (view), FALSE);
  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (view), 0);
  gtk_text_view_set_right_margin (GTK_TEXT_VIEW (view), 0);
  gtk_text_view_set_top_margin (GTK_TEXT_VIEW (view), 0);
  gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (view), 0);
  gtk_widget_set_vexpand (view, FALSE);
  gtk_widget_add_css_class (view, "msg-text");
  g_signal_connect_object (
      gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (view)), "changed",
      G_CALLBACK (defer_resize), view, G_CONNECT_SWAPPED);

  GtkGesture *click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click),
                                 GDK_BUTTON_PRIMARY);
  gtk_gesture_single_set_exclusive (GTK_GESTURE_SINGLE (click), FALSE);
  g_signal_connect (click, "released", G_CALLBACK (on_link_clicked), view);
  gtk_widget_add_controller (view, GTK_EVENT_CONTROLLER (click));

  gtk_box_append (GTK_BOX (ctx->box), view);
  ctx->view = GTK_TEXT_VIEW (view);
  ctx->buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
  GtkTextIter start;
  gtk_text_buffer_get_start_iter (ctx->buffer, &start);
  for (guint i = 0; i < ctx->scopes->len; i++)
    {
      Scope *scope = &g_array_index (ctx->scopes, Scope, i);
      get_tag (ctx, scope->name);
      scope->mark =
          gtk_text_buffer_create_mark (ctx->buffer, NULL, &start, TRUE);
    }
}

static GtkTextTag *
get_tag (MdCtx       *ctx,
         const gchar *name)
{
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table (ctx->buffer);
  GtkTextTag *tag = gtk_text_tag_table_lookup (table, name);
  if (tag != NULL)
    return tag;

  tag = gtk_text_buffer_create_tag (ctx->buffer, name, NULL);
  GdkRGBA grey = { 0.5, 0.5, 0.5, 1.0 };

  if (g_str_equal (name, "bold"))
    {
      g_object_set (tag, "weight", PANGO_WEIGHT_SEMIBOLD, NULL);
    }
  else if (g_str_equal (name, "italic"))
    {
      g_object_set (tag, "style", PANGO_STYLE_ITALIC, NULL);
    }
  else if (g_str_equal (name, "strike"))
    {
      g_object_set (tag, "strikethrough", TRUE, NULL);
    }
  else if (g_str_equal (name, "code"))
    {
      g_object_set (tag, "family", "monospace", "scale", 0.94, NULL);
    }
  else if (g_str_equal (name, "dim"))
    {
      g_object_set (tag, "foreground-rgba", &grey, NULL);
    }
  else if (g_str_has_prefix (name, "h"))
    {
      int level = atoi (name + 1);
      double scale = level == 1   ? 1.5
                     : level == 2 ? 1.375
                     : level == 3 ? 1.25
                                  : 1;
      g_object_set (tag, "scale", scale, "weight",
                    level <= 3 ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_SEMIBOLD,
                    NULL);
    }
  else if (g_str_has_prefix (name, "gap"))
    {
      g_object_set (tag, "pixels-below-lines", atoi (name + 3), NULL);
    }
  else if (g_str_has_prefix (name, "item"))
    {
      gchar *end;
      int margin = (int) g_ascii_strtoll (name + 4, &end, 10);
      int width = atoi (end + 1);
      g_object_set (tag, "left-margin", margin + width, NULL);
    }
  else if (g_str_has_prefix (name, "marker"))
    {
      gchar *end;
      int margin = (int) g_ascii_strtoll (name + 6, &end, 10);
      int width = atoi (end + 1);
      g_object_set (tag, "left-margin", margin, "indent", -width,
                    "foreground-rgba", &grey, NULL);
    }
  else if (g_str_has_prefix (name, "quote"))
    {
      int depth = atoi (name + 5);
      g_object_set (tag, "left-margin", 10 * depth, "foreground-rgba", &grey,
                    NULL);
    }
  return tag;
}

/* Inserts text and applies the open inline tags (plus one extra tag) to it.
   A mark keeps the range valid across the insertion. */
static void
insert_text (MdCtx       *ctx,
             const gchar *text,
             gsize        n,
             GtkTextTag  *extra)
{
  ensure_view (ctx);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter (ctx->buffer, &end);

  GtkTextMark *start =
      gtk_text_buffer_create_mark (ctx->buffer, NULL, &end, TRUE);
  gtk_text_buffer_insert (ctx->buffer, &end, text, (gint) n);

  GtkTextIter from, to;
  gtk_text_buffer_get_iter_at_mark (ctx->buffer, &from, start);
  gtk_text_buffer_get_end_iter (ctx->buffer, &to);
  for (guint i = 0; i < ctx->spans->len; i++)
    gtk_text_buffer_apply_tag (ctx->buffer, g_ptr_array_index (ctx->spans, i),
                               &from, &to);
  if (extra != NULL)
    gtk_text_buffer_apply_tag (ctx->buffer, extra, &from, &to);
  gtk_text_buffer_delete_mark (ctx->buffer, start);
}

/* Ends the previous block with one line break, spaced by the given gap tag. */
static void
break_line (MdCtx       *ctx,
            const gchar *gap_tag)
{
  ensure_view (ctx);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter (ctx->buffer, &end);
  if (gtk_text_iter_is_start (&end))
    return;

  GtkTextIter probe = end;
  if (gtk_text_iter_backward_char (&probe) &&
      gtk_text_iter_get_char (&probe) == '\n')
    {
      return;
    }
  int line = gtk_text_iter_get_line (&end);
  insert_text (ctx, "\n", 1, NULL);
  /* Paragraph spacing is read at the line start, not at the newline. */
  gtk_text_buffer_get_iter_at_line (ctx->buffer, &probe, line);
  gtk_text_buffer_get_end_iter (ctx->buffer, &end);
  gtk_text_buffer_apply_tag (ctx->buffer, get_tag (ctx, gap_tag), &probe,
                             &end);
}

static void
push_scope (MdCtx       *ctx,
            const gchar *name)
{
  ensure_view (ctx);
  get_tag (ctx, name);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter (ctx->buffer, &end);
  Scope scope = { g_strdup (name),
                  gtk_text_buffer_create_mark (ctx->buffer, NULL, &end, TRUE) };
  g_array_append_val (ctx->scopes, scope);
}

static void
finish_scope (MdCtx *ctx,
              Scope *scope)
{
  if (scope->mark == NULL)
    return;
  GtkTextIter start, end;
  gtk_text_buffer_get_iter_at_mark (ctx->buffer, &start, scope->mark);
  gtk_text_buffer_get_end_iter (ctx->buffer, &end);
  GtkTextTag *tag = get_tag (ctx, scope->name);
  /* Heading size and weight take precedence over inline emphasis. */
  if (g_str_has_prefix (scope->name, "h"))
    {
      GtkTextTagTable *table = gtk_text_buffer_get_tag_table (ctx->buffer);
      gtk_text_tag_set_priority (tag, gtk_text_tag_table_get_size (table) - 1);
    }
  gtk_text_buffer_apply_tag (ctx->buffer, tag, &start, &end);
  gtk_text_buffer_delete_mark (ctx->buffer, scope->mark);
  scope->mark = NULL;
}

static void
pop_scope (MdCtx *ctx)
{
  if (ctx->scopes->len == 0)
    return;
  Scope *scope = &g_array_index (ctx->scopes, Scope, ctx->scopes->len - 1);
  finish_scope (ctx, scope);
  g_free (scope->name);
  g_array_remove_index (ctx->scopes, ctx->scopes->len - 1);
}

static void
flush_view (MdCtx *ctx)
{
  /* The scope keeps names across widgets; a mark and a tag belong to the
     current text buffer only. */
  for (guint i = 0; i < ctx->scopes->len; i++)
    finish_scope (ctx, &g_array_index (ctx->scopes, Scope, i));
  ctx->view = NULL;
  ctx->buffer = NULL;
}

static void
append_card (MdCtx     *ctx,
             GtkWidget *card)
{
  flush_view (ctx);
  gtk_box_append (GTK_BOX (ctx->box), card);
}

/* ---------- md4c callbacks ---------- */

static void
table_separator (MdCtx *ctx)
{
  GtkWidget *sep = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (sep, "table-sep");
  gtk_widget_set_size_request (sep, -1, 1);
  gtk_grid_attach (ctx->grid, sep, 0, ctx->row_slot, (gint) ctx->col_count, 1);
}

static int
marker_width (GtkWidget   *view,
              const gchar *prefix)
{
  /* Match GtkTextView's separate marker run, including symbol fallback. */
  g_autofree gchar *probe = g_strconcat (prefix, "x", NULL);
  g_autoptr (PangoLayout) layout =
      gtk_widget_create_pango_layout (view, probe);
  g_autoptr (PangoAttrList) attrs = pango_attr_list_new ();
  PangoAttribute *height = pango_attr_line_height_new (1);
  height->end_index = (guint) strlen (prefix);
  pango_attr_list_insert (attrs, height);
  pango_layout_set_attributes (layout, attrs);
  PangoRectangle position;
  pango_layout_index_to_pos (layout, (int) strlen (prefix), &position);
  return PANGO_PIXELS (position.x);
}

static int
on_enter_block (MD_BLOCKTYPE type,
                void        *detail,
                void        *ud)
{
  MdCtx *ctx = ud;

  switch (type)
    {
    case MD_BLOCK_QUOTE:
      ctx->quote_depth++;
      break;

    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
      {
        break_line (ctx, ctx->list_depth > 0             ? "gap4"
                         : ctx->last_block == MD_BLOCK_H ? "gap8"
                                                         : "gap16");
        /* -1 means an unordered list; 0 is a valid ordered list start. */
        gint64 start = -1;
        if (type == MD_BLOCK_OL && detail != NULL)
          start = ((MD_BLOCK_OL_DETAIL *) detail)->start;
        g_array_append_val (ctx->ol_numbers, start);
        ctx->list_depth++;
      }
      break;

    case MD_BLOCK_LI:
      {
        MD_BLOCK_LI_DETAIL *li = detail;
        gchar marker[16];
        gint64 *counter =
            &g_array_index (ctx->ol_numbers, gint64, ctx->list_depth - 1);
        if (li != NULL && li->is_task)
          {
            g_strlcpy (marker,
                       (li->task_mark == 'x' || li->task_mark == 'X')
                           ? "☑"
                           : "☐",
                       sizeof marker);
          }
        else if (*counter >= 0)
          {
            g_snprintf (marker, sizeof marker, "%" G_GINT64_FORMAT ".",
                        (*counter)++);
          }
        else
          {
            g_strlcpy (marker, "•", sizeof marker);
          }

        break_line (ctx, "gap6");
        g_autofree gchar *prefix = g_strdup_printf ("%s ", marker);
        int width = marker_width (GTK_WIDGET (ctx->view), prefix);
        int margin = 0;
        for (guint i = ctx->scopes->len; i > 0; i--)
          {
            Scope *scope = &g_array_index (ctx->scopes, Scope, i - 1);
            if (g_str_has_prefix (scope->name, "item"))
              {
                g_object_get (get_tag (ctx, scope->name), "left-margin",
                              &margin, NULL);
                margin += 12;
                break;
              }
          }
        /* Unique tags keep nested margins above their parent tags. */
        g_autofree gchar *item_tag =
            g_strdup_printf ("item%d-%d-%u", margin, width, ctx->item_number);
        push_scope (ctx, item_tag);
        g_autofree gchar *marker_tag =
            g_strdup_printf ("marker%d-%d-%u", margin, width,
                             ctx->item_number++);
        insert_text (ctx, prefix, strlen (prefix), get_tag (ctx, marker_tag));
      }
      break;

    case MD_BLOCK_P:
      if (ctx->in_cell)
        break;
      /* The first paragraph of a list item follows the number; later
         paragraphs keep their own line. */
      if (ctx->list_depth == 0 || ctx->last_block != MD_BLOCK_LI)
        break_line (ctx, ctx->list_depth > 0 ? "gap8" : "gap16");
      if (ctx->quote_depth > 0)
        {
          for (int i = 0; i < ctx->quote_depth; i++)
            insert_text (ctx, "| ", 2, get_tag (ctx, "dim"));
          g_autofree gchar *tag_name =
              g_strdup_printf ("quote%d", ctx->quote_depth);
          push_scope (ctx, tag_name);
          ctx->p_quote_depth = ctx->quote_depth;
        }
      break;

    case MD_BLOCK_H:
      {
        unsigned level =
            detail != NULL ? ((MD_BLOCK_H_DETAIL *) detail)->level : 2;
        break_line (ctx, ctx->last_block == MD_BLOCK_H ? "gap8"
                         : level <= 3                  ? "gap24"
                                                       : "gap16");
        g_autofree gchar *tag_name = g_strdup_printf ("h%u", level);
        push_scope (ctx, tag_name);
      }
      break;

    case MD_BLOCK_HR:
      {
        GtkWidget *rule = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
        gtk_widget_set_margin_bottom (rule, 4);
        if (ctx->last_block == MD_BLOCK_TABLE)
          {
            GtkWidget *area =
                gtk_widget_get_last_child (GTK_WIDGET (ctx->box));
            GtkScrolledWindow *scroller =
                GTK_SCROLLED_WINDOW (gtk_widget_get_first_child (area));
            GtkWidget *viewport = gtk_scrolled_window_get_child (scroller);
            GtkWidget *card = gtk_viewport_get_child (GTK_VIEWPORT (viewport));
            gtk_box_append (GTK_BOX (card), rule);
          }
        else
          {
            gtk_widget_set_margin_top (rule, 4);
            append_card (ctx, rule);
          }
      }
      break;

    case MD_BLOCK_CODE:
    case MD_BLOCK_HTML:
      {
        g_autofree gchar *lang = NULL;
        if (type == MD_BLOCK_HTML)
          {
            lang = g_strdup ("html");
          }
        else
          {
            MD_BLOCK_CODE_DETAIL *code = detail;
            if (code != NULL && code->lang.text != NULL && code->lang.size)
              lang = g_strndup (code->lang.text, code->lang.size);
          }
        g_string_truncate (ctx->code, 0);
        g_free (ctx->code_lang);
        ctx->code_lang = g_steal_pointer (&lang);
      }
      break;

    case MD_BLOCK_TABLE:
      {
        MD_BLOCK_TABLE_DETAIL *table = detail;
        GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
        ctx->grid = GTK_GRID (gtk_grid_new ());
        ctx->col_count = table != NULL ? table->col_count : 1;
        ctx->row_slot = 0;
        gtk_box_append (GTK_BOX (card), GTK_WIDGET (ctx->grid));
        GtkWidget *area = horizontal_scroll_area (card);
        gtk_widget_add_css_class (area, "table-card");
        append_card (ctx, area);
      }
      break;

    case MD_BLOCK_TR:
      ctx->col = 0;
      if (ctx->row_slot > 0)
        {
          table_separator (ctx);
          ctx->row_slot++;
        }
      break;

    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
      ctx->in_cell = TRUE;
      ctx->cell_header = (type == MD_BLOCK_TH);
      ctx->cell_align = detail != NULL
                            ? ((MD_BLOCK_TD_DETAIL *) detail)->align
                            : MD_ALIGN_DEFAULT;
      g_string_truncate (ctx->cell, 0);
      break;

    default:
      break;
    }

  ctx->last_block = (int) type;
  return 0;
}

static int
on_leave_block (MD_BLOCKTYPE type,
                void        *detail G_GNUC_UNUSED,
                void        *ud)
{
  MdCtx *ctx = ud;

  switch (type)
    {
    case MD_BLOCK_QUOTE:
      ctx->quote_depth--;
      break;

    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
      g_array_remove_index (ctx->ol_numbers, ctx->ol_numbers->len - 1);
      ctx->list_depth--;
      break;

    case MD_BLOCK_LI:
    case MD_BLOCK_H:
      pop_scope (ctx);
      break;

    case MD_BLOCK_P:
      if (ctx->p_quote_depth)
        {
          pop_scope (ctx);
          ctx->p_quote_depth = 0;
        }
      break;

    case MD_BLOCK_CODE:
    case MD_BLOCK_HTML:
      {
        append_card (ctx, code_card (ctx->code_lang, ctx->code->str));
        g_clear_pointer (&ctx->code_lang, g_free);
        g_string_truncate (ctx->code, 0);
      }
      break;

    case MD_BLOCK_TABLE:
      ctx->grid = NULL;
      break;

    case MD_BLOCK_TR:
      ctx->row_slot++;
      break;

    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
      gtk_grid_attach (
          ctx->grid,
          cell_label (ctx->cell->str, ctx->cell_align, ctx->cell_header),
          ctx->col, ctx->row_slot, 1, 1);
      ctx->col++;
      g_string_truncate (ctx->cell, 0);
      ctx->in_cell = FALSE;
      break;

    default:
      break;
    }
  ctx->last_block = (int) type;
  return 0;
}

typedef struct
{
  const gchar *tag, *open, *close;
} SpanStyle;

static const SpanStyle SPAN_STYLES[] = {
  [MD_SPAN_EM] = { "italic", "<i>", "</i>" },
  [MD_SPAN_STRONG] = { "bold", "<span weight=\"semibold\">", "</span>" },
  [MD_SPAN_DEL] = { "strike", "<s>", "</s>" },
  [MD_SPAN_CODE] = { "code", "<tt>", "</tt>" },
};

static int
on_enter_span (MD_SPANTYPE type,
               void       *detail,
               void       *ud)
{
  MdCtx *ctx = ud;
  GString *cell = ctx->cell;

  if (ctx->image_depth > 0)
    {
      if (type == MD_SPAN_IMG)
        ctx->image_depth++;
      return 0;
    }

  switch (type)
    {
    case MD_SPAN_EM:
    case MD_SPAN_STRONG:
    case MD_SPAN_DEL:
    case MD_SPAN_CODE:
      {
        const SpanStyle *style = &SPAN_STYLES[type];
        if (ctx->in_cell)
          g_string_append (cell, style->open);
        else
          g_ptr_array_add (ctx->spans, get_tag (ctx, style->tag));
      }
      break;
    case MD_SPAN_A:
      {
        MD_SPAN_A_DETAIL *link = detail;
        g_autofree gchar *href = link_href (&link->href);
        if (ctx->in_cell)
          {
            if (href != NULL)
              {
                g_autofree gchar *escaped = g_markup_escape_text (href, -1);
                g_string_append_printf (cell, "<a href=\"%s\">", escaped);
              }
            else
              {
                g_string_append (cell, "<span>");
              }
            break;
          }
        ensure_view (ctx);
        GtkTextTag *tag = gtk_text_buffer_create_tag (ctx->buffer, NULL, NULL);
        if (href != NULL)
          g_object_set (tag, "foreground", "#1c71d8", "underline",
                        PANGO_UNDERLINE_SINGLE, NULL);
        g_object_set_data_full (G_OBJECT (tag), "href",
                                g_steal_pointer (&href), g_free);
        g_ptr_array_add (ctx->spans, tag);
      }
      break;
    case MD_SPAN_IMG:
      if (ctx->in_cell)
        g_string_append (cell, "[");
      else
        {
          MD_SPAN_IMG_DETAIL *image = detail;
          ctx->image_src = attribute_text (&image->src);
          ctx->image_alt = g_string_new (NULL);
          ctx->image_depth = 1;
        }
      break;
    default:
      break;
    }
  return 0;
}

static int
on_leave_span (MD_SPANTYPE type,
               void       *detail,
               void       *ud)
{
  MdCtx *ctx = ud;

  if (ctx->image_depth > 0)
    {
      if (type == MD_SPAN_IMG && --ctx->image_depth == 0)
        {
          ensure_view (ctx);
          GtkTextIter end;
          gtk_text_buffer_get_end_iter (ctx->buffer, &end);
          GtkTextChildAnchor *anchor =
              gtk_text_buffer_create_child_anchor (ctx->buffer, &end);
          GtkWidget *widget = image_widget (ctx->image_src,
                                            ctx->image_alt->str);
          gtk_text_view_add_child_at_anchor (ctx->view, widget, anchor);
          g_clear_pointer (&ctx->image_src, g_free);
          g_string_free (ctx->image_alt, TRUE);
          ctx->image_alt = NULL;
        }
      return 0;
    }

  switch (type)
    {
    case MD_SPAN_EM:
    case MD_SPAN_STRONG:
    case MD_SPAN_DEL:
    case MD_SPAN_CODE:
    case MD_SPAN_A:
      if (ctx->in_cell)
        {
          if (type == MD_SPAN_A)
            {
              g_autofree gchar *href =
                  link_href (&((MD_SPAN_A_DETAIL *) detail)->href);
              g_string_append (ctx->cell, href != NULL ? "</a>" : "</span>");
            }
          else
            {
              g_string_append (ctx->cell, SPAN_STYLES[type].close);
            }
        }
      else if (ctx->spans->len)
        g_ptr_array_remove_index (ctx->spans, ctx->spans->len - 1);
      break;
    case MD_SPAN_IMG:
      if (ctx->in_cell)
        g_string_append (ctx->cell, "]");
      else
        insert_text (ctx, "]", 1, NULL);
      break;
    default:
      break;
    }
  return 0;
}

static int
on_text (MD_TEXTTYPE    type,
         const MD_CHAR *text,
         MD_SIZE        size,
         void          *ud)
{
  MdCtx *ctx = ud;

  if (ctx->last_block == MD_BLOCK_CODE || ctx->last_block == MD_BLOCK_HTML)
    {
      g_string_append_len (ctx->code, text, (gssize) size);
      return 0;
    }

  g_autoptr (GString) decoded = NULL;
  gsize length = size;
  switch (type)
    {
    case MD_TEXT_NORMAL:
    case MD_TEXT_CODE:
    case MD_TEXT_HTML:
      break;
    case MD_TEXT_ENTITY:
      decoded = g_string_new (NULL);
      entity_text (decoded, text, size);
      text = decoded->str;
      length = decoded->len;
      break;
    case MD_TEXT_BR:
    case MD_TEXT_SOFTBR:
      text = "\n";
      length = 1;
      break;
    case MD_TEXT_NULLCHAR:
      text = "\xef\xbf\xbd";
      length = 3;
      break;
    default:
      return 0;
    }
  if (ctx->image_depth > 0)
    g_string_append_len (ctx->image_alt, text, (gssize) length);
  else if (ctx->in_cell)
    escape_into (ctx->cell, text, length);
  else
    insert_text (ctx, text, length, NULL);
  return 0;
}

static void
restore_item_margin (GtkTextTag *tag,
                     gpointer    data)
{
  g_autofree gchar *name = NULL;
  g_object_get (tag, "name", &name, NULL);
  if (name == NULL)
    return;
  gpointer margin = g_hash_table_lookup (data, name);
  if (margin != NULL)
    g_object_set (tag, "left-margin", GPOINTER_TO_INT (margin), NULL);
}

void
md_refresh_fonts (GtkBox *box)
{
  g_autoptr (GHashTable) margins =
      g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (GtkWidget *view = gtk_widget_get_first_child (GTK_WIDGET (box));
       view != NULL; view = gtk_widget_get_next_sibling (view))
    {
      if (!GTK_IS_TEXT_VIEW (view))
        continue;
      GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
      gtk_text_tag_table_foreach (gtk_text_buffer_get_tag_table (buffer),
                                  restore_item_margin, margins);
      GtkTextIter iter;
      gtk_text_buffer_get_start_iter (buffer, &iter);
      do
        {
          GtkTextTag *item = NULL, *parent = NULL, *marker = NULL;
          g_autoptr (GSList) tags = gtk_text_iter_get_tags (&iter);
          /* GTK returns tags in ascending priority, with child items last. */
          for (GSList *l = tags; l != NULL; l = l->next)
            {
              GtkTextTag *tag = l->data;
              g_autofree gchar *name = NULL;
              g_object_get (tag, "name", &name, NULL);
              if (name == NULL)
                continue;
              if (g_str_has_prefix (name, "item"))
                {
                  parent = item;
                  item = tag;
                }
              else if (g_str_has_prefix (name, "marker"))
                marker = tag;
            }
          if (item == NULL || marker == NULL)
            continue;
          GtkTextIter end = iter;
          gtk_text_iter_forward_to_tag_toggle (&end, marker);
          g_autofree gchar *prefix =
              gtk_text_buffer_get_text (buffer, &iter, &end, FALSE);
          int width = marker_width (view, prefix);
          int margin = 0;
          if (parent != NULL)
            {
              g_object_get (parent, "left-margin", &margin, NULL);
              margin += 12;
            }
          g_object_set (marker, "left-margin", margin, "indent", -width,
                        NULL);
          g_object_set (item, "left-margin", margin + width, NULL);
          g_autofree gchar *name = NULL;
          g_object_get (item, "name", &name, NULL);
          g_hash_table_insert (margins, g_steal_pointer (&name),
                               GINT_TO_POINTER (margin + width));
        }
      while (gtk_text_iter_forward_line (&iter));
      defer_resize (view);
    }
}

void
md_render (GtkBox      *box,
           const gchar *text)
{
  g_auto (MdCtx) ctx = {
    .box = box,
    .spans = g_ptr_array_new (),
    .scopes = g_array_new (FALSE, FALSE, sizeof (Scope)),
    .ol_numbers = g_array_new (FALSE, FALSE, sizeof (gint64)),
    .cell = g_string_new (NULL),
    .code = g_string_new (NULL),
  };

  static const MD_PARSER parser = {
    .abi_version = 0,
    .flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS |
             MD_FLAG_PERMISSIVEAUTOLINKS,
    .enter_block = on_enter_block,
    .leave_block = on_leave_block,
    .enter_span = on_enter_span,
    .leave_span = on_leave_span,
    .text = on_text,
    .debug_log = NULL,
    .syntax = NULL,
  };

  md_parse (text != NULL ? text : "",
            (MD_SIZE) strlen (text != NULL ? text : ""), &parser, &ctx);
  while (ctx.scopes->len)
    pop_scope (&ctx);

  /* A view inserted into a mapped container right after creation measures 0x0
     in this frame; requeue the layout for the next frame, or the reply stays
     blank. */
  defer_resize (GTK_WIDGET (box));
}
