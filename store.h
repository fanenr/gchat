/* SPDX-FileCopyrightText: 2026 Arthur <arthur63@163.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <json-glib/json-glib.h>

typedef struct
{
  gchar     *id;
  gchar     *title;
  gchar     *draft, *edit_text;

  guint      edit_from; /* Use G_MAXUINT for a normal draft; otherwise edit
                           the indexed user message. */
  gboolean   pinned;
  gint64     created;   /* Store Unix milliseconds for sidebar grouping. */
  JsonArray *messages;  /* Store message objects with role and content
                           fields. */
} Chat;

Chat *     chat_new           (const gchar *id,
                               const gchar *title);
Chat *     chat_new_empty     (void);
void       chat_free          (Chat *chat);

/* Return an empty list when there is no file. A file must match the current
   session structure; a missing required field is not filled in. Return NULL
   when neither the main file nor the backup is usable, and keep the original
   data. */
GPtrArray *store_load         (const gchar *path,
                               gchar      **current_id,
                               gchar      **model,
                               gchar      **effort,
                               GError     **error);
gboolean   store_save         (const gchar *path,
                               GPtrArray   *chats,
                               const gchar *current_id,
                               const gchar *model,
                               const gchar *effort);
gchar *    store_session_path (void);
gchar *    store_config_path  (void);

/* Return the locked fd, or -1 on failure. Close it before the process exits;
   do not delete the .lock file. */
gint       store_lock         (const gchar *path,
                               GError     **error);

/* Merge string key/value pairs into config.json atomically without a backup;
   refuse to overwrite an invalid configuration. */
gchar *    config_get         (const gchar *name);
gboolean   config_set         (const gchar *name,
                               const gchar *value);
