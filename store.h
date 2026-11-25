/* SPDX-FileCopyrightText: 2026 Arthur <arthur63@163.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <json-glib/json-glib.h>
#include <sqlite3.h>

typedef struct
{
  gchar     *id;
  gchar     *title;
  gchar     *draft, *edit_text;
  JsonArray *draft_images, *edit_images;

  guint      edit_from; /* Use G_MAXUINT for a normal draft; otherwise edit
                           the indexed user message. */
  gboolean   pinned;
  gboolean   dirty;     /* Set after changes; clear only after commit. */
  gint64     created;   /* Store Unix milliseconds for sidebar grouping. */
  JsonArray *messages;  /* Store message objects with role and content
                           fields. */

  gchar     *system_prompt; /* Use NULL to inherit or "" to omit. */
  gchar     *summary;
  guint      summary_until; /* Count original messages covered by summary. */
  guint      summary_position; /* Count messages before the marker. */

  /* Keep the last measured and estimated input counts in memory only. */
  gchar     *usage_model;
  gint64     prompt_tokens;
  gint64     estimated_tokens;
} Chat;

Chat      *chat_new             (const gchar *id,
                                 const gchar *title);
Chat      *chat_new_empty       (void);
void       chat_free            (Chat        *chat);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Chat, chat_free)

/* Return the borrowed array for a validated attachment field, or NULL. */
JsonArray *image_array_member   (JsonObject  *object,
                                 const gchar *name);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (sqlite3, sqlite3_close)

/* Return an owned connection. Hold the process lock until it is closed. */
sqlite3   *store_open           (const gchar *path,
                                 GError     **error);

/* Return an owned array and owned output strings. An empty store has no
   current ID. Return NULL if the database cannot be read or is invalid. */
GPtrArray *store_load           (sqlite3  *db,
                                 gchar   **current_id,
                                 gchar   **model,
                                 gchar   **effort,
                                 GError  **error);

/* On commit, clear dirty flags and empty the owned-string deletion queue. */
gboolean   store_save           (sqlite3     *db,
                                 GPtrArray   *chats,
                                 GPtrArray   *deleted_ids,
                                 const gchar *current_id,
                                 const gchar *model,
                                 const gchar *effort,
                                 GError     **error);

/* Return newly allocated paths; the caller frees them. */
gchar     *store_session_path   (void);
gchar     *store_config_path    (void);
gchar     *store_image_dir      (const gchar *path);

/* Remove unreferenced images after a complete, successful load. */
void       store_cleanup_images (const gchar *path,
                                 GPtrArray   *chats);

/* Return the locked fd, or -1 on failure. Close it before the process exits;
   do not delete the .lock file. */
gint       store_lock           (const gchar *path,
                                 GError     **error);

/* Return an owned string, or NULL when the value is missing or empty. */
gchar     *config_get           (const gchar *name);

/* Merge string key/value pairs into config.json atomically without a backup;
   refuse to overwrite an invalid configuration. */
gboolean   config_set           (const gchar *name,
                                 const gchar *value);
