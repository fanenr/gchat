/* SPDX-FileCopyrightText: 2026 Arthur <arthur63@163.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

gchar   *macos_bundle_dup_resources_dir (void);
gboolean macos_bundle_init              (GError **error);
void     macos_bundle_bind_domains      (void);

G_END_DECLS
