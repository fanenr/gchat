/* SPDX-FileCopyrightText: 2026 Arthur <arthur63@163.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "i18n.h"
#include "md.h"
#include "store.h"

#include <adwaita.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <unistd.h>

#define API_URL "https://api.deepseek.com/chat/completions"

#define INPUT_MIN_HEIGHT 92
#define INPUT_MAX_HEIGHT 210

/* Share stored values and UI names with the picker, the chip, and startup
   validation. */
typedef struct
{
  const gchar *value;
  const gchar *label;
} Choice;

static const Choice MODELS[] = { { "deepseek-flash", "DeepSeek-Flash" },
                                 { "deepseek-v4-pro", "DeepSeek-V4-Pro" } };
static const Choice EFFORTS[] = { { "none", N_ ("Off") },
                                  { "low", N_ ("Low") },
                                  { "high", N_ ("High") },
                                  { "max", N_ ("Max") } };

typedef struct
{
  GtkWidget    *window, *toasts, *split;
  GtkWidget    *sidebar_list, *search;
  GtkWidget    *messages, *scroller, *chat_bar, *input, *input_frame, *send,
               *stack, *placeholder;
  GtkWidget    *model_chip, *chip_model, *chip_effort, *choice_popover;
  GtkWidget    *new_btn, *title_label, *edit_bar;
  GtkWidget    *reply_bubble, *reply_body, *reply_label;
  GtkWidget    *reason_box, *reason_view, *reason_title;

  JsonObject   *reply_msg;
  GString      *answer, *reasoning;
  SoupSession  *session;
  GCancellable *cancel;

  GPtrArray    *chats;
  Chat         *chat;

  gchar        *model, *effort, *api_key;

  gboolean      busy, syncing, preedit, restoring_input, closing;

  guint         draft_save_source;
  gint64        request_started;
  gdouble       thought_seconds; /* Store the first content token's arrival
                                    time; use 0 while thinking. */
} App;

static App *app;

static void on_cancel_edit (GtkButton *button,
                            gpointer   data);
static void on_delete_chat (GtkButton *button,
                            gpointer   data);
static void on_delete_message (GtkButton *button,
                               gpointer   data);
static void on_edit_message (GtkButton *button,
                             gpointer   data);
static void on_fork_message (GtkButton *button,
                             gpointer   data);
static void on_regenerate_message (GtkButton *button,
                                   gpointer   data);
static void on_row_rename (GtkGestureClick *gesture,
                           int              n_press,
                           double           x,
                           double           y,
                           gpointer         data);
static void regenerate (void);
static void start_request (void);
static void refresh_input (void);
static void render_chat (void);
static void update_send_state (void);
static void fit_input_height (void);
static void schedule_geometry_sync (void);

static guint focus_source;

static gboolean
grab_input_focus (gpointer data G_GNUC_UNUSED)
{
  focus_source = 0;
  if (app->input != NULL)
    gtk_widget_grab_focus (app->input);
  return G_SOURCE_REMOVE;
}
static GtkWidget *thought_widget (const gchar *reasoning,
                                  gdouble      seconds,
                                  GtkWidget  **view_out,
                                  GtkWidget  **title_out);
static void refresh_choices (void);
static gboolean save_session (void);
static gboolean record_reply (void);
static void rebuild_sidebar (void);

void
app_toast (const gchar *msg)
{
  if (app != NULL && app->toasts != NULL)
    {
      AdwToast *toast = adw_toast_new (msg);
      adw_toast_set_use_markup (toast, FALSE);
      adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (app->toasts), toast);
    }
}

/* Add a "Retry" button on failure so the user does not retype the prompt. */
static void
app_toast_retry (const gchar *msg)
{
  if (app == NULL || app->toasts == NULL)
    return;
  AdwToast *toast = adw_toast_new (msg);
  adw_toast_set_use_markup (toast, FALSE);
  adw_toast_set_button_label (toast, _ ("Retry"));
  adw_toast_set_action_name (toast, "win.retry");
  adw_toast_set_action_target_value (toast,
                                     g_variant_new_string (app->chat->id));
  adw_toast_set_timeout (toast, 8);
  adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (app->toasts), toast);
}

/* ---------- scroll ---------- */

/* Stick to the bottom. Restoring history settles the layout over several
   rounds, and one set_value scrolls only to the bottom of that moment. So keep
   sticking while the content keeps growing, until the user scrolls up. There is
   one for the message area and one for the input box. */
typedef struct
{
  GtkAdjustment *adj;
  gboolean       follow;
  gboolean       setting; /* Do not treat our own value change as a user
                             scroll. */
  guint          scroll_source;
} Stick;

static Stick chat_stick, input_stick;

static gdouble
stick_bottom (Stick *stick)
{
  return gtk_adjustment_get_upper (stick->adj) -
         gtk_adjustment_get_page_size (stick->adj);
}

static void
stick_scroll (Stick *stick)
{
  gdouble bottom = stick_bottom (stick);
  if (bottom <= 0)
    return;
  stick->setting = TRUE;
  gtk_adjustment_set_value (stick->adj, bottom);
  stick->setting = FALSE;
}

/* Resume following after the user scrolls them back to the bottom. */
static void
stick_on_value_changed (Stick *stick)
{
  if (stick->setting)
    return;
  gdouble bottom = stick_bottom (stick);
  stick->follow = bottom - gtk_adjustment_get_value (stick->adj) < 2;
}

/* Update measure caches in stages after a text view is mapped; requeue the
   layout to get a stable height. */
static gboolean
kick_reflow_cb (gpointer box)
{
  gtk_widget_queue_resize (GTK_WIDGET (box));
  return G_SOURCE_REMOVE;
}

static gboolean
stick_scroll_cb (gpointer data)
{
  Stick *stick = data;
  stick->scroll_source = 0;
  if (stick->follow)
    stick_scroll (stick);
  return G_SOURCE_REMOVE;
}

static void
stick_queue_scroll (Stick *stick)
{
  /* changed fires during GtkViewport allocation; update the scroll position
     after allocation finishes. */
  if (!stick->scroll_source)
    stick->scroll_source = g_idle_add (stick_scroll_cb, stick);
}

static void
scroll_bottom (void)
{
  chat_stick.follow = TRUE;
  stick_queue_scroll (&chat_stick);
}

/* Scrollbar: shows on scroll and fades out after 1.2s idle; stays hidden while
   the content fits. Use a class instead of visible to avoid a layout pass. */
static guint chat_bar_timer;

static gboolean
chat_bar_fade_cb (gpointer data G_GNUC_UNUSED)
{
  chat_bar_timer = 0;
  if (app->chat_bar != NULL)
    gtk_widget_add_css_class (app->chat_bar, "faded");
  return G_SOURCE_REMOVE;
}

static void
chat_bar_show (void)
{
  if (app->chat_bar == NULL)
    return;
  gtk_widget_remove_css_class (app->chat_bar, "faded");
  if (chat_bar_timer)
    g_source_remove (chat_bar_timer);
  chat_bar_timer = g_timeout_add (1200, chat_bar_fade_cb, NULL);
}

static void
update_chat_bar (GtkAdjustment *adj)
{
  if (app->chat_bar == NULL)
    return;
  if (gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj) > 1)
    {
      chat_bar_show ();
      return;
    }
  if (chat_bar_timer)
    {
      g_source_remove (chat_bar_timer);
      chat_bar_timer = 0;
    }
  gtk_widget_add_css_class (app->chat_bar, "faded");
}

/* Do not fade out while the pointer is on the scrollbar, so dragging works. */
static void
on_chat_bar_enter (GtkEventControllerMotion *motion G_GNUC_UNUSED,
                   double                    x G_GNUC_UNUSED,
                   double                    y G_GNUC_UNUSED,
                   gpointer                  data G_GNUC_UNUSED)
{
  chat_bar_show ();
}

/* Stick to the bottom only when the content height changes (changed); plain
   scrolling goes through value-changed. */
static void
on_content_changed (GtkAdjustment *adj)
{
  update_chat_bar (adj);

  if (chat_stick.follow)
    stick_queue_scroll (&chat_stick);
}

static void
on_value_changed (GtkAdjustment *adj)
{
  update_chat_bar (adj);
  stick_on_value_changed (&chat_stick);
}

/* Streaming and the final pass keep the user's scroll choice and do not
   interrupt reading upward. */
static void
scroll_if_following (void)
{
  if (chat_stick.follow)
    stick_queue_scroll (&chat_stick);
}

/* ---------- SSE ---------- */

static gchar *
member_string (JsonObject  *obj,
               const gchar *name)
{
  JsonNode *node = json_object_get_member (obj, name);
  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_STRING)
    {
      return NULL;
    }
  return g_strdup (json_node_get_string (node));
}

/* shortcut: DeepSeek uses one data line per event, aggregate frames before
   adding multiline SSE providers. */
static const gchar *
sse_data (const gchar *line)
{
  if (!g_str_has_prefix (line, "data:"))
    return NULL;
  const gchar *data = line + 5;
  return *data == ' ' ? data + 1 : data;
}

static gboolean
sse_parse (const gchar *line,
           gchar      **content,
           gchar      **reasoning)
{
  *content = NULL;
  *reasoning = NULL;
  const gchar *data = sse_data (line);
  if (data == NULL)
    return FALSE;

  JsonParser *parser = json_parser_new ();
  gboolean ok = FALSE;
  if (json_parser_load_from_data (parser, data, -1, NULL))
    {
      JsonNode *root = json_parser_get_root (parser);
      JsonNode *choices =
          root != NULL && JSON_NODE_HOLDS_OBJECT (root)
              ? json_object_get_member (json_node_get_object (root), "choices")
              : NULL;
      if (choices != NULL && JSON_NODE_HOLDS_ARRAY (choices))
        {
          JsonArray *array = json_node_get_array (choices);
          JsonNode *first = json_array_get_length (array)
                                ? json_array_get_element (array, 0)
                                : NULL;
          JsonNode *delta = first != NULL && JSON_NODE_HOLDS_OBJECT (first)
                                ? json_object_get_member (
                                      json_node_get_object (first), "delta")
                                : NULL;
          if (delta != NULL && JSON_NODE_HOLDS_OBJECT (delta))
            {
              *content =
                  member_string (json_node_get_object (delta), "content");
              *reasoning = member_string (json_node_get_object (delta),
                                          "reasoning_content");
              ok = TRUE;
            }
        }
    }
  g_object_unref (parser);
  return ok;
}

/* ---------- bubbles ---------- */

static void
set_copy_text (GtkWidget   *bubble,
               const gchar *text)
{
  GtkWidget *copy = g_object_get_data (G_OBJECT (bubble), "copy");
  if (copy != NULL)
    g_object_set_data_full (G_OBJECT (copy), "text", g_strdup (text), g_free);
}

static GtkWidget *
icon_button (const gchar *icon,
             gint         px,
             gboolean     compact,
             const gchar *tooltip)
{
  GtkWidget *button = gtk_button_new_from_icon_name (icon);
  GtkWidget *image = gtk_button_get_child (GTK_BUTTON (button));
  if (image != NULL && GTK_IS_IMAGE (image))
    gtk_image_set_pixel_size (GTK_IMAGE (image), px);
  gtk_widget_add_css_class (button, "flat");
  if (compact)
    gtk_widget_add_css_class (button, "compact-action");
  if (tooltip != NULL)
    gtk_widget_set_tooltip_text (button, tooltip);
  return button;
}

static void
on_copy_message (GtkButton *button,
                 gpointer   data G_GNUC_UNUSED)
{
  const gchar *text = g_object_get_data (G_OBJECT (button), "text");
  if (text == NULL)
    return;
  gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (button)), text);
  app_toast (_ ("Copied"));
}

/* The message action row supports copy, edit, fork, regenerate, and delete. */
static GtkWidget *
add_bubble_action (GtkWidget   *bubble,
                   const gchar *icon,
                   const gchar *tooltip,
                   GCallback    callback)
{
  GtkWidget *actions = g_object_get_data (G_OBJECT (bubble), "actions");
  GtkWidget *button = icon_button (icon, 14, TRUE, tooltip);
  g_signal_connect (button, "clicked", callback, bubble);
  gtk_box_append (GTK_BOX (actions), button);
  return button;
}

static void
draw_fork_icon (GtkDrawingArea *area,
                cairo_t        *cr,
                int             width,
                int             height,
                gpointer        data G_GNUC_UNUSED)
{
  GdkRGBA color;
  gtk_widget_get_color (GTK_WIDGET (area), &color);
  gdk_cairo_set_source_rgba (cr, &color);
  cairo_scale (cr, width / 16.0, height / 16.0);
  cairo_set_line_width (cr, 1.5);
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
  cairo_move_to (cr, 4, 5);
  cairo_line_to (cr, 4, 11);
  cairo_move_to (cr, 12, 5);
  cairo_curve_to (cr, 12, 9, 4, 6, 4, 11);
  cairo_stroke (cr);
  cairo_arc (cr, 4, 3.5, 1.5, 0, 2 * G_PI);
  cairo_new_sub_path (cr);
  cairo_arc (cr, 4, 12.5, 1.5, 0, 2 * G_PI);
  cairo_new_sub_path (cr);
  cairo_arc (cr, 12, 3.5, 1.5, 0, 2 * G_PI);
  cairo_fill (cr);
}

static void
add_fork_action (GtkWidget *bubble)
{
  GtkWidget *button = add_bubble_action (bubble, "list-add-symbolic",
                                        _ ("Fork conversation"),
                                        G_CALLBACK (on_fork_message));
  GtkWidget *icon = gtk_drawing_area_new ();
  gtk_widget_set_size_request (icon, 14, 14);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (icon), draw_fork_icon,
                                  NULL, NULL);
  gtk_button_set_child (GTK_BUTTON (button), icon);
}

