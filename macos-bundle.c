/* SPDX-FileCopyrightText: 2026 Arthur <arthur63@163.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "macos-bundle.h"

#include <CoreFoundation/CoreFoundation.h>
#include <gio/gio.h>
#include <libintl.h>
#include <limits.h>

gchar *
macos_bundle_dup_resources_dir (void)
{
  CFBundleRef bundle = CFBundleGetMainBundle ();
  if (bundle == NULL)
    return NULL;

  CFStringRef id = CFBundleGetIdentifier (bundle);
  if (id == NULL || !CFEqual (id, CFSTR (GCHAT_APPLICATION_ID)))
    return NULL;

  CFURLRef url = CFBundleCopyResourcesDirectoryURL (bundle);
  if (url == NULL)
    return NULL;

  gchar path[PATH_MAX];
  gboolean resolved = CFURLGetFileSystemRepresentation (url, TRUE,
                                                        (UInt8 *) path,
                                                        sizeof path);
  CFRelease (url);
  return resolved ? g_strdup (path) : NULL;
}

gboolean
macos_bundle_init (GError **error)
{
  g_autofree gchar *resources = macos_bundle_dup_resources_dir ();
  if (resources == NULL)
    return TRUE;

  g_autofree gchar *data = g_build_filename (resources, "share", NULL);
  if (!g_file_test (data, G_FILE_TEST_IS_DIR))
    return TRUE;

  g_autofree gchar *gio_modules = g_build_filename (resources, "..",
                                                    "Frameworks", "gio",
                                                    "modules", NULL);
  g_autofree gchar *loaders = g_build_filename (resources, "loaders.cache",
                                                NULL);
  g_autofree gchar *fonts = g_build_filename (resources, "fonts.conf", NULL);
  g_setenv ("XDG_DATA_DIRS", data, TRUE);
  g_setenv ("GIO_MODULE_DIR", gio_modules, TRUE);
  g_setenv ("GDK_PIXBUF_MODULE_FILE", loaders, TRUE);
  g_setenv ("FONTCONFIG_FILE", fonts, TRUE);

  g_autofree gchar *certs = g_build_filename (resources, "cacert.pem", NULL);
  g_autoptr (GTlsDatabase) database = g_tls_file_database_new (certs, error);
  if (database == NULL)
    return FALSE;

  g_tls_backend_set_default_database (g_tls_backend_get_default (), database);
  return TRUE;
}

void
macos_bundle_bind_domains (void)
{
  const gchar *domains[] = { "glib20", "gtk40", "libadwaita" };
  g_autofree gchar *resources = macos_bundle_dup_resources_dir ();
  if (resources == NULL)
    return;

  g_autofree gchar *locale = g_build_filename (resources, "share", "locale",
                                               NULL);
  if (!g_file_test (locale, G_FILE_TEST_IS_DIR))
    return;

  /* The libraries bind their built-in paths during startup. */
  for (guint i = 0; i < G_N_ELEMENTS (domains); i++)
    bindtextdomain (domains[i], locale);
}
