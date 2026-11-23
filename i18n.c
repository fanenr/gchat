/* SPDX-FileCopyrightText: 2026 Arthur <arthur63@163.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "i18n.h"

#ifdef __APPLE__
#include "macos-bundle.h"
#endif

#include <locale.h>
#include <string.h>

static gboolean
language_prefix (const gchar *language,
                 const gchar *prefix)
{
  gsize len = strlen (prefix);
  return g_ascii_strncasecmp (language, prefix, len) == 0 &&
         (language[len] == '\0' || strchr ("_.-@", language[len]) != NULL);
}

const gchar *
i18n_resolve (const gchar        *preference,
              const gchar *const *languages)
{
  if (g_strcmp0 (preference, "en") == 0 || g_strcmp0 (preference, "zh_CN") == 0)
    return preference;
  for (guint i = 0; languages != NULL && languages[i] != NULL; i++)
    {
      const gchar *language = languages[i];
      if (language_prefix (language, "en"))
        return "en";
      if (language_prefix (language, "zh"))
        {
          /* Traditional Chinese skips the generic zh fallback that GLib
             adds later. */
          if (language_prefix (language, "zh_CN") ||
              language_prefix (language, "zh-CN") ||
              language_prefix (language, "zh_SG") ||
              language_prefix (language, "zh-SG") ||
              language_prefix (language, "zh_Hans") ||
              language_prefix (language, "zh-Hans") || language[2] == '\0' ||
              language[2] == '.' || language[2] == '@')
            {
              return "zh_CN";
            }
          return "en";
        }
    }
  return "en";
}

void
i18n_init (const gchar *preference)
{
  const gchar *language = i18n_resolve (preference, g_get_language_names ());
  setlocale (LC_ALL, "");
  /* gettext also reads the locale environment; C/POSIX must not override a
     manually chosen language. */
  const gchar *locale = setlocale (LC_MESSAGES, NULL);
  if (g_strcmp0 (locale, "C") == 0 || g_strcmp0 (locale, "POSIX") == 0 ||
      (locale != NULL && g_str_has_prefix (locale, "C.")))
    {
      g_unsetenv ("LC_ALL");
      locale = setlocale (LC_MESSAGES, "en_US.UTF-8");
      if (locale == NULL)
        locale = setlocale (LC_MESSAGES, "C.UTF-8");
      if (locale != NULL)
        g_setenv ("LC_MESSAGES", locale, TRUE);
    }
  g_setenv ("LANGUAGE", language, TRUE);
  const gchar *locale_dir = LOCALEDIR;
#ifdef __APPLE__
  g_autofree gchar *resources = macos_bundle_dup_resources_dir ();
  g_autofree gchar *bundle_locale_dir = NULL;
  if (resources != NULL)
    {
      bundle_locale_dir = g_build_filename (resources, "locale", NULL);
      locale_dir = bundle_locale_dir;
    }
#endif
  bindtextdomain (GETTEXT_PACKAGE, locale_dir);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);
}