static void
update_bubble_actions (GtkWidget *bubble)
{
  GtkWidget *parent = gtk_widget_get_parent (bubble);
  GtkWidget *actions = g_object_get_data (G_OBJECT (bubble), "actions");
  GtkEventControllerFocus *focus =
      g_object_get_data (G_OBJECT (bubble), "focus");
  gboolean show = (parent != NULL &&
                   gtk_widget_get_last_child (parent) == bubble) ||
                  gtk_event_controller_focus_contains_focus (focus) ||
                  GPOINTER_TO_INT (g_object_get_data (G_OBJECT (bubble),
                                                      "hovered"));
  gtk_widget_set_opacity (actions, show ? 1 : 0);
  gtk_widget_set_can_target (actions, show);
}

static void
on_bubble_focus_changed (GObject    *controller,
                         GParamSpec *pspec G_GNUC_UNUSED,
                         gpointer    data G_GNUC_UNUSED)
{
  GtkWidget *bubble =
      gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));
  if (bubble != NULL)
    update_bubble_actions (bubble);
}

static void
on_bubble_enter (GtkEventControllerMotion *controller G_GNUC_UNUSED,
                 double                    x G_GNUC_UNUSED,
                 double                    y G_GNUC_UNUSED,
                 gpointer                  data)
{
  GtkWidget *bubble = data;
  g_object_set_data (G_OBJECT (bubble), "hovered", GINT_TO_POINTER (TRUE));
  update_bubble_actions (bubble);
}

static void
on_bubble_leave (GtkEventControllerMotion *controller G_GNUC_UNUSED,
                 gpointer                  data)
{
  GtkWidget *bubble = data;
  g_object_set_data (G_OBJECT (bubble), "hovered", NULL);
  update_bubble_actions (bubble);
}

static GtkWidget *
bubble_new (const gchar *role)
{
  gboolean is_user = g_str_equal (role, "user");

  GtkWidget *bubble = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class (bubble, "msg");
  if (is_user)
    gtk_widget_set_halign (bubble, GTK_ALIGN_END);

  /* Lay out the body and the action row separately; the user background shrinks
     to the body plus the fixed padding. */
  GtkWidget *surface = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  if (is_user)
    {
      gtk_widget_add_css_class (surface, "user-bubble");
      gtk_widget_set_halign (surface, GTK_ALIGN_END);
    }
  GtkWidget *body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append (GTK_BOX (surface), body);
  gtk_box_append (GTK_BOX (bubble), surface);
  g_object_set_data (G_OBJECT (bubble), "body", body);

  GtkWidget *actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class (actions, "msg-actions-row");
  if (is_user)
    gtk_widget_set_halign (actions, GTK_ALIGN_END);
  GtkWidget *copy =
      icon_button ("edit-copy-symbolic", 14, TRUE,
                   is_user ? _ ("Copy message") : _ ("Copy reply"));
  g_signal_connect (copy, "clicked", G_CALLBACK (on_copy_message), NULL);
  gtk_box_append (GTK_BOX (actions), copy);
  gtk_box_append (GTK_BOX (bubble), actions);
  g_object_set_data (G_OBJECT (bubble), "copy", copy);
  g_object_set_data (G_OBJECT (bubble), "actions", actions);
  GtkEventController *focus = gtk_event_controller_focus_new ();
  g_signal_connect (focus, "notify::contains-focus",
                    G_CALLBACK (on_bubble_focus_changed), NULL);
  gtk_widget_add_controller (bubble, focus);
  g_object_set_data (G_OBJECT (bubble), "focus", focus);
  GtkEventController *motion = gtk_event_controller_motion_new ();
  g_signal_connect (motion, "enter", G_CALLBACK (on_bubble_enter), bubble);
  g_signal_connect (motion, "leave", G_CALLBACK (on_bubble_leave), bubble);
  gtk_widget_add_controller (bubble, motion);
  g_object_set_data (G_OBJECT (bubble), "motion", motion);
  GtkWidget *previous = gtk_widget_get_last_child (app->messages);
  gtk_box_append (GTK_BOX (app->messages), bubble);
  if (previous != NULL)
    update_bubble_actions (previous);
  update_bubble_actions (bubble);
  return bubble;
}

/* ---------- chat rendering ---------- */

static void
clear_messages (void)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (app->messages)) != NULL)
    gtk_box_remove (GTK_BOX (app->messages), child);
}

static void
update_title (void)
{
  const gchar *title = app->chat->title[0] ? app->chat->title : "gchat";
  adw_window_title_set_title (ADW_WINDOW_TITLE (app->title_label), title);
  gtk_window_set_title (GTK_WINDOW (app->window), title);
}

/* The user body shrinks to its longest line, up to 520px; the bubble padding
   provides the space under a paragraph. */
static void
layout_user_bubble (GtkWidget   *body,
                    const gchar *content)
{
  for (GtkWidget *child = gtk_widget_get_first_child (body); child != NULL;
       child = gtk_widget_get_next_sibling (child))
    if (GTK_IS_TEXT_VIEW (child))
      gtk_text_view_set_pixels_below_lines (GTK_TEXT_VIEW (child), 0);
  PangoLayout *layout = pango_layout_new (gtk_widget_get_pango_context (body));
  pango_layout_set_text (layout, content, -1);
  int width = 0;
  pango_layout_get_pixel_size (layout, &width, NULL);
  g_object_unref (layout);
  /* A shown GtkTextView reserves room for the cursor; add 2px so a trailing
     punctuation mark does not wrap. */
  gtk_widget_set_size_request (body, MIN (width + 2, 520), -1);
}

static void
render_history (guint count)
{
  for (guint i = 0; i < count; i++)
    {
      JsonObject *msg = json_array_get_object_element (app->chat->messages, i);
      const gchar *role = json_object_get_string_member (msg, "role");
      const gchar *content = json_object_get_string_member (msg, "content");
      GtkWidget *bubble = bubble_new (role);
      g_object_set_data (G_OBJECT (bubble), "index", GUINT_TO_POINTER (i));
      GtkBox *body = GTK_BOX (g_object_get_data (G_OBJECT (bubble), "body"));
      if (g_str_equal (role, "user"))
        {
          /* User messages and replies share the same Markdown rendering. */
          md_render (body, content);
          layout_user_bubble (GTK_WIDGET (body), content);
          add_bubble_action (bubble, "document-edit-symbolic",
                             _ ("Edit and resend"),
                             G_CALLBACK (on_edit_message));
        }
      else
        {
          const gchar *reasoning =
              json_object_get_string_member_with_default (msg, "reasoning", "");
          gdouble duration =
              json_object_has_member (msg, "duration")
                  ? json_object_get_double_member (msg, "duration")
                  : 0;
          if (*reasoning)
            gtk_box_append (body,
                            thought_widget (reasoning, duration, NULL, NULL));
          md_render (body, content);
          if (!*content)
            content = reasoning;
          /* Only the last message is worth regenerating. */
          if (i + 1 == count && app->chat->edit_from == G_MAXUINT)
            add_bubble_action (bubble, "view-refresh-symbolic",
                               _ ("Regenerate"),
                               G_CALLBACK (on_regenerate_message));
        }
      add_fork_action (bubble);
      add_bubble_action (bubble, "user-trash-symbolic", _ ("Delete message"),
                         G_CALLBACK (on_delete_message));
      set_copy_text (bubble, content);
    }
}

static void
restore_input (void)
{
  gboolean editing = app->chat->edit_from != G_MAXUINT;
  app->restoring_input = TRUE;
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (app->input));
  gtk_text_buffer_set_text (
      buffer, editing ? app->chat->edit_text : app->chat->draft, -1);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter (buffer, &end);
  gtk_text_buffer_place_cursor (buffer, &end);
  app->restoring_input = FALSE;
  gtk_widget_set_visible (app->edit_bar, editing);
  refresh_input ();
}

static void
clear_edit (Chat *chat)
{
  chat->edit_from = G_MAXUINT;
  g_free (chat->edit_text);
  chat->edit_text = g_strdup ("");
}

static void
render_chat (void)
{
  clear_messages ();
  guint count = app->chat->edit_from == G_MAXUINT
                    ? json_array_get_length (app->chat->messages)
                    : app->chat->edit_from;
  restore_input ();
  render_history (count);
  gtk_stack_set_visible_child_name (GTK_STACK (app->stack),
                                    count ? "chat" : "empty");
  update_title ();
  scroll_bottom ();
  schedule_geometry_sync ();
  for (guint i = 1; i <= 3; i++)
    g_timeout_add_full (G_PRIORITY_DEFAULT, 300 * i, kick_reflow_cb,
                        g_object_ref (app->messages), g_object_unref);
}

/* ---------- sidebar ---------- */

static gboolean
save_session (void)
{
  if (app->draft_save_source)
    g_source_remove (app->draft_save_source);
  app->draft_save_source = 0;
  if (app->busy && app->reply_msg != NULL)
    record_reply ();
  gchar *path = store_session_path ();
  gboolean ok =
      store_save (path, app->chats, app->chat->id, app->model, app->effort);
  if (!ok)
    app_toast (_ ("Failed to save conversation"));
  g_free (path);
  return ok;
}

static void
set_busy (gboolean busy)
{
  app->busy = busy;
  gtk_button_set_icon_name (GTK_BUTTON (app->send),
                            busy ? "process-stop-symbolic" : "go-up-symbolic");
  gtk_widget_set_tooltip_text (
      app->send, busy ? _ ("Stop generating (Ctrl/Cmd+.)") : _ ("Send"));
  gtk_widget_set_sensitive (app->sidebar_list, !busy);
  gtk_widget_set_sensitive (app->search, !busy);
  gtk_widget_set_sensitive (app->model_chip, !busy);
  gtk_widget_set_sensitive (app->new_btn, !busy);
  update_send_state ();
}

static gboolean
same_day (GDateTime *a,
          GDateTime *b)
{
  return g_date_time_get_year (a) == g_date_time_get_year (b) &&
         g_date_time_get_month (a) == g_date_time_get_month (b) &&
         g_date_time_get_day_of_month (a) == g_date_time_get_day_of_month (b);
}

static const gchar *
chat_group_label (gint64 created)
{
  GDateTime *now = g_date_time_new_now_local ();
  GDateTime *yday = g_date_time_add_days (now, -1);
  GDateTime *day =
      created > 0 ? g_date_time_new_from_unix_local (created / 1000) : NULL;
  const gchar *label = _ ("Earlier");
  if (day != NULL)
    {
      if (same_day (now, day))
        label = _ ("Today");
      else if (same_day (yday, day))
        label = _ ("Yesterday");
    }
  if (day != NULL)
    g_date_time_unref (day);
  g_date_time_unref (yday);
  g_date_time_unref (now);
  return label;
}

static gint
cmp_chat_desc (gconstpointer a,
               gconstpointer b)
{
  const Chat *x = *(Chat *const *) a;
  const Chat *y = *(Chat *const *) b;
  if (x->created == y->created)
    return 0;
  return x->created > y->created ? -1 : 1;
}

static void
sidebar_add_group (const gchar *text)
{
  GtkWidget *row = gtk_list_box_row_new ();
  gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
  gtk_widget_add_css_class (row, "sidebar-group");
  GtkWidget *label = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), label);
  gtk_list_box_append (GTK_LIST_BOX (app->sidebar_list), row);
}

/* NULL means no match, an empty string means the title matched, otherwise an
   excerpt around the body match. */
static gchar *
search_preview (Chat   *chat,
                GRegex *regex)
{
  if (g_regex_match (regex, *chat->title ? chat->title : _ ("New chat"), 0,
                     NULL))
    {
      return g_strdup ("");
    }
  for (guint i = 0; i < json_array_get_length (chat->messages); i++)
    {
      JsonObject *msg = json_array_get_object_element (chat->messages, i);
      const gchar *text = json_object_get_string_member (msg, "content");
      GMatchInfo *match = NULL;
      gboolean found = g_regex_match (regex, text, 0, &match);
      gint pos = 0;
      if (found)
        g_match_info_fetch_pos (match, 0, &pos, NULL);
      g_match_info_free (match);
      if (!found)
        continue;
      /* The excerpt starts at the match, so the narrow sidebar does not cut the
         keyword behind an ellipsis. */
      const gchar *start = text + pos;
      const gchar *end = g_utf8_offset_to_pointer (
          start, MIN (120, g_utf8_strlen (start, -1)));
      gchar *excerpt = g_strndup (start, end - start);
      g_strdelimit (excerpt, "\r\n\t", ' ');
      gchar *preview = g_strdup_printf ("%s%s%s", start > text ? "…" : "",
                                        excerpt, *end ? "…" : "");
      g_free (excerpt);
      return preview;
    }
  return NULL;
}

