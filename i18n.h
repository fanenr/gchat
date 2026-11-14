/* SPDX-FileCopyrightText: 2026 Arthur <arthur63@163.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "config.h"
#include <glib/gi18n.h>

/* Return a supported gettext language code; an unknown choice resolves from the
   system language. */
const gchar *i18n_resolve (const gchar        *preference,
                           const gchar *const *languages);

/* Call at startup, before GTK init; the gettext language does not change at run
   time. */
void         i18n_init    (const gchar *preference);
