/* SPDX-FileCopyrightText: 2026 Arthur <arthur63@163.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "i18n.h"

#include <locale.h>

typedef struct
{
  const gchar *name, *preference;
  const gchar *languages[4];
  const gchar *expected;
} ResolveCase;

static void
resolve_language (gconstpointer data)
{
  const ResolveCase *test = data;
  g_assert_cmpstr (i18n_resolve (test->preference, test->languages), ==,
                   test->expected);
}

typedef struct
{
  const gchar *name, *preference, *system;
  gboolean     chinese;
} TranslationCase;

static void
translation (gconstpointer data)
{
  const TranslationCase *test = data;
  if (!g_test_subprocess ())
    {
      g_test_trap_subprocess (NULL, 5000000, G_TEST_SUBPROCESS_DEFAULT);
      g_test_trap_assert_passed ();
      return;
    }
  g_setenv ("LC_ALL", "C", TRUE);
  g_setenv ("LANGUAGE", test->system, TRUE);
  i18n_init (test->preference);
  g_assert_cmpstr (setlocale (LC_MESSAGES, NULL), !=, "C");
  g_assert_cmpstr (_ ("New chat"), ==, test->chinese ? "新对话" : "New chat");
  g_assert_cmpstr (_ ("Send"), ==, test->chinese ? "发送" : "Send");
  g_assert_cmpstr (_ ("Copy code"), ==,
                   test->chinese ? "复制代码" : "Copy code");
  g_assert_cmpstr (_ ("Language"), ==, test->chinese ? "语言" : "Language");
  g_autofree gchar *title = g_strdup_printf (_ ("Thought (%.1f s)"), 1.5);
  g_assert_cmpstr (title, ==,
                   test->chinese ? "已思考（用时 1.5 秒）" : "Thought (1.5 s)");
  g_autofree gchar *error = g_strdup_printf (
      _ ("Unsupported conversation database version %d "
         "(maximum supported: %d)"),
      3, 1);
  g_assert_cmpstr (error, ==,
                   test->chinese
                       ? "不支持会话数据库版本 3（最高支持版本：1）"
                       : "Unsupported conversation database version 3 "
                         "(maximum supported: 1)");
}

int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);
  static const ResolveCase cases[] = {
    { "manual-english", "en", { "zh_CN", NULL }, "en" },
    { "manual-chinese", "zh_CN", { "en_US", NULL }, "zh_CN" },
    { "default-english", NULL, { "en_US.UTF-8", "en", NULL }, "en" },
    { "system-chinese", "system", { "zh_CN.UTF-8", "zh", NULL }, "zh_CN" },
    { "singapore", "system", { "zh_SG", "zh", NULL }, "zh_CN" },
    { "hans", "system", { "zh-Hans-CN", "zh", NULL }, "zh_CN" },
    { "hans-underscore", "system", { "zh_Hans", "zh", NULL }, "zh_CN" },
    { "generic-chinese", NULL, { "zh.UTF-8", NULL }, "zh_CN" },
    { "traditional", "system", { "zh_TW", "zh", NULL }, "en" },
    { "hant", "system", { "zh-Hant-HK", "zh", NULL }, "en" },
    { "hong-kong", "system", { "zh_HK", "zh", NULL }, "en" },
    { "unsupported-system", "system", { "fr_FR", "fr", "C", NULL }, "en" },
    { "invalid-preference", "bogus", { "zh_CN", NULL }, "zh_CN" },
    { "invalid-prefix", NULL, { "english", "zhuang", NULL }, "en" },
    { "no-languages", NULL, { NULL }, "en" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree gchar *name =
          g_strdup_printf ("/i18n/resolve/%s", cases[i].name);
      g_test_add_data_func (name, &cases[i], resolve_language);
    }
  static const TranslationCase translations[] = {
    { "manual-english", "en", "zh_CN", FALSE },
    { "manual-chinese", "zh_CN", "en_US", TRUE },
    { "system-chinese", "system", "zh_CN", TRUE },
    { "unsupported-system", "system", "fr_FR", FALSE },
    { "traditional", "system", "zh_TW", FALSE },
  };
  for (guint i = 0; i < G_N_ELEMENTS (translations); i++)
    {
      g_autofree gchar *name =
          g_strdup_printf ("/i18n/translation/%s", translations[i].name);
      g_test_add_data_func (name, &translations[i], translation);
    }
  return g_test_run ();
}