static void
rebuild_sidebar (void)
{
  app->syncing = TRUE;
  gchar *query = g_strstrip (
      g_strdup (gtk_editable_get_text (GTK_EDITABLE (app->search))));
  gchar *pattern = g_regex_escape_string (query, -1);
  GRegex *regex =
      *query
          ? g_regex_new (pattern, G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL)
          : NULL;
  g_free (pattern);
  g_free (query);
  GtkWidget *child = gtk_widget_get_first_child (app->sidebar_list);
  while (child != NULL)
    {
      GtkWidget *next = gtk_widget_get_next_sibling (child);
      if (GTK_IS_LIST_BOX_ROW (child))
        gtk_list_box_remove (GTK_LIST_BOX (app->sidebar_list), child);
      child = next;
    }

  /* Put the newest session on top. */
  GArray *sorted = g_array_new (FALSE, FALSE, sizeof (Chat *));
  for (guint i = 0; i < app->chats->len; i++)
    {
      Chat *chat = g_ptr_array_index (app->chats, i);
      g_array_append_val (sorted, chat);
    }
  g_array_sort (sorted, cmp_chat_desc);

  const gchar *group = NULL;
  GtkListBoxRow *current_row = NULL;
  for (guint i = 0; i < sorted->len; i++)
    {
      Chat *chat = g_array_index (sorted, Chat *, i);
      gchar *preview = regex != NULL ? search_preview (chat, regex) : NULL;
      if (regex != NULL && preview == NULL)
        continue;

      const gchar *label_text = chat_group_label (chat->created);
      if (group == NULL || !g_str_equal (group, label_text))
        {
          sidebar_add_group (label_text);
          group = label_text;
        }

      GtkWidget *row = gtk_list_box_row_new ();
      gtk_widget_add_css_class (row, "sidebar-row");

      GtkWidget *overlay = gtk_overlay_new ();
      GtkWidget *title =
          gtk_label_new (chat->title[0] ? chat->title : _ ("New chat"));
      gtk_label_set_xalign (GTK_LABEL (title), 0);
      gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
      g_object_set (title, "margin-start", 10, "margin-end", 26, NULL);

      GtkWidget *del = icon_button ("user-trash-symbolic", 13, TRUE,
                                    _ ("Delete conversation"));
      gtk_widget_add_css_class (del, "row-del");
      gtk_widget_set_halign (del, GTK_ALIGN_END);
      gtk_widget_set_valign (del, GTK_ALIGN_CENTER);
      gtk_widget_set_margin_end (del, 6);
      g_signal_connect (del, "clicked", G_CALLBACK (on_delete_chat), chat);

      GtkWidget *labels = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
      gtk_widget_set_valign (labels, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (labels), title);
      if (preview != NULL && *preview)
        {
          GtkWidget *snippet = gtk_label_new (preview);
          gtk_label_set_xalign (GTK_LABEL (snippet), 0);
          gtk_label_set_single_line_mode (GTK_LABEL (snippet), TRUE);
          gtk_label_set_ellipsize (GTK_LABEL (snippet), PANGO_ELLIPSIZE_END);
          gtk_widget_add_css_class (snippet, "caption");
          gtk_widget_add_css_class (snippet, "dimmed");
          g_object_set (snippet, "margin-start", 10, "margin-end", 26, NULL);
          gtk_widget_set_tooltip_text (snippet, preview);
          gtk_box_append (GTK_BOX (labels), snippet);
        }
      g_free (preview);
      gtk_overlay_set_child (GTK_OVERLAY (overlay), labels);
      gtk_overlay_add_overlay (GTK_OVERLAY (overlay), del);
      gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), overlay);
      g_object_set_data (G_OBJECT (row), "chat", chat);
      gtk_widget_set_tooltip_text (row, _ ("Right-click to rename"));
      GtkGesture *secondary = gtk_gesture_click_new ();
      gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (secondary),
                                     GDK_BUTTON_SECONDARY);
      g_signal_connect (secondary, "pressed", G_CALLBACK (on_row_rename), row);
      gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (secondary));
      gtk_list_box_append (GTK_LIST_BOX (app->sidebar_list), row);
      if (chat == app->chat)
        current_row = GTK_LIST_BOX_ROW (row);
    }
  g_array_free (sorted, TRUE);
  if (regex != NULL)
    g_regex_unref (regex);
  if (current_row != NULL)
    gtk_list_box_select_row (GTK_LIST_BOX (app->sidebar_list), current_row);
  app->syncing = FALSE;
}

/* ---------- actions ---------- */

/* Message actions. The bubble holds the index from render time and reads it on
   click; a re-render after a delete refreshes the index. */
static guint
bubble_index (GtkWidget *bubble)
{
  return GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (bubble), "index"));
}

static void
on_delete_message (GtkButton *button G_GNUC_UNUSED,
                   gpointer   data)
{
  guint index = bubble_index (data);
  if (app->busy || index >= json_array_get_length (app->chat->messages))
    return;
  json_array_remove_element (app->chat->messages, index);
  if (app->chat->edit_from != G_MAXUINT)
    {
      if (index == app->chat->edit_from)
        clear_edit (app->chat);
      else if (index < app->chat->edit_from)
        app->chat->edit_from--;
    }
  render_chat ();
  save_session ();
}

static void
on_edit_message (GtkButton *button G_GNUC_UNUSED,
                 gpointer   data)
{
  guint index = bubble_index (data);
  if (app->busy || index >= json_array_get_length (app->chat->messages))
    return;
  JsonObject *msg = json_array_get_object_element (app->chat->messages, index);
  const gchar *content = json_object_get_string_member (msg, "content");

  /* The normal draft and the edit draft are separate; truncate the history only
     on send. */
  app->chat->edit_from = index;
  g_free (app->chat->edit_text);
  app->chat->edit_text = g_strdup (content);
  render_chat ();
  save_session ();
  gtk_widget_grab_focus (app->input);
}

static void
on_fork_message (GtkButton *button G_GNUC_UNUSED,
                 gpointer   data)
{
  guint index = bubble_index (data);
  if (app->busy || index >= json_array_get_length (app->chat->messages))
    return;

  Chat *source = app->chat;
  JsonObject *selected =
      json_array_get_object_element (source->messages, index);
  gboolean is_user = g_str_equal (
      json_object_get_string_member (selected, "role"), "user");
  Chat *fork = chat_new_empty ();
  g_free (fork->title);
  fork->title = *source->title
                    ? g_strdup_printf (_ ("%s (Fork)"), source->title)
                    : g_strdup (_ ("Fork"));
  if (is_user)
    {
      g_free (fork->draft);
      fork->draft = g_strdup (json_object_get_string_member (selected,
                                                             "content"));
    }
  for (guint i = 0; i < index + !is_user; i++)
    {
      JsonObject *original =
          json_array_get_object_element (source->messages, i);
      JsonObject *copy = json_object_new ();
      GList *members = json_object_get_members (original);
      for (GList *l = members; l != NULL; l = l->next)
        json_object_set_member (copy, l->data,
                                json_node_copy (json_object_get_member (
                                    original, l->data)));
      g_list_free (members);
      json_array_add_object_element (fork->messages, copy);
    }

  gtk_editable_set_text (GTK_EDITABLE (app->search), "");
  app->chat = fork;
  g_ptr_array_add (app->chats, fork);
  rebuild_sidebar ();
  render_chat ();
  save_session ();
  gtk_widget_grab_focus (app->input);
}

static void
on_cancel_edit (GtkButton *button G_GNUC_UNUSED,
                gpointer   data G_GNUC_UNUSED)
{
  if (app->busy || app->chat->edit_from == G_MAXUINT)
    return;
  clear_edit (app->chat);
  render_chat ();
  save_session ();
  gtk_widget_grab_focus (app->input);
}

static void
on_regenerate_message (GtkButton *button G_GNUC_UNUSED,
                       gpointer   data G_GNUC_UNUSED)
{
  regenerate ();
}

static void
on_new_chat (void)
{
  if (app->busy || adw_application_window_get_visible_dialog (
                       ADW_APPLICATION_WINDOW (app->window)) != NULL)
    {
      return;
    }
  gtk_editable_set_text (GTK_EDITABLE (app->search), "");
  if (json_array_get_length (app->chat->messages) == 0 && !*app->chat->draft)
    {
      gtk_widget_grab_focus (app->input);
      return;
    }
  app->chat = chat_new_empty ();
  g_ptr_array_add (app->chats, app->chat);
  rebuild_sidebar ();
  render_chat ();
  save_session ();
  gtk_widget_grab_focus (app->input);
}

static void
on_rename_response (AdwAlertDialog *dialog,
                    const gchar    *response,
                    gpointer        data)
{
  if (!g_str_equal (response, "save"))
    return;
  Chat *chat = data;
  GtkWidget *entry = adw_alert_dialog_get_extra_child (dialog);
  const gchar *text =
      entry != NULL ? gtk_editable_get_text (GTK_EDITABLE (entry)) : NULL;
  if (text == NULL || !*text)
    return;

  g_free (chat->title);
  chat->title = g_strdup (text);
  rebuild_sidebar ();
  if (chat == app->chat)
    update_title ();
  save_session ();
}

static void
on_rename_activate (GtkEntry *entry,
                    gpointer  data)
{
  AdwAlertDialog *dialog = data;
  on_rename_response (dialog, "save",
                      g_object_get_data (G_OBJECT (entry), "chat"));
  adw_dialog_close (ADW_DIALOG (dialog));
}

static void
on_row_rename (GtkGestureClick *gesture G_GNUC_UNUSED,
               int              n_press G_GNUC_UNUSED,
               double           x G_GNUC_UNUSED,
               double           y G_GNUC_UNUSED,
               gpointer         data)
{
  Chat *chat = g_object_get_data (G_OBJECT (data), "chat");
  if (chat == NULL || app->busy)
    return;

  AdwAlertDialog *dialog =
      ADW_ALERT_DIALOG (adw_alert_dialog_new (_ ("Rename conversation"), NULL));
  GtkWidget *entry = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (entry), chat->title);
  gtk_widget_set_size_request (entry, 320, -1);
  g_object_set_data (G_OBJECT (entry), "chat", chat);
  g_signal_connect (entry, "activate", G_CALLBACK (on_rename_activate), dialog);
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), entry);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog), "cancel",
                                  _ ("Cancel"), "save", _ ("Save"), NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "save",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "save");
  g_signal_connect (dialog, "response", G_CALLBACK (on_rename_response), chat);
  adw_dialog_present (ADW_DIALOG (dialog), app->window);
}

static void
on_new_chat_clicked (GtkButton *button G_GNUC_UNUSED,
                     gpointer   data G_GNUC_UNUSED)
{
  on_new_chat ();
}

static void
on_delete_chat_response (AdwAlertDialog *dialog,
                         const gchar    *response,
                         gpointer        data G_GNUC_UNUSED)
{
  if (app->busy || !g_str_equal (response, "delete"))
    return;
  const gchar *id = g_object_get_data (G_OBJECT (dialog), "chat-id");
  guint index;
  for (index = 0; index < app->chats->len; index++)
    if (g_str_equal (((Chat *) g_ptr_array_index (app->chats, index))->id, id))
      break;
  if (index == app->chats->len)
    return;
  Chat *chat = g_ptr_array_index (app->chats, index);
  gboolean was_current = (chat == app->chat);
  g_ptr_array_remove_index (app->chats, index); /* This also frees the chat. */
  if (was_current)
    {
      if (app->chats->len == 0)
        g_ptr_array_add (app->chats, chat_new_empty ());
      app->chat =
          g_ptr_array_index (app->chats, MIN (index, app->chats->len - 1));
      render_chat ();
    }
  rebuild_sidebar ();
  save_session ();
}

