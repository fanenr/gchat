/* SPDX-FileCopyrightText: 2026 Arthur <arthur63@163.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

/* Render Markdown into box: the body goes into one read-only text
   view, while code blocks and tables become separate widgets. */
void md_render        (GtkBox            *box,
                       const gchar *markdown);

/* Recalculate list margins after a font change without replacing widgets. */
void md_refresh_fonts (GtkBox *box);

/* The app implements this to report copy results and similar notices. */
void app_toast        (const gchar *msg);
