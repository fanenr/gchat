/* SPDX-FileCopyrightText: 2026 Arthur <arthur63@163.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "md.h"
#include "i18n.h"

#include <adwaita.h>
#include <gtksourceview/gtksource.h>
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

/* Markdown comes from untrusted content, so it must not launch a local file or
   an arbitrary app scheme. */
static gchar *
link_href (const MD_ATTRIBUTE *attribute)
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
  const gchar *scheme = g_uri_peek_scheme (href->str);
  if (scheme != NULL && (g_ascii_strcasecmp (scheme, "https") == 0 ||
                         g_ascii_strcasecmp (scheme, "http") == 0 ||
                         g_ascii_strcasecmp (scheme, "mailto") == 0))
    {
      return g_string_free (g_steal_pointer (&href), FALSE);
    }
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
  gtk_text_view_set_pixels_below_lines (GTK_TEXT_VIEW (view), 2);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (view), GTK_WRAP_WORD_CHAR);
  gtk_widget_add_css_class (view, "source-code");
  gtk_widget_set_hexpand (view, TRUE);
  g_object_set (view, "margin-start", 12, "margin-end", 12, "margin-top", 8,
                "margin-bottom", 8, NULL);
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
  g_object_set (bar, "margin-start", 12, "margin-end", 8, "margin-top", 4,
                NULL);

  GtkWidget *lang_label = gtk_label_new (lang != NULL && *lang ? lang : "code");
  gtk_widget_add_css_class (lang_label, "caption");
  gtk_widget_add_css_class (lang_label, "dimmed");
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
  gtk_box_append (GTK_BOX (card), code_view (lang, code));
  return card;
}

/* ---------- table cell ---------- */

static GtkWidget *
cell_label (const gchar *markup,
            MD_ALIGN     align,
            gboolean     header)
{
  g_autofree gchar *text =
      header ? g_strdup_printf ("<b>%s</b>", markup) : g_strdup (markup);
  GtkWidget *label = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (label), text);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars (GTK_LABEL (label), 34);
  gtk_label_set_xalign (GTK_LABEL (label), align == MD_ALIGN_CENTER  ? 0.5
                                           : align == MD_ALIGN_RIGHT ? 1
                                                                     : 0);
  gtk_label_set_yalign (GTK_LABEL (label), 0);
  gtk_label_set_selectable (GTK_LABEL (label), TRUE);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_halign (label, GTK_ALIGN_FILL);
  if (header)
    gtk_widget_add_css_class (label, "dimmed");
  g_object_set (label, "margin-start", 12, "margin-end", 12, "margin-top", 8,
                "margin-bottom", 8, NULL);
  return label;
}

/* Prose lives in one read-only GtkTextView per message, so a mouse drag
   selects across paragraphs. Code cards and tables are separate widgets
   between views. */

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
  int             quote_depth;
  int             p_quote_depth;
  int             last_block;

  gboolean        in_cell;
  GString        *cell;
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

  GtkWidget *view = gtk_text_view_new ();
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_editable (GTK_TEXT_VIEW (view), FALSE);
  gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (view), FALSE);
  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (view), 0);
  gtk_text_view_set_right_margin (GTK_TEXT_VIEW (view), 0);
  gtk_text_view_set_top_margin (GTK_TEXT_VIEW (view), 0);
  gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (view), 0);
  gtk_text_view_set_pixels_below_lines (GTK_TEXT_VIEW (view), 3);
  gtk_text_view_set_pixels_inside_wrap (GTK_TEXT_VIEW (view), 1);
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
      g_object_set (tag, "weight", PANGO_WEIGHT_BOLD, NULL);
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
      double scale = g_str_equal (name, "h1")   ? 1.6
                     : g_str_equal (name, "h2") ? 1.35
                                                : 1.15;
      g_object_set (tag, "scale", scale, "weight", PANGO_WEIGHT_BOLD, NULL);
    }
  else if (g_str_has_prefix (name, "gap"))
    {
      g_object_set (tag, "pixels-below-lines", atoi (name + 3), NULL);
    }
  else if (g_str_has_prefix (name, "list"))
    {
      int depth = atoi (name + 4);
      g_object_set (tag, "left-margin", 24 * (depth - 1), NULL);
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
  insert_text (ctx, "\n", 1, get_tag (ctx, gap_tag));
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
  gtk_text_buffer_apply_tag (ctx->buffer, get_tag (ctx, scope->name), &start,
                             &end);
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

        break_line (ctx, "gap4");
        g_autofree gchar *tag_name =
            g_strdup_printf ("list%d", ctx->list_depth);
        push_scope (ctx, tag_name);
        insert_text (ctx, marker, strlen (marker), get_tag (ctx, "dim"));
        insert_text (ctx, " ", 1, NULL);
      }
      break;

    case MD_BLOCK_P:
      if (ctx->in_cell)
        break;
      /* The first paragraph of a list item follows the number; later
         paragraphs keep their own line. */
      if (ctx->list_depth == 0 || ctx->last_block != MD_BLOCK_LI)
        break_line (ctx, ctx->list_depth > 0 ? "gap4" : "gap16");
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
        break_line (ctx, "gap24");
        g_autofree gchar *tag_name = g_strdup_printf ("h%u", MIN (level, 3));
        push_scope (ctx, tag_name);
      }
      break;

    case MD_BLOCK_HR:
      {
        GtkWidget *rule = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
        g_object_set (rule, "margin-top", 4, "margin-bottom", 4, NULL);
        append_card (ctx, rule);
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
        gtk_widget_add_css_class (card, "table-card");
        ctx->grid = GTK_GRID (gtk_grid_new ());
        ctx->col_count = table != NULL ? table->col_count : 1;
        ctx->row_slot = 0;
        gtk_box_append (GTK_BOX (card), GTK_WIDGET (ctx->grid));
        append_card (ctx, card);
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
  const gchar *tag, *markup;
} SpanStyle;

static const SpanStyle SPAN_STYLES[] = {
  [MD_SPAN_EM] = { "italic", "i" },
  [MD_SPAN_STRONG] = { "bold", "b" },
  [MD_SPAN_DEL] = { "strike", "s" },
  [MD_SPAN_CODE] = { "code", "tt" },
};

static int
on_enter_span (MD_SPANTYPE type,
               void       *detail,
               void       *ud)
{
  MdCtx *ctx = ud;
  GString *cell = ctx->cell;

  switch (type)
    {
    case MD_SPAN_EM:
    case MD_SPAN_STRONG:
    case MD_SPAN_DEL:
    case MD_SPAN_CODE:
      {
        const SpanStyle *style = &SPAN_STYLES[type];
        if (ctx->in_cell)
          g_string_append_printf (cell, "<%s>", style->markup);
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
        insert_text (ctx, "[", 1, NULL);
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
              g_string_append_printf (ctx->cell, "</%s>",
                                      SPAN_STYLES[type].markup);
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
  if (ctx->in_cell)
    escape_into (ctx->cell, text, length);
  else
    insert_text (ctx, text, length, NULL);
  return 0;
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