static void
on_delete_chat (GtkButton *button G_GNUC_UNUSED,
                gpointer   data)
{
  if (app->busy)
    return;
  Chat *chat = data;
  gchar *body = g_strdup_printf (
      _ ("Delete “%s” and all its messages and drafts? This cannot be undone."),
      *chat->title ? chat->title : _ ("New chat"));
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG (
      adw_alert_dialog_new (_ ("Delete conversation?"), body));
  g_free (body);
  adw_alert_dialog_add_responses (dialog, "cancel", _ ("Cancel"), "delete",
                                  _ ("Delete"), NULL);
  adw_alert_dialog_set_response_appearance (dialog, "delete",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (dialog, "cancel");
  adw_alert_dialog_set_close_response (dialog, "cancel");
  g_object_set_data_full (G_OBJECT (dialog), "chat-id", g_strdup (chat->id),
                          g_free);
  g_signal_connect (dialog, "response", G_CALLBACK (on_delete_chat_response),
                    NULL);
  adw_dialog_present (ADW_DIALOG (dialog), app->window);
}

static void
on_row_selected (GtkListBox    *list G_GNUC_UNUSED,
                 GtkListBoxRow *row,
                 gpointer       data G_GNUC_UNUSED)
{
  if (app->syncing || row == NULL || app->busy)
    return;
  Chat *chat = g_object_get_data (G_OBJECT (row), "chat");
  if (chat == NULL || chat == app->chat)
    return;
  app->chat = chat;
  render_chat ();
  save_session ();
  if (adw_overlay_split_view_get_collapsed (
          ADW_OVERLAY_SPLIT_VIEW (app->split)))
    {
      adw_overlay_split_view_set_show_sidebar (
          ADW_OVERLAY_SPLIT_VIEW (app->split), FALSE);
    }
}

static const Choice *
find_choice (const Choice *table,
             guint         n,
             const gchar  *value)
{
  for (guint i = 0; i < n; i++)
    if (g_strcmp0 (table[i].value, value) == 0)
      return &table[i];
  return NULL;
}

static void
on_model_choice (GtkButton *button G_GNUC_UNUSED,
                 gpointer   value)
{
  g_free (app->model);
  app->model = g_strdup (value);
  gtk_popover_popdown (GTK_POPOVER (app->choice_popover));
  refresh_choices ();
  save_session ();
}

static void
on_effort_choice (GtkButton *button G_GNUC_UNUSED,
                  gpointer   value)
{
  g_free (app->effort);
  app->effort = g_strdup (value);
  gtk_popover_popdown (GTK_POPOVER (app->choice_popover));
  refresh_choices ();
  save_session ();
}

static GtkWidget *
section_title (const gchar *text)
{
  GtkWidget *label = gtk_label_new (text);
  gtk_widget_add_css_class (label, "caption");
  gtk_widget_add_css_class (label, "dimmed");
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  g_object_set (label, "margin-start", 10, "margin-top", 6, "margin-bottom", 2,
                NULL);
  return label;
}

static GtkWidget *
choice_row (const gchar *text,
            GCallback    callback,
            const gchar *value,
            gboolean     active)
{
  GtkWidget *row = gtk_button_new ();
  gtk_widget_add_css_class (row, "flat");

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *label = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_box_append (GTK_BOX (box), label);

  if (active)
    {
      GtkWidget *check =
          gtk_image_new_from_icon_name ("object-select-symbolic");
      gtk_image_set_pixel_size (GTK_IMAGE (check), 14);
      gtk_box_append (GTK_BOX (box), check);
    }

  gtk_button_set_child (GTK_BUTTON (row), box);
  g_signal_connect (row, "clicked", callback, (gpointer) value);
  return row;
}

static void
refresh_choices (void)
{
  const Choice *model = find_choice (MODELS, G_N_ELEMENTS (MODELS), app->model);
  const Choice *effort =
      find_choice (EFFORTS, G_N_ELEMENTS (EFFORTS), app->effort);
  gtk_label_set_text (GTK_LABEL (app->chip_model), model->label);
  gtk_label_set_text (GTK_LABEL (app->chip_effort), _ (effort->label));
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  g_object_set (box, "margin-top", 6, "margin-bottom", 6, "margin-start", 6,
                "margin-end", 6, NULL);

  gtk_box_append (GTK_BOX (box), section_title (_ ("Model")));
  for (guint i = 0; i < G_N_ELEMENTS (MODELS); i++)
    gtk_box_append (GTK_BOX (box),
                    choice_row (MODELS[i].label, G_CALLBACK (on_model_choice),
                                MODELS[i].value,
                                g_str_equal (app->model, MODELS[i].value)));

  GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  g_object_set (sep, "margin-top", 4, "margin-bottom", 4, NULL);
  gtk_box_append (GTK_BOX (box), sep);

  gtk_box_append (GTK_BOX (box), section_title (_ ("Reasoning level")));
  for (guint i = 0; i < G_N_ELEMENTS (EFFORTS); i++)
    gtk_box_append (GTK_BOX (box),
                    choice_row (_ (EFFORTS[i].label),
                                G_CALLBACK (on_effort_choice), EFFORTS[i].value,
                                g_str_equal (app->effort, EFFORTS[i].value)));

  gtk_popover_set_child (GTK_POPOVER (app->choice_popover), box);
}

static void
on_toggle_sidebar (GtkButton *button G_GNUC_UNUSED,
                   gpointer   data G_GNUC_UNUSED)
{
  AdwOverlaySplitView *split = ADW_OVERLAY_SPLIT_VIEW (app->split);
  adw_overlay_split_view_set_show_sidebar (
      split, !adw_overlay_split_view_get_show_sidebar (split));
}

/* ---------- API key ---------- */

/* Share API key saving between the first-run dialog and the settings panel. */
static gboolean
save_api_key (const gchar *key)
{
  if (key == NULL || !*key)
    return FALSE;
  if (!config_set ("api_key", key))
    {
      app_toast (_ ("Failed to save API key"));
      return FALSE;
    }
  const gchar *env_key = g_getenv ("DEEPSEEK_API_KEY");
  g_free (app->api_key);
  app->api_key = g_strdup (env_key != NULL && *env_key ? env_key : key);
  return TRUE;
}

static void
on_key_response (AdwAlertDialog *dialog G_GNUC_UNUSED,
                 const gchar    *response,
                 gpointer        data)
{
  if (!g_str_equal (response, "save"))
    return;
  if (save_api_key (gtk_editable_get_text (GTK_EDITABLE (data))))
    app_toast (_ ("API key saved"));
}

static void
prompt_api_key (void)
{
  gchar *path = store_config_path ();
  gchar *body =
      g_strdup_printf (_ ("Your key is saved in %s for future use."), path);
  AdwDialog *dialog =
      adw_alert_dialog_new (_ ("DeepSeek API key required"), body);
  g_free (body);
  g_free (path);
  GtkWidget *entry = gtk_password_entry_new ();
  gtk_password_entry_set_show_peek_icon (GTK_PASSWORD_ENTRY (entry), TRUE);
  gtk_widget_set_size_request (entry, 320, -1);
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), entry);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog), "cancel",
                                  _ ("Later"), "save", _ ("Save"), NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "save",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "save");
  g_signal_connect (dialog, "response", G_CALLBACK (on_key_response), entry);
  adw_dialog_present (dialog, app->window);
}

/* ---------- streaming ---------- */

static void
on_thought_toggle (GtkButton *button,
                   gpointer   content)
{
  gboolean open = !gtk_widget_get_visible (GTK_WIDGET (content));
  gtk_widget_set_visible (GTK_WIDGET (content), open);
  GtkWidget *chevron = g_object_get_data (G_OBJECT (button), "chevron");
  if (chevron != NULL)
    gtk_image_set_from_icon_name (
        GTK_IMAGE (chevron), open ? "pan-down-symbolic" : "pan-end-symbolic");
  gtk_widget_queue_resize (GTK_WIDGET (content));
}

static void
set_thought_expanded (GtkWidget *root,
                      gboolean   expanded)
{
  GtkWidget *toggle = gtk_widget_get_first_child (root);
  GtkWidget *content = gtk_widget_get_last_child (root);

  if (gtk_widget_get_visible (content) != expanded)
    on_thought_toggle (GTK_BUTTON (toggle), content);
}

static gchar *
thought_title (gdouble seconds)
{
  if (seconds <= 0)
    return g_strdup (_ ("Thought"));
  return seconds >= 10 ? g_strdup_printf (_ ("Thought (%.0f s)"), seconds)
                       : g_strdup_printf (_ ("Thought (%.1f s)"), seconds);
}

static GtkWidget *
thought_widget (const gchar *reasoning,
                gdouble      seconds,
                GtkWidget  **view_out,
                GtkWidget  **title_out)
{
  GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);

  gchar *title = thought_title (seconds);
  GtkWidget *toggle = gtk_button_new ();
  gtk_widget_add_css_class (toggle, "thought-toggle");
  gtk_widget_set_halign (toggle, GTK_ALIGN_START);

  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *title_label = gtk_label_new (title);
  gtk_widget_add_css_class (title_label, "caption");
  gtk_widget_add_css_class (title_label, "dimmed");
  GtkWidget *chevron = gtk_image_new_from_icon_name ("pan-end-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (chevron), 13);
  gtk_widget_add_css_class (chevron, "dimmed");
  gtk_box_append (GTK_BOX (row), title_label);
  gtk_box_append (GTK_BOX (row), chevron);
  gtk_button_set_child (GTK_BUTTON (toggle), row);
  g_free (title);

  /* GtkLabel rounds each line's selection clip separately and can leave
     gaps at fractional line heights. GtkTextView fills between lines. */
  GtkWidget *view = gtk_text_view_new ();
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_editable (GTK_TEXT_VIEW (view), FALSE);
  gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (view), FALSE);
  gtk_text_buffer_set_text (
      gtk_text_view_get_buffer (GTK_TEXT_VIEW (view)), reasoning, -1);
  gtk_widget_add_css_class (view, "thought-text");
  gtk_widget_add_css_class (view, "dimmed");
  gtk_widget_set_hexpand (view, TRUE);

  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (bar, "quote-bar");
  gtk_widget_set_size_request (bar, 3, -1);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  g_object_set (content, "margin-top", 4, "margin-bottom", 6, NULL);
  gtk_box_append (GTK_BOX (content), bar);
  gtk_box_append (GTK_BOX (content), view);
  gtk_widget_set_visible (content, FALSE);

  g_object_set_data (G_OBJECT (toggle), "chevron", chevron);
  g_signal_connect (toggle, "clicked", G_CALLBACK (on_thought_toggle), content);

  gtk_box_append (GTK_BOX (root), toggle);
  gtk_box_append (GTK_BOX (root), content);

  if (view_out != NULL)
    *view_out = view;
  if (title_out != NULL)
    *title_out = title_label;
  return root;
}

static void
set_thought_label (gdouble seconds)
{
  if (app->reason_title == NULL)
    return;
  gchar *text = thought_title (seconds);
  gtk_label_set_text (GTK_LABEL (app->reason_title), text);
  g_free (text);
}

static gdouble
elapsed_seconds (void)
{
  return (g_get_monotonic_time () - app->request_started) / 1e6;
}

static void
clear_stream_state (void)
{
  g_clear_object (&app->cancel);
  app->reply_bubble = NULL;
  app->reply_body = NULL;
  app->reply_label = NULL;
  app->reply_msg = NULL;
  app->reason_box = NULL;
  app->reason_view = NULL;
  app->reason_title = NULL;
  g_string_truncate (app->answer, 0);
  g_string_truncate (app->reasoning, 0);
  set_busy (FALSE);
  rebuild_sidebar ();
}

/* Remove the placeholder when both the body and the reasoning are empty. */
static void
drop_reply (void)
{
  if (app->reply_bubble != NULL)
    {
      gtk_box_remove (GTK_BOX (app->messages), app->reply_bubble);
      GtkWidget *last = gtk_widget_get_last_child (app->messages);
      if (last != NULL)
        update_bubble_actions (last);
    }
  json_array_remove_element (app->chat->messages,
                             json_array_get_length (app->chat->messages) - 1);
  if (!json_array_get_length (app->chat->messages))
    gtk_stack_set_visible_child_name (GTK_STACK (app->stack), "empty");
  clear_stream_state ();
}

static gboolean
record_reply (void)
{
  if (!app->answer->len && !app->reasoning->len)
    return FALSE;
  json_object_set_string_member (app->reply_msg, "content", app->answer->str);
  if (app->reasoning->len)
    {
      json_object_set_string_member (app->reply_msg, "reasoning",
                                     app->reasoning->str);
      json_object_set_double_member (app->reply_msg, "duration",
                                     app->thought_seconds ? app->thought_seconds
                                                          : elapsed_seconds ());
    }
  return TRUE;
}

static void
end_stream (void)
{
  if (!app->busy)
    return;
  if (!record_reply ())
    {
      drop_reply ();
      save_session ();
      app_toast (_ ("No response received"));
      return;
    }
  if (app->reply_label != NULL)
    {
      gtk_box_remove (GTK_BOX (app->reply_body), app->reply_label);
      app->reply_label = NULL;
    }
  if (app->reason_box != NULL)
    {
      set_thought_label (
          json_object_get_double_member (app->reply_msg, "duration"));
      if (app->answer->len == 0)
        set_thought_expanded (app->reason_box, FALSE);
    }
  md_render (GTK_BOX (app->reply_body), app->answer->str);
  set_copy_text (app->reply_bubble,
                 app->answer->len ? app->answer->str : app->reasoning->str);

  /* A streaming bubble starts with copy only; add the other actions at the
     end so it matches a re-rendered message. */
  g_object_set_data (
      G_OBJECT (app->reply_bubble), "index",
      GUINT_TO_POINTER (json_array_get_length (app->chat->messages) - 1));
  add_bubble_action (app->reply_bubble, "view-refresh-symbolic",
                     _ ("Regenerate"), G_CALLBACK (on_regenerate_message));
  add_fork_action (app->reply_bubble);
  add_bubble_action (app->reply_bubble, "user-trash-symbolic",
                     _ ("Delete message"), G_CALLBACK (on_delete_message));
  scroll_if_following ();
  clear_stream_state ();
  save_session ();
}

static void
fail_stream (const gchar *reason)
{
  /* Keep the text and reasoning received so far; the user decides whether to
     regenerate. */
  if (app->answer->len || app->reasoning->len)
    {
      end_stream ();
      app_toast (reason);
      return;
    }
  drop_reply ();
  save_session ();
  app_toast_retry (reason);
}

static void
stream_line_cb (GObject      *src,
                GAsyncResult *res,
                gpointer      data)
{
  GCancellable *cancel = data;
  GDataInputStream *lines = G_DATA_INPUT_STREAM (src);
  GError *error = NULL;
  gchar *line =
      g_data_input_stream_read_line_finish_utf8 (lines, res, NULL, &error);
  if (app == NULL || !app->busy || app->cancel != cancel)
    {
      g_free (line);
      g_clear_error (&error);
      g_object_unref (lines);
      g_object_unref (cancel);
      return;
    }

  if (line == NULL)
    {
      if (error != NULL)
        {
          if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            end_stream ();
          else
            fail_stream (error->message);
          g_error_free (error);
        }
      else
        {
          /* The server sends [DONE] first in the normal case; a stream that
             ends without it holds a truncated reply. */
          if (app->answer->len || app->reasoning->len)
            app_toast (
                _ ("Connection interrupted. The response may be incomplete."));
          end_stream ();
        }
      g_object_unref (lines);
      g_object_unref (cancel);
      return;
    }

  const gchar *event = sse_data (line);
  if (event != NULL && g_str_equal (event, "[DONE]"))
    {
      g_free (line);
      end_stream ();
      g_object_unref (lines);
      g_object_unref (cancel);
      return;
    }

  gchar *content = NULL, *reasoning = NULL;
  if (sse_parse (line, &content, &reasoning))
    {
      if (reasoning != NULL && *reasoning)
        {
          g_string_append (app->reasoning, reasoning);
          if (app->reason_box == NULL)
            {
              app->reason_box =
                  thought_widget (app->reasoning->str, 0, &app->reason_view,
                                  &app->reason_title);
              gtk_box_prepend (GTK_BOX (app->reply_body), app->reason_box);
              if (app->answer->len == 0)
                set_thought_expanded (app->reason_box, TRUE);
              gtk_label_set_text (GTK_LABEL (app->reason_title),
                                  _ ("Thinking…"));
            }
          else
            {
              gtk_text_buffer_set_text (
                  gtk_text_view_get_buffer (GTK_TEXT_VIEW (app->reason_view)),
                  app->reasoning->str, -1);
            }
          if (app->answer->len == 0)
            gtk_label_set_text (GTK_LABEL (app->reply_label), _ ("Thinking…"));
          scroll_if_following ();
        }
      if (content != NULL && *content)
        {
          if (app->answer->len == 0 && app->reason_box != NULL)
            {
              app->thought_seconds = elapsed_seconds ();
              set_thought_label (app->thought_seconds);
              set_thought_expanded (app->reason_box, FALSE);
            }
          g_string_append (app->answer, content);
          gchar *shown = g_strconcat (app->answer->str, "▍", NULL);
          gtk_label_set_text (GTK_LABEL (app->reply_label), shown);
          g_free (shown);
          scroll_if_following ();
        }
    }
  g_free (content);
  g_free (reasoning);
  g_free (line);
  g_data_input_stream_read_line_async (lines, G_PRIORITY_DEFAULT, cancel,
                                       stream_line_cb, cancel);
}

static void
error_body_cb (GObject      *src,
               GAsyncResult *res,
               gpointer      data)
{
  SoupMessage *msg = data;
  GError *error = NULL;
  GBytes *bytes =
      g_input_stream_read_bytes_finish (G_INPUT_STREAM (src), res, &error);
  if (app != NULL && app->busy &&
      g_object_get_data (G_OBJECT (msg), "cancel") == app->cancel)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          end_stream ();
        }
      else
        {
          gchar *detail = NULL;
          if (bytes != NULL)
            {
              gsize size;
              const gchar *text = g_bytes_get_data (bytes, &size);
              if (size)
                detail = g_utf8_make_valid (text, MIN (size, 200));
            }
          else if (error != NULL)
            detail = g_strdup (error->message);
          gchar *reason = g_strdup_printf (
              "HTTP %u %s%s%s", soup_message_get_status (msg),
              soup_message_get_reason_phrase (msg),
              detail != NULL && *detail ? " · " : "",
              detail != NULL ? detail : "");
          fail_stream (reason);
          g_free (reason);
          g_free (detail);
        }
    }
  g_clear_pointer (&bytes, g_bytes_unref);
  g_clear_error (&error);
  g_object_unref (msg);
}

static void
response_cb (GObject      *src,
             GAsyncResult *res,
             gpointer      data)
{
  SoupMessage *msg = data;
  GError *error = NULL;
  GInputStream *body =
      soup_session_send_finish (SOUP_SESSION (src), res, &error);
  if (app == NULL || !app->busy ||
      g_object_get_data (G_OBJECT (msg), "cancel") != app->cancel)
    {
      g_clear_object (&body);
      g_clear_error (&error);
      g_object_unref (msg);
      return;
    }

  if (body == NULL)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        end_stream ();
      else
        fail_stream (error != NULL ? error->message : _ ("Request failed"));
      if (error != NULL)
        g_error_free (error);
    }
  else if (soup_message_get_status (msg) != SOUP_STATUS_OK)
    {
      g_input_stream_read_bytes_async (body, 512, G_PRIORITY_DEFAULT,
                                       app->cancel, error_body_cb, msg);
      g_object_unref (body);
      return;
    }
  else
    {
      GDataInputStream *lines = g_data_input_stream_new (body);
      g_data_input_stream_set_newline_type (lines,
                                            G_DATA_STREAM_NEWLINE_TYPE_ANY);
      g_object_unref (body);
      g_data_input_stream_read_line_async (lines, G_PRIORITY_DEFAULT,
                                           app->cancel, stream_line_cb,
                                           g_object_ref (app->cancel));
    }
  g_object_unref (msg);
}

/* A callback after a cancel can arrive after the next request; tell them apart
   by their own cancellable. */
static void
send_request (SoupMessage *request)
{
  g_object_set_data_full (G_OBJECT (request), "cancel",
                          g_object_ref (app->cancel), g_object_unref);
  soup_session_send_async (app->session, request, G_PRIORITY_DEFAULT,
                           app->cancel, response_cb, request);
}

/* Send the body history only; do not feed reasoning-only records or
   placeholders back to the API. */
static gchar *
request_body (guint  turns,
              gsize *len)
{
  JsonArray *payload = json_array_new ();
  for (guint i = 0; i < turns; i++)
    {
      JsonObject *msg = json_array_get_object_element (app->chat->messages, i);
      const gchar *role = json_object_get_string_member (msg, "role");
      const gchar *content = json_object_get_string_member (msg, "content");
      if (g_str_equal (role, "assistant") && !*content)
        continue;
      JsonObject *clean = json_object_new ();
      json_object_set_string_member (clean, "role", role);
      json_object_set_string_member (clean, "content", content);
      json_array_add_object_element (payload, clean);
    }

  gboolean thinking = !g_str_equal (app->effort, "none");
  JsonObject *root = json_object_new ();
  json_object_set_string_member (root, "model", app->model);
  json_object_set_boolean_member (root, "stream", TRUE);
  json_object_set_array_member (root, "messages", payload);

  JsonObject *thinking_obj = json_object_new ();
  json_object_set_string_member (thinking_obj, "type",
                                 thinking ? "enabled" : "disabled");
  json_object_set_object_member (root, "thinking", thinking_obj);
  if (thinking)
    json_object_set_string_member (root, "reasoning_effort", app->effort);

  JsonNode *node = json_node_alloc ();
  json_node_init (node, JSON_NODE_OBJECT);
  json_node_take_object (node, root);
  JsonGenerator *gen = json_generator_new ();
  json_generator_set_root (gen, node);
  gchar *body = json_generator_to_data (gen, len);
  g_object_unref (gen);
  json_node_free (node);
  return body;
}

/* Truncate the history when the edit draft is sent; a normal draft is
   unaffected by an edit and resend. */
static void
commit_message (const gchar *text)
{
  gboolean editing = app->chat->edit_from != G_MAXUINT;
  while (editing &&
         json_array_get_length (app->chat->messages) > app->chat->edit_from)
    json_array_remove_element (app->chat->messages, app->chat->edit_from);
  clear_edit (app->chat);
  if (!editing)
    {
      g_free (app->chat->draft);
      app->chat->draft = g_strdup ("");
    }
  JsonObject *user_msg = json_object_new ();
  json_object_set_string_member (user_msg, "role", "user");
  json_object_set_string_member (user_msg, "content", text);
  json_array_add_object_element (app->chat->messages, user_msg);
}

/* Treat only blank text as empty; do not trim code indentation or trailing
   whitespace. */
static gchar *
input_message (void)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (app->input));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds (buffer, &start, &end);
  gchar *text = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
  for (const gchar *p = text; *p; p++)
    if (!g_ascii_isspace (*p))
      return text;
  g_free (text);
  return NULL;
}

static void
send_message (void)
{
  if (app->busy)
    {
      g_cancellable_cancel (app->cancel);
      return;
    }
  if (app->api_key == NULL)
    {
      prompt_api_key ();
      return;
    }

  gchar *text = input_message ();
  if (text == NULL)
    return;

  commit_message (text);

  if (!*app->chat->title)
    {
      /* Take the first 30 characters of the first line; cutting by bytes
         would break UTF-8. */
      gchar *trimmed = g_strstrip (g_strdup (text));
      gchar *line = g_strndup (trimmed, strcspn (trimmed, "\n"));
      g_free (trimmed);
      gchar *title = g_strstrip (
          g_utf8_substring (line, 0, MIN (30, g_utf8_strlen (line, -1))));
      g_free (line);
      g_free (app->chat->title);
      app->chat->title = title;
      update_title ();
    }

  restore_input ();
  rebuild_sidebar ();
  g_free (text);
  start_request ();
  save_session ();
}

/* Resend the last user message; regenerate and retry after a failure both
   come here. */
static void
regenerate (void)
{
  if (app->busy || app->chat == NULL)
    return;
  if (app->chat->edit_from != G_MAXUINT)
    {
      app_toast (_ ("Finish or cancel editing before regenerating."));
      return;
    }
  if (app->api_key == NULL)
    {
      prompt_api_key ();
      return;
    }

  guint keep = json_array_get_length (app->chat->messages);
  while (keep > 0)
    {
      JsonObject *msg =
          json_array_get_object_element (app->chat->messages, keep - 1);
      if (g_str_equal (json_object_get_string_member (msg, "role"), "user"))
        break;
      keep--;
    }
  if (!keep)
    return;

  while (json_array_get_length (app->chat->messages) > keep)
    json_array_remove_element (app->chat->messages, keep);
  start_request ();
}

/* Start a request from the last user message in the session and add a
   placeholder bubble for the reply. */
static void
start_request (void)
{
  if (app->busy)
    return;

  JsonObject *assistant_msg = json_object_new ();
  json_object_set_string_member (assistant_msg, "role", "assistant");
  json_object_set_string_member (assistant_msg, "content", "");
  json_array_add_object_element (app->chat->messages, assistant_msg);
  guint turns = json_array_get_length (app->chat->messages) - 1;

  clear_messages ();
  render_history (turns);
  gtk_stack_set_visible_child_name (GTK_STACK (app->stack), "chat");

  app->reply_bubble = bubble_new ("assistant");
  app->reply_body = g_object_get_data (G_OBJECT (app->reply_bubble), "body");
  app->reply_msg = assistant_msg;
  app->reply_label = gtk_label_new ("…");
  gtk_label_set_wrap (GTK_LABEL (app->reply_label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (app->reply_label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (app->reply_label), 0);
  gtk_label_set_selectable (GTK_LABEL (app->reply_label), TRUE);
  gtk_box_append (GTK_BOX (app->reply_body), app->reply_label);

  app->cancel = g_cancellable_new ();
  app->request_started = g_get_monotonic_time ();
  app->thought_seconds = 0;
  set_busy (TRUE);
  scroll_bottom ();

  gsize len;
  gchar *body = request_body (turns, &len);

  SoupMessage *request = soup_message_new (SOUP_METHOD_POST, API_URL);
  gchar *auth = g_strdup_printf ("Bearer %s", app->api_key);
  soup_message_headers_append (soup_message_get_request_headers (request),
                               "Authorization", auth);
  GBytes *bytes = g_bytes_new_take (body, len);
  soup_message_set_request_body_from_bytes (request, "application/json", bytes);
  g_bytes_unref (bytes);
  send_request (request);

  g_free (auth);
}

/* ---------- input ---------- */

static gboolean
on_input_key (GtkEventControllerKey *controller G_GNUC_UNUSED,
              guint                  keyval,
              guint                  keycode G_GNUC_UNUSED,
              GdkModifierType        state,
              gpointer               data G_GNUC_UNUSED)
{
  if (keyval == GDK_KEY_Escape && !app->preedit &&
      app->chat->edit_from != G_MAXUINT)
    {
      on_cancel_edit (NULL, NULL);
      return GDK_EVENT_STOP;
    }
  if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) &&
      !(state & GDK_SHIFT_MASK))
    {
      /* This controller runs before the one GtkTextView installs (event
         controllers run in the order they were added). Enter during composition
         commits the text, so pass it on to the input method. */
      if (app->preedit)
        return GDK_EVENT_PROPAGATE;
      if (app->busy)
        app_toast (_ ("Generating. Click ■ to stop."));
      else
        send_message ();
      return GDK_EVENT_STOP;
    }
  return GDK_EVENT_PROPAGATE;
}

/* The card grows with its content and scrolls internally at the limit. */
static void
fit_input_height (void)
{
  if (gtk_text_buffer_get_char_count (
          gtk_text_view_get_buffer (GTK_TEXT_VIEW (app->input))) == 0)
    {
      gtk_widget_set_size_request (app->input_frame, -1, INPUT_MIN_HEIGHT);
      return;
    }

  /* The text area has 14px margins on both sides; measure the real usable
     width, or the line count is underestimated. */
  int width = gtk_widget_get_width (app->input_frame) - 28;
  if (width <= 0)
    return;

  int min = 0, natural = 0;
  gtk_widget_measure (app->input, GTK_ORIENTATION_VERTICAL, width, &min,
                      &natural, NULL, NULL);
  /* The result of gtk_widget_measure already includes the margins of the text
     view (14px top, 42px bottom), so add only 4px for rounding, not the margins
     again. */
  gtk_widget_set_size_request (
      app->input_frame, -1,
      CLAMP (natural + 4, INPUT_MIN_HEIGHT, INPUT_MAX_HEIGHT));
}

static void
update_send_state (void)
{
  gboolean has_text = gtk_text_buffer_get_char_count (gtk_text_view_get_buffer (
                          GTK_TEXT_VIEW (app->input))) > 0;
  gtk_widget_set_sensitive (app->send, app->busy || has_text);
  /* The buffer is empty during composition, and the placeholder would
     collapse with the preedit text. */
  if (app->placeholder != NULL)
    gtk_widget_set_visible (app->placeholder, !has_text && !app->preedit);
}

static void
on_input_preedit (GtkTextView *view G_GNUC_UNUSED,
                  const gchar *preedit,
                  gpointer     data G_GNUC_UNUSED)
{
  app->preedit = preedit != NULL && *preedit;
  update_send_state ();
}

/* After a multi-line paste, the text view layout updates only on the next
   frame. Measuring at once returns the old height, so measure from an idle
   callback. */
static guint fit_source;

/* With the EXTERNAL policy, GtkScrolledWindow sometimes does not update
   page-size: the computed max is larger than the real one, so the view scrolls
   past the end (blank space below, or a list that does not scroll further).
   After a geometry change, correct it over several frames and pull the
   page-size of both scrolled areas back to the real visible height. */
static guint sync_frames_left, sync_tick;

static void
clamp_adjustment (GtkAdjustment *adj)
{
  gdouble max =
      gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj);
  if (max > 0 && gtk_adjustment_get_value (adj) > max)
    gtk_adjustment_set_value (adj, max);
}

static void
sync_one_scroller (GtkScrolledWindow *scroller,
                   int                viewport)
{
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (scroller);
  if (viewport <= 0 ||
      ABS (gtk_adjustment_get_page_size (adj) - viewport) < 0.5)
    {
      return;
    }
  gtk_adjustment_set_page_size (adj, viewport);
  clamp_adjustment (adj);
}

/* Correct page-size and invalidate the whole message area subtree once: a
   taller input box changes the message area height, but the measure caches of
   the text views inside do not invalidate by themselves. The last message then
   stays measured at its old size, which shows as missing content and a bottom
   that cannot be reached, until the next input triggers a new layout. */
static void
sync_geometry_now (void)
{
  if (app->input_frame == NULL || app->scroller == NULL)
    return;

  input_stick.setting = TRUE;
  sync_one_scroller (GTK_SCROLLED_WINDOW (app->input_frame),
                     gtk_widget_get_height (app->input));
  sync_one_scroller (GTK_SCROLLED_WINDOW (app->scroller),
                     gtk_widget_get_height (app->scroller));
  input_stick.setting = FALSE;

  for (GtkWidget *msg = gtk_widget_get_first_child (app->messages); msg != NULL;
       msg = gtk_widget_get_next_sibling (msg))
    gtk_widget_queue_resize (msg);
  gtk_widget_queue_resize (GTK_WIDGET (app->messages));
  gtk_widget_queue_draw (app->scroller);
}

static gboolean
geometry_sync_tick (GtkWidget     *widget G_GNUC_UNUSED,
                    GdkFrameClock *clock G_GNUC_UNUSED,
                    gpointer       data G_GNUC_UNUSED)
{
  sync_geometry_now ();

  if (--sync_frames_left)
    return G_SOURCE_CONTINUE;
  sync_tick = 0;
  return G_SOURCE_REMOVE;
}

static guint sync_retry_timer;

static gboolean
geometry_sync_retry_cb (gpointer data G_GNUC_UNUSED)
{
  sync_retry_timer = 0;
  sync_geometry_now ();
  return G_SOURCE_REMOVE;
}

static void
schedule_geometry_sync (void)
{
  if (sync_retry_timer)
    g_source_remove (sync_retry_timer);
  sync_retry_timer = g_timeout_add (250, geometry_sync_retry_cb, NULL);

  if (sync_frames_left)
    return;
  sync_frames_left = 3;
  sync_tick =
      gtk_widget_add_tick_callback (app->input, geometry_sync_tick, NULL, NULL);
}

static gboolean
caret_at_end (void)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (app->input));
  GtkTextIter insert, end;
  gtk_text_buffer_get_iter_at_mark (buffer, &insert,
                                    gtk_text_buffer_get_insert (buffer));
  gtk_text_buffer_get_end_iter (buffer, &end);
  return gtk_text_iter_equal (&insert, &end);
}

/* Stick to the bottom whenever the content height changes (only with the cursor
   at the end, otherwise the user edits in the middle), until the user scrolls
   up. A paste inserts a whole block, and the height settles only after a few
   frames, so an event-driven check is more reliable than a timer. */
static void
on_input_content_changed (GtkAdjustment *adj G_GNUC_UNUSED)
{
  if (input_stick.follow && caret_at_end ())
    stick_queue_scroll (&input_stick);
}

static void
on_input_value_changed (GtkAdjustment *adj G_GNUC_UNUSED)
{
  stick_on_value_changed (&input_stick);
}

static guint fit_retry_timer;

static gboolean
fit_input_height_retry (gpointer data G_GNUC_UNUSED)
{
  fit_retry_timer = 0;
  fit_input_height ();
  return G_SOURCE_REMOVE;
}

static gboolean
fit_input_height_idle (gpointer data G_GNUC_UNUSED)
{
  fit_source = 0;
  fit_input_height ();
  /* A paste inserts a whole block, so this frame may still measure the old
     height; measure again after the layout settles. */
  if (fit_retry_timer)
    g_source_remove (fit_retry_timer);
  fit_retry_timer = g_timeout_add (120, fit_input_height_retry, NULL);
  return G_SOURCE_REMOVE;
}

static void
refresh_input (void)
{
  update_send_state ();
  if (!fit_source)
    fit_source = g_idle_add (fit_input_height_idle, NULL);
}

static gboolean
save_drafts (gpointer data G_GNUC_UNUSED)
{
  app->draft_save_source = 0;
  save_session ();
  return G_SOURCE_REMOVE;
}

static void
on_input_changed (GtkTextBuffer *buffer,
                  gpointer       data G_GNUC_UNUSED)
{
  if (!app->restoring_input)
    {
      GtkTextIter start, end;
      gtk_text_buffer_get_bounds (buffer, &start, &end);
      gchar **draft = app->chat->edit_from == G_MAXUINT ? &app->chat->draft
                                                        : &app->chat->edit_text;
      g_free (*draft);
      *draft = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
      if (app->draft_save_source)
        g_source_remove (app->draft_save_source);
      app->draft_save_source = g_timeout_add (750, save_drafts, NULL);
    }
  refresh_input ();
  schedule_geometry_sync ();
}

static void
on_frame_width (GObject    *frame G_GNUC_UNUSED,
                GParamSpec *pspec G_GNUC_UNUSED,
                gpointer    data G_GNUC_UNUSED)
{
  fit_input_height ();
  schedule_geometry_sync ();
}

static void
on_send_clicked (GtkButton *button G_GNUC_UNUSED,
                 gpointer   data G_GNUC_UNUSED)
{
  send_message ();
}

/* ---------- lifecycle ---------- */

/* Save on quit commits the text and reasoning received so far, then clears the
   request state. */
static void
flush_stream (void)
{
  if (!app->busy || app->reply_msg == NULL)
    return;
  if (!record_reply ())
    json_array_remove_element (app->chat->messages,
                               json_array_get_length (app->chat->messages) - 1);
  clear_stream_state ();
}

static void
cancel_ui_updates (void)
{
  g_clear_handle_id (&app->draft_save_source, g_source_remove);
  g_clear_handle_id (&chat_stick.scroll_source, g_source_remove);
  g_clear_handle_id (&input_stick.scroll_source, g_source_remove);
  g_clear_handle_id (&chat_bar_timer, g_source_remove);
  g_clear_handle_id (&fit_retry_timer, g_source_remove);
  g_clear_handle_id (&sync_retry_timer, g_source_remove);
  g_clear_handle_id (&fit_source, g_source_remove);
  g_clear_handle_id (&focus_source, g_source_remove);
  if (sync_tick)
    gtk_widget_remove_tick_callback (app->input, sync_tick);
  sync_tick = sync_frames_left = 0;
}

static gboolean
on_close (GtkWindow *window G_GNUC_UNUSED,
          gpointer   data G_GNUC_UNUSED)
{
  if (app->closing)
    return FALSE;
  if (app->cancel != NULL)
    g_cancellable_cancel (app->cancel);
  flush_stream ();
  if (!save_session ())
    return TRUE; /* The draft is still in memory; do not lose it on a close. */
  cancel_ui_updates ();
  app->closing = TRUE;
  return FALSE;
}

static void
on_shutdown (GApplication *application G_GNUC_UNUSED,
             gpointer      data G_GNUC_UNUSED)
{
  if (app->window != NULL && !app->closing && on_close (NULL, NULL))
    {
      g_printerr ("%s\n", _ ("Failed to save conversation"));
      cancel_ui_updates ();
    }
}

static GtkCssProvider *dark_css;

/* Raise contrast in the dark theme: the bubble background and the input box are
   too close to the background color. */
static void
refresh_dark_css (void)
{
  gboolean dark = adw_style_manager_get_dark (adw_style_manager_get_default ());
  if (dark_css == NULL)
    dark_css = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (
      dark_css, dark ? ".user-bubble {"
                       " background: alpha(@accent_bg_color, .32); }"

                       ".input-frame {"
                       " background: alpha(@window_fg_color, .075); }"
                     : "");
}

static void
on_dark_changed (GObject    *manager G_GNUC_UNUSED,
                 GParamSpec *pspec G_GNUC_UNUSED,
                 gpointer    data G_GNUC_UNUSED)
{
  refresh_dark_css ();
}

static void
load_css (void)
{
  if (dark_css != NULL)
    return;
  GtkCssProvider *css = gtk_css_provider_new ();
  gtk_css_provider_load_from_resource (css, "/cc/arthur63/gchat/style.css");
  gtk_style_context_add_provider_for_display (
      gdk_display_get_default (), GTK_STYLE_PROVIDER (css),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  refresh_dark_css ();
  gtk_style_context_add_provider_for_display (
      gdk_display_get_default (), GTK_STYLE_PROVIDER (dark_css),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_signal_connect (adw_style_manager_get_default (), "notify::dark",
                    G_CALLBACK (on_dark_changed), NULL);
  g_object_unref (css);
}

/* ---------- settings ---------- */

static const Choice THEMES[] = { { "system", N_ ("System default") },
                                 { "light", N_ ("Light") },
                                 { "dark", N_ ("Dark") } };
static const Choice LANGUAGES[] = { { "system", N_ ("System language") },
                                    { "en", "English" },
                                    { "zh_CN", "简体中文" } };

static void
apply_theme (const gchar *name)
{
  AdwColorScheme scheme = ADW_COLOR_SCHEME_DEFAULT;
  if (g_strcmp0 (name, "light") == 0)
    scheme = ADW_COLOR_SCHEME_FORCE_LIGHT;
  else if (g_strcmp0 (name, "dark") == 0)
    scheme = ADW_COLOR_SCHEME_FORCE_DARK;
  adw_style_manager_set_color_scheme (adw_style_manager_get_default (), scheme);
}

static gboolean
save_selected_choice (AdwComboRow  *row,
                      const gchar  *key,
                      const Choice *choices,
                      guint         count,
                      const gchar  *failure)
{
  guint selected = adw_combo_row_get_selected (row);
  guint saved =
      GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row), "saved-choice"));
  if (selected >= count || selected == saved)
    return FALSE;
  if (!config_set (key, choices[selected].value))
    {
      adw_combo_row_set_selected (row, saved);
      app_toast (failure);
      return FALSE;
    }
  g_object_set_data (G_OBJECT (row), "saved-choice",
                     GUINT_TO_POINTER (selected));
  return TRUE;
}

static void
on_theme_selected (GObject    *combo,
                   GParamSpec *pspec G_GNUC_UNUSED,
                   gpointer    data G_GNUC_UNUSED)
{
  AdwComboRow *row = ADW_COMBO_ROW (combo);
  if (save_selected_choice (row, "theme", THEMES, G_N_ELEMENTS (THEMES),
                            _ ("Failed to save theme")))
    {
      apply_theme (THEMES[adw_combo_row_get_selected (row)].value);
    }
}

static void
on_language_selected (GObject    *combo,
                      GParamSpec *pspec G_GNUC_UNUSED,
                      gpointer    data G_GNUC_UNUSED)
{
  if (save_selected_choice (ADW_COMBO_ROW (combo), "language", LANGUAGES,
                            G_N_ELEMENTS (LANGUAGES),
                            _ ("Failed to save language")))
    {
      app_toast (_ ("Restart the app to apply language changes."));
    }
}

static void
on_settings_key_apply (AdwEntryRow *row,
                       gpointer     data G_GNUC_UNUSED)
{
  if (!*gtk_editable_get_text (GTK_EDITABLE (row)))
    {
      app_toast (_ ("API key cannot be empty"));
      return;
    }
  if (save_api_key (gtk_editable_get_text (GTK_EDITABLE (row))))
    app_toast (_ ("API key saved"));
}

static AdwActionRow *
settings_info_row (const gchar *title,
                   const gchar *subtitle)
{
  AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  adw_action_row_set_subtitle (row, subtitle);
  return row;
}

static void
on_settings_clicked (GtkButton *button G_GNUC_UNUSED,
                     gpointer   data G_GNUC_UNUSED)
{
  AdwPreferencesDialog *dialog =
      ADW_PREFERENCES_DIALOG (adw_preferences_dialog_new ());
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());

  /* API */
  AdwPreferencesGroup *api =
      ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (api, "API");

  AdwEntryRow *key_row = ADW_ENTRY_ROW (adw_password_entry_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (key_row), _ ("API key"));
  if (app->api_key != NULL)
    gtk_editable_set_text (GTK_EDITABLE (key_row), app->api_key);
  adw_entry_row_set_show_apply_button (key_row, TRUE);
  g_signal_connect (key_row, "apply", G_CALLBACK (on_settings_key_apply), NULL);
  adw_preferences_group_add (api, GTK_WIDGET (key_row));

  const gchar *env = g_getenv ("DEEPSEEK_API_KEY");
  gchar *config_file = store_config_path ();
  gchar *key_source =
      (env != NULL && *env)
          ? g_strdup (_ ("DEEPSEEK_API_KEY environment variable (overrides "
                         "this setting)"))
          : g_strdup_printf (_ ("Local configuration file: %s"), config_file);
  adw_preferences_group_add (
      api, GTK_WIDGET (settings_info_row (_ ("Key source"), key_source)));
  g_free (key_source);
  g_free (config_file);
  adw_preferences_page_add (page, api);

  /* Appearance */
  AdwPreferencesGroup *look =
      ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (look, _ ("Appearance"));

  AdwComboRow *theme = ADW_COMBO_ROW (adw_combo_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (theme), _ ("Theme"));
  GtkStringList *theme_model = gtk_string_list_new (NULL);
  for (guint i = 0; i < G_N_ELEMENTS (THEMES); i++)
    gtk_string_list_append (theme_model, _ (THEMES[i].label));
  adw_combo_row_set_model (theme, G_LIST_MODEL (theme_model));
  g_object_unref (theme_model);
  gchar *saved_theme = config_get ("theme");
  guint theme_index = 0;
  for (guint i = 0; i < G_N_ELEMENTS (THEMES); i++)
    if (g_strcmp0 (saved_theme, THEMES[i].value) == 0)
      theme_index = i;
  g_free (saved_theme);
  adw_combo_row_set_selected (theme, theme_index);
  g_object_set_data (G_OBJECT (theme), "saved-choice",
                     GUINT_TO_POINTER (theme_index));
  g_signal_connect (theme, "notify::selected", G_CALLBACK (on_theme_selected),
                    NULL);
  adw_preferences_group_add (look, GTK_WIDGET (theme));

  AdwComboRow *language = ADW_COMBO_ROW (adw_combo_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (language),
                                 _ ("Language"));
  adw_action_row_set_subtitle (
      ADW_ACTION_ROW (language),
      _ ("Restart the app to apply language changes."));
  GtkStringList *language_model = gtk_string_list_new (NULL);
  for (guint i = 0; i < G_N_ELEMENTS (LANGUAGES); i++)
    gtk_string_list_append (language_model, i == 0 ? _ (LANGUAGES[i].label)
                                                   : LANGUAGES[i].label);
  adw_combo_row_set_model (language, G_LIST_MODEL (language_model));
  g_object_unref (language_model);
  gchar *saved_language = config_get ("language");
  guint language_index = 0;
  for (guint i = 0; i < G_N_ELEMENTS (LANGUAGES); i++)
    if (g_strcmp0 (saved_language, LANGUAGES[i].value) == 0)
      language_index = i;
  g_free (saved_language);
  adw_combo_row_set_selected (language, language_index);
  g_object_set_data (G_OBJECT (language), "saved-choice",
                     GUINT_TO_POINTER (language_index));
  g_signal_connect (language, "notify::selected",
                    G_CALLBACK (on_language_selected), NULL);
  adw_preferences_group_add (look, GTK_WIDGET (language));
  adw_preferences_page_add (page, look);

  /* About */
  AdwPreferencesGroup *about =
      ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (about, _ ("About"));
  adw_preferences_group_add (
      about, GTK_WIDGET (settings_info_row ("gchat", GCHAT_VERSION
                                            " · GTK 4 + libadwaita")));
  gchar *session_path = store_session_path ();
  adw_preferences_group_add (
      about,
      GTK_WIDGET (settings_info_row (_ ("Conversation data"), session_path)));
  g_free (session_path);
  adw_preferences_page_add (page, about);

  adw_dialog_set_title (ADW_DIALOG (dialog), _ ("Settings"));
  adw_preferences_dialog_set_search_enabled (dialog, FALSE);
  adw_preferences_dialog_add (dialog, page);
  adw_dialog_present (ADW_DIALOG (dialog), app->window);
}

static void
on_search_preedit (GtkText     *text G_GNUC_UNUSED,
                   const gchar *preedit,
                   gpointer     context)
{
  g_object_set_data (context, "search-preedit",
                     GINT_TO_POINTER (preedit != NULL && *preedit));
}

static gboolean
sync_search_preedit (GSignalInvocationHint *hint G_GNUC_UNUSED,
                     guint                  n_values G_GNUC_UNUSED,
                     const GValue          *values,
                     gpointer               context)
{
  if (g_value_get_object (values) == context &&
      g_object_get_data (context, "search-preedit") != NULL)
    {
      gchar *preedit = NULL;
      gtk_im_context_get_preedit_string (context, &preedit, NULL, NULL);
      /* macOS can clear the preedit text before the commit, so GtkText must
         drop the old preedit cursor first. */
      if (preedit != NULL && !*preedit)
        g_signal_emit_by_name (context, "preedit-changed");
      g_free (preedit);
    }
  return TRUE;
}

static void
on_search_context_gone (gpointer hook,
                        GObject *context G_GNUC_UNUSED)
{
  g_signal_remove_emission_hook (
      g_signal_lookup ("commit", GTK_TYPE_IM_CONTEXT), GPOINTER_TO_SIZE (hook));
}

static void
connect_search_ime (GtkWidget *search)
{
  GtkWidget *text =
      GTK_WIDGET (gtk_editable_get_delegate (GTK_EDITABLE (search)));
  GListModel *controllers = gtk_widget_observe_controllers (text);
  for (guint i = 0; i < g_list_model_get_n_items (controllers); i++)
    {
      GObject *controller = g_list_model_get_item (controllers, i);
      GtkIMContext *context = GTK_IS_EVENT_CONTROLLER_KEY (controller)
                                  ? gtk_event_controller_key_get_im_context (
                                        GTK_EVENT_CONTROLLER_KEY (controller))
                                  : NULL;
      if (context != NULL)
        {
          g_signal_connect (text, "preedit-changed",
                            G_CALLBACK (on_search_preedit), context);
          /* The emission hook runs before the GtkText commit handler and dies
             with the input method context. */
          gulong hook = g_signal_add_emission_hook (
              g_signal_lookup ("commit", GTK_TYPE_IM_CONTEXT), 0,
              sync_search_preedit, context, NULL);
          g_object_weak_ref (G_OBJECT (context), on_search_context_gone,
                             GSIZE_TO_POINTER (hook));
          g_object_unref (controller);
          break;
        }
      g_object_unref (controller);
    }
  g_object_unref (controllers);
}

static void
on_search_changed (GtkSearchEntry *entry G_GNUC_UNUSED,
                   gpointer        data G_GNUC_UNUSED)
{
  rebuild_sidebar ();
}

static void
on_search_stop (GtkSearchEntry *entry,
                gpointer        data G_GNUC_UNUSED)
{
  gtk_editable_set_text (GTK_EDITABLE (entry), "");
  gtk_widget_grab_focus (app->input);
}

static GtkWidget *
build_sidebar (void)
{
  GtkWidget *view = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *brand = gtk_label_new ("gchat");
  gtk_widget_add_css_class (brand, "heading");
  g_object_set (brand, "margin-start", 6, NULL);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), brand);
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header),
                                   gtk_label_new (NULL));
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (view), header);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *new_row = gtk_button_new ();
  gtk_widget_add_css_class (new_row, "new-chat-row");
  gtk_widget_set_margin_start (new_row, 8);
  gtk_widget_set_margin_end (new_row, 8);
  gtk_widget_set_margin_top (new_row, 4);
  gtk_widget_set_margin_bottom (new_row, 2);
  GtkWidget *nb = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *ni = gtk_image_new_from_icon_name ("document-new-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (ni), 15);
  GtkWidget *nl = gtk_label_new (_ ("New chat"));
  gtk_label_set_xalign (GTK_LABEL (nl), 0);
  gtk_box_append (GTK_BOX (nb), ni);
  gtk_box_append (GTK_BOX (nb), nl);
  gtk_button_set_child (GTK_BUTTON (new_row), nb);
  gtk_widget_set_tooltip_text (new_row, _ ("New chat (Ctrl+N)"));
  g_signal_connect (new_row, "clicked", G_CALLBACK (on_new_chat_clicked), NULL);
  app->new_btn = new_row;
  gtk_box_append (GTK_BOX (content), new_row);

  app->search = gtk_search_entry_new ();
  connect_search_ime (app->search);
  g_object_set (app->search, "placeholder-text", _ ("Search conversations"),
                "margin-start", 8, "margin-end", 8, "margin-top", 6,
                "margin-bottom", 8, NULL);
  gtk_accessible_update_property (GTK_ACCESSIBLE (app->search),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _ ("Search conversations"), -1);
  gtk_widget_set_tooltip_text (app->search,
                               _ ("Search conversations (Ctrl/Cmd+F)"));
  g_signal_connect (app->search, "search-changed",
                    G_CALLBACK (on_search_changed), NULL);
  g_signal_connect (app->search, "stop-search", G_CALLBACK (on_search_stop),
                    NULL);
  gtk_box_append (GTK_BOX (content), app->search);

  app->sidebar_list = gtk_list_box_new ();
  GtkWidget *no_results = gtk_label_new (_ ("No conversations found"));
  gtk_widget_add_css_class (no_results, "dimmed");
  g_object_set (no_results, "margin-top", 16, "margin-bottom", 16, NULL);
  gtk_list_box_set_placeholder (GTK_LIST_BOX (app->sidebar_list), no_results);
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (app->sidebar_list),
                                   GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class (app->sidebar_list, "navigation-sidebar");
  gtk_widget_add_css_class (app->sidebar_list, "sidebar-list");
  g_signal_connect (app->sidebar_list, "row-selected",
                    G_CALLBACK (on_row_selected), NULL);

  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller),
                                 app->sidebar_list);
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_box_append (GTK_BOX (content), scroller);

  GtkWidget *settings = gtk_button_new ();
  gtk_widget_add_css_class (settings, "settings-row");
  gtk_widget_set_margin_start (settings, 8);
  gtk_widget_set_margin_end (settings, 8);
  gtk_widget_set_margin_top (settings, 2);
  gtk_widget_set_margin_bottom (settings, 8);
  GtkWidget *sb = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *si = gtk_image_new_from_icon_name ("emblem-system-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (si), 15);
  GtkWidget *sl = gtk_label_new (_ ("Settings"));
  gtk_label_set_xalign (GTK_LABEL (sl), 0);
  gtk_box_append (GTK_BOX (sb), si);
  gtk_box_append (GTK_BOX (sb), sl);
  gtk_button_set_child (GTK_BUTTON (settings), sb);
  gtk_widget_set_tooltip_text (settings, _ ("Settings (Ctrl/Cmd+,)"));
  g_signal_connect (settings, "clicked", G_CALLBACK (on_settings_clicked),
                    NULL);
  gtk_box_append (GTK_BOX (content), settings);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (view), content);
  gtk_widget_set_size_request (view, 240, -1);
  return view;
}

static GtkWidget *
build_content (void)
{
  GtkWidget *view = adw_toolbar_view_new ();

  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *toggle =
      icon_button ("sidebar-show-symbolic", 15, FALSE, _ ("Conversations"));
  g_signal_connect (toggle, "clicked", G_CALLBACK (on_toggle_sidebar), NULL);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), toggle);

  app->title_label = adw_window_title_new ("gchat", NULL);
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), app->title_label);

  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (view), header);

  GtkWidget *center = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  app->stack = gtk_stack_new ();
  gtk_widget_set_vexpand (app->stack, TRUE);

  /* EXTERNAL: the scrollbar takes no part in layout and no width, so draw one
     that floats above the content instead. The column width stays the same when
     the content exceeds one screen, and centered content does not shift
     sideways. */
  app->scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (app->scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);
  app->messages = gtk_box_new (GTK_ORIENTATION_VERTICAL, 26);
  g_object_set (app->messages, "margin-top", 24, "margin-bottom", 24,
                "margin-start", 16, "margin-end", 16, NULL);
  GtkWidget *clamp = adw_clamp_new ();
  gtk_widget_set_hexpand (clamp, TRUE);
  gtk_widget_set_halign (clamp, GTK_ALIGN_FILL);
  adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 760);
  adw_clamp_set_tightening_threshold (ADW_CLAMP (clamp), 620);
  adw_clamp_set_child (ADW_CLAMP (clamp), app->messages);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (app->scroller), clamp);

  GtkWidget *empty = adw_status_page_new ();
  adw_status_page_set_title (ADW_STATUS_PAGE (empty),
                             _ ("Start a conversation with DeepSeek"));
  adw_status_page_set_description (
      ADW_STATUS_PAGE (empty),
      _ ("Enter to send · Shift+Enter for a new line · Ctrl+N for a new chat"));

  GtkAdjustment *vadj =
      gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (app->scroller));
  app->chat_bar = gtk_scrollbar_new (GTK_ORIENTATION_VERTICAL, vadj);
  gtk_widget_add_css_class (app->chat_bar, "vertical");
  gtk_widget_add_css_class (app->chat_bar, "chat-scrollbar");
  gtk_widget_set_halign (app->chat_bar, GTK_ALIGN_END);
  gtk_widget_set_valign (app->chat_bar, GTK_ALIGN_FILL);
  gtk_widget_set_size_request (app->chat_bar, 12, -1);
  GtkEventController *hover = gtk_event_controller_motion_new ();
  g_signal_connect (hover, "enter", G_CALLBACK (on_chat_bar_enter), NULL);
  gtk_widget_add_controller (app->chat_bar, hover);
  chat_stick.adj = vadj;
  g_signal_connect (vadj, "changed", G_CALLBACK (on_content_changed), NULL);
  g_signal_connect (vadj, "value-changed", G_CALLBACK (on_value_changed), NULL);

  GtkWidget *chat_overlay = gtk_overlay_new ();
  gtk_overlay_set_child (GTK_OVERLAY (chat_overlay), app->scroller);
  gtk_overlay_add_overlay (GTK_OVERLAY (chat_overlay), app->chat_bar);
  gtk_stack_add_named (GTK_STACK (app->stack), chat_overlay, "chat");
  gtk_stack_add_named (GTK_STACK (app->stack), empty, "empty");
  gtk_box_append (GTK_BOX (center), app->stack);

  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  g_object_set (bar, "margin-top", 8, "margin-bottom", 16, "margin-start", 16,
                "margin-end", 16, NULL);
  GtkWidget *bar_clamp = adw_clamp_new ();
  gtk_widget_set_hexpand (bar_clamp, TRUE);
  gtk_widget_set_halign (bar_clamp, GTK_ALIGN_FILL);
  adw_clamp_set_maximum_size (ADW_CLAMP (bar_clamp), 760);
  adw_clamp_set_tightening_threshold (ADW_CLAMP (bar_clamp), 620);
  adw_clamp_set_child (ADW_CLAMP (bar_clamp), bar);

  app->edit_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *edit_label = gtk_label_new (_ ("Editing message"));
  gtk_label_set_xalign (GTK_LABEL (edit_label), 0);
  gtk_widget_set_hexpand (edit_label, TRUE);
  gtk_box_append (GTK_BOX (app->edit_bar), edit_label);
  GtkWidget *cancel_edit = gtk_button_new_with_label (_ ("Cancel"));
  gtk_widget_add_css_class (cancel_edit, "flat");
  gtk_widget_set_tooltip_text (cancel_edit, _ ("Cancel editing (Esc)"));
  g_signal_connect (cancel_edit, "clicked", G_CALLBACK (on_cancel_edit), NULL);
  gtk_box_append (GTK_BOX (app->edit_bar), cancel_edit);
  gtk_widget_set_visible (app->edit_bar, FALSE);
  gtk_box_append (GTK_BOX (bar), app->edit_bar);

  GtkWidget *frame = gtk_scrolled_window_new ();
  gtk_widget_set_size_request (frame, -1, INPUT_MIN_HEIGHT);
  gtk_widget_set_hexpand (frame, TRUE);
  gtk_widget_add_css_class (frame, "card");
  gtk_widget_add_css_class (frame, "input-frame");
  /* Use EXTERNAL to hide the scrollbar and reserve no width; keep wheel and
     key scrolling. */
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (frame), GTK_POLICY_NEVER,
                                  GTK_POLICY_EXTERNAL);
  app->input_frame = frame;

  app->input = gtk_text_view_new ();
  gtk_widget_add_css_class (app->input, "chat-input");
  gtk_widget_set_tooltip_text (app->input, _ ("Focus input (Ctrl/Cmd+L)"));
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (app->input), GTK_WRAP_WORD_CHAR);
  g_object_set (app->input, "margin-top", 14, "margin-bottom", 42,
                "margin-start", 14, "margin-end", 14, NULL);
  gtk_accessible_update_property (
      GTK_ACCESSIBLE (app->input), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
      _ ("Message DeepSeek. Enter to send, Shift+Enter for a new line."), -1);
  GtkEventController *keys = gtk_event_controller_key_new ();
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_input_key), NULL);
  gtk_widget_add_controller (app->input, keys);
  g_signal_connect (app->input, "preedit-changed",
                    G_CALLBACK (on_input_preedit), NULL);

  /* GtkTextView has no placeholder, so overlay a label that takes no events. */
  app->placeholder = gtk_label_new (_ ("Message DeepSeek"));
  gtk_widget_add_css_class (app->placeholder, "dimmed");
  gtk_widget_add_css_class (app->placeholder, "chat-input");
  gtk_widget_set_can_target (app->placeholder, FALSE);
  gtk_widget_set_halign (app->placeholder, GTK_ALIGN_START);
  gtk_widget_set_valign (app->placeholder, GTK_ALIGN_START);
  g_object_set (app->placeholder, "margin-top", 14, "margin-start", 16, NULL);

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (frame), app->input);

  GtkAdjustment *input_adj =
      gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (frame));
  input_stick.adj = input_adj;
  g_signal_connect (input_adj, "changed", G_CALLBACK (on_input_content_changed),
                    NULL);
  g_signal_connect (input_adj, "value-changed",
                    G_CALLBACK (on_input_value_changed), NULL);

  GtkWidget *overlay = gtk_overlay_new ();
  gtk_overlay_set_child (GTK_OVERLAY (overlay), frame);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), app->placeholder);
  g_signal_connect (gtk_text_view_get_buffer (GTK_TEXT_VIEW (app->input)),
                    "changed", G_CALLBACK (on_input_changed), NULL);
  g_signal_connect (frame, "notify::width", G_CALLBACK (on_frame_width), NULL);

  app->send = gtk_button_new_from_icon_name ("go-up-symbolic");
  gtk_widget_add_css_class (app->send, "suggested-action");
  gtk_widget_add_css_class (app->send, "circular");
  gtk_widget_set_valign (app->send, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request (app->send, 32, 32);
  gtk_image_set_pixel_size (
      GTK_IMAGE (gtk_button_get_child (GTK_BUTTON (app->send))), 15);
  gtk_widget_set_tooltip_text (app->send, _ ("Send"));
  g_signal_connect (app->send, "clicked", G_CALLBACK (on_send_clicked), NULL);

  app->choice_popover = gtk_popover_new ();

  app->model_chip = gtk_menu_button_new ();
  gtk_widget_add_css_class (app->model_chip, "input-chip");
  gtk_widget_set_tooltip_text (app->model_chip,
                               _ ("Model and reasoning level"));
  GtkWidget *chip = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  app->chip_model = gtk_label_new (NULL);
  app->chip_effort = gtk_label_new (NULL);
  gtk_widget_add_css_class (app->chip_effort, "dimmed");
  GtkWidget *chevron = gtk_image_new_from_icon_name ("pan-down-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (chevron), 13);
  gtk_widget_add_css_class (chevron, "dimmed");
  gtk_box_append (GTK_BOX (chip), app->chip_model);
  gtk_box_append (GTK_BOX (chip), app->chip_effort);
  gtk_box_append (GTK_BOX (chip), chevron);
  gtk_menu_button_set_child (GTK_MENU_BUTTON (app->model_chip), chip);
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (app->model_chip),
                               app->choice_popover);
  refresh_choices ();

  /* The bottom row's chip and send button share one vertical center line. */
  GtkWidget *controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_valign (controls, GTK_ALIGN_END);
  gtk_widget_set_halign (controls, GTK_ALIGN_FILL);
  g_object_set (controls, "margin-start", 10, "margin-end", 8, "margin-bottom",
                8, NULL);

  GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (spacer, TRUE);
  gtk_box_append (GTK_BOX (controls), spacer);
  gtk_box_append (GTK_BOX (controls), app->model_chip);
  gtk_box_append (GTK_BOX (controls), app->send);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), controls);

  GtkWidget *hint = gtk_label_new (
      _ ("AI-generated content. Please verify important information."));
  gtk_widget_add_css_class (hint, "caption");
  gtk_widget_add_css_class (hint, "dimmed");
  gtk_widget_set_halign (hint, GTK_ALIGN_CENTER);
  g_object_set (hint, "margin-top", 6, NULL);

  gtk_box_append (GTK_BOX (bar), overlay);
  gtk_box_append (GTK_BOX (bar), hint);
  gtk_box_append (GTK_BOX (center), bar_clamp);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (view), center);
  return view;
}

static void
on_retry_action (GSimpleAction *action G_GNUC_UNUSED,
                 GVariant      *param,
                 gpointer       data G_GNUC_UNUSED)
{
  /* A toast can outlive its session, so it must not retry a prompt from another
     session. */
  if (param == NULL ||
      !g_str_equal (g_variant_get_string (param, NULL), app->chat->id))
    {
      return;
    }
  guint len = json_array_get_length (app->chat->messages);
  JsonObject *last =
      len ? json_array_get_object_element (app->chat->messages, len - 1) : NULL;
  if (last != NULL &&
      g_str_equal (json_object_get_string_member (last, "role"), "user"))
    {
      regenerate ();
    }
}

static void
on_new_chat_action (GSimpleAction *action G_GNUC_UNUSED,
                    GVariant      *param G_GNUC_UNUSED,
                    gpointer       data G_GNUC_UNUSED)
{
  on_new_chat ();
}

static void
on_focus_input_action (GSimpleAction *action G_GNUC_UNUSED,
                       GVariant      *param G_GNUC_UNUSED,
                       gpointer       data G_GNUC_UNUSED)
{
  if (adw_application_window_get_visible_dialog (
          ADW_APPLICATION_WINDOW (app->window)) == NULL)
    {
      gtk_widget_grab_focus (app->input);
    }
}

static void
on_search_action (GSimpleAction *action G_GNUC_UNUSED,
                  GVariant      *param G_GNUC_UNUSED,
                  gpointer       data G_GNUC_UNUSED)
{
  if (app->busy || adw_application_window_get_visible_dialog (
                       ADW_APPLICATION_WINDOW (app->window)) != NULL)
    {
      return;
    }
  adw_overlay_split_view_set_show_sidebar (ADW_OVERLAY_SPLIT_VIEW (app->split),
                                           TRUE);
  gtk_widget_grab_focus (app->search);
}

static void
on_settings_action (GSimpleAction *action G_GNUC_UNUSED,
                    GVariant      *param G_GNUC_UNUSED,
                    gpointer       data G_GNUC_UNUSED)
{
  if (adw_application_window_get_visible_dialog (
          ADW_APPLICATION_WINDOW (app->window)) == NULL)
    {
      on_settings_clicked (NULL, NULL);
    }
}

static void
on_stop_action (GSimpleAction *action G_GNUC_UNUSED,
                GVariant      *param G_GNUC_UNUSED,
                gpointer       data G_GNUC_UNUSED)
{
  if (app->busy && app->cancel != NULL)
    g_cancellable_cancel (app->cancel);
}

static void
register_actions (GtkApplication *gtk_app)
{
  const GActionEntry actions[] = {
    { .name = "new-chat", .activate = on_new_chat_action },
    { .name = "retry", .parameter_type = "s", .activate = on_retry_action },
    { .name = "focus-input", .activate = on_focus_input_action },
    { .name = "search", .activate = on_search_action },
    { .name = "settings", .activate = on_settings_action },
    { .name = "stop", .activate = on_stop_action },
  };
  g_action_map_add_action_entries (G_ACTION_MAP (app->window), actions,
                                   G_N_ELEMENTS (actions), NULL);
  const struct
  {
    const gchar *action, *key;
  } shortcuts[] = {
    { "win.new-chat", "n" },  { "win.focus-input", "l" },
    { "win.search", "f" },    { "win.settings", "comma" },
    { "win.stop", "period" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (shortcuts); i++)
    {
      gchar *primary = g_strconcat ("<Primary>", shortcuts[i].key, NULL);
      gchar *control = g_strconcat ("<Control>", shortcuts[i].key, NULL);
      const gchar *accels[] = { primary, control, NULL };
      gtk_application_set_accels_for_action (gtk_app, shortcuts[i].action,
                                             accels);
      g_free (primary);
      g_free (control);
    }
}

static void
activate (GtkApplication *gtk_app,
          gpointer        data G_GNUC_UNUSED)
{
  if (app->window != NULL && !app->closing)
    {
      gtk_window_present (GTK_WINDOW (app->window));
      return;
    }

  gchar *theme = config_get ("theme");
  apply_theme (theme);
  g_free (theme);

  g_clear_object (&app->window);
  app->closing = FALSE;
  app->window = adw_application_window_new (gtk_app);
  g_object_ref (app->window);
  gtk_window_set_default_size (GTK_WINDOW (app->window), 940, 800);
  g_signal_connect (app->window, "close-request", G_CALLBACK (on_close), NULL);

  app->toasts = adw_toast_overlay_new ();
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (app->window),
                                      app->toasts);

  app->split = adw_overlay_split_view_new ();
  adw_overlay_split_view_set_pin_sidebar (ADW_OVERLAY_SPLIT_VIEW (app->split),
                                          TRUE);
  adw_overlay_split_view_set_max_sidebar_width (
      ADW_OVERLAY_SPLIT_VIEW (app->split), 280);
  adw_overlay_split_view_set_sidebar (ADW_OVERLAY_SPLIT_VIEW (app->split),
                                      build_sidebar ());
  adw_overlay_split_view_set_content (ADW_OVERLAY_SPLIT_VIEW (app->split),
                                      build_content ());
  adw_overlay_split_view_set_show_sidebar (ADW_OVERLAY_SPLIT_VIEW (app->split),
                                           TRUE);
  adw_toast_overlay_set_child (ADW_TOAST_OVERLAY (app->toasts), app->split);

  register_actions (gtk_app);

  load_css ();
  rebuild_sidebar ();
  render_chat ();
  update_send_state ();
  gtk_window_present (GTK_WINDOW (app->window));
  fit_input_height ();
  gtk_widget_grab_focus (app->input);
  focus_source = g_idle_add (grab_input_focus, NULL);

  if (app->api_key == NULL)
    prompt_api_key ();
}

int
main (int     argc,
      gchar **argv)
{
  gchar *language = config_get ("language");
  i18n_init (language);
  g_free (language);

  gchar *current_id = NULL, *model = NULL, *effort = NULL;
  gchar *path = store_session_path ();
  GError *error = NULL;
  gint lock = store_lock (path, &error);
  if (lock < 0)
    {
      g_printerr ("%s\n", error->message);
      g_error_free (error);
      g_free (path);
      return 1;
    }
  GPtrArray *chats = store_load (path, &current_id, &model, &effort, &error);
  g_free (path);
  if (chats == NULL)
    {
      g_printerr (
          _ ("Unable to load conversations; original files preserved: %s\n"),
          error->message);
      g_error_free (error);
      close (lock);
      return 1;
    }

  app = g_new0 (App, 1);
  app->answer = g_string_new (NULL);
  app->reasoning = g_string_new (NULL);
  app->chats = chats;
  app->model = model;
  app->effort = effort;

  /* The configuration accepts only the supported options; an unknown value
     falls back to the default. */
  if (find_choice (MODELS, G_N_ELEMENTS (MODELS), app->model) == NULL)
    {
      g_free (app->model);
      app->model = g_strdup (MODELS[0].value);
    }
  if (find_choice (EFFORTS, G_N_ELEMENTS (EFFORTS), app->effort) == NULL)
    {
      g_free (app->effort);
      app->effort = g_strdup ("high");
    }

  for (guint i = 0; i < app->chats->len; i++)
    {
      Chat *chat = g_ptr_array_index (app->chats, i);
      if (g_str_equal (chat->id, current_id))
        {
          app->chat = chat;
          break;
        }
    }
  if (app->chat == NULL)
    {
      g_assert_cmpuint (app->chats->len, ==, 0);
      app->chat = chat_new_empty ();
      g_ptr_array_add (app->chats, app->chat);
    }
  g_free (current_id);

  const gchar *env_key = g_getenv ("DEEPSEEK_API_KEY");
  app->api_key = (env_key != NULL && *env_key)
                     ? g_strdup (env_key)
                     : config_get ("api_key");
  app->session = soup_session_new_with_options ("user-agent",
                                                "gchat/" GCHAT_VERSION, NULL);

  gtk_disable_setlocale ();
  AdwApplication *gtk_app =
      adw_application_new ("cc.arthur63.gchat", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (gtk_app, "activate", G_CALLBACK (activate), NULL);
  g_signal_connect (gtk_app, "shutdown", G_CALLBACK (on_shutdown), NULL);

  int status = g_application_run (G_APPLICATION (gtk_app), argc, argv);

  if (app->window != NULL)
    {
      if (!app->closing)
        status = 1;
      gtk_window_destroy (GTK_WINDOW (app->window));
      g_clear_object (&app->window);
    }
  g_object_unref (gtk_app);
  g_object_unref (app->session);
  g_ptr_array_unref (app->chats);
  g_string_free (app->answer, TRUE);
  g_string_free (app->reasoning, TRUE);
  g_free (app->model);
  g_free (app->effort);
  g_free (app->api_key);
  g_free (app);
  app = NULL;
  close (lock);
  return status;
}
