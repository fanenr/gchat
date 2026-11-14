/* SPDX-FileCopyrightText: 2026 Arthur <arthur63@163.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Include the implementation to check the buffer lifetime without adding a test
   interface to production code. */
#include "config.h"

#include "../md.c"

void
app_toast (const gchar *msg G_GNUC_UNUSED)
{
}

typedef struct
{
  const gchar *markdown, *text, *tag;
  gboolean     active;
} TagCase;

static void
clear_box (GtkWidget *box)
{
  while (g_main_context_iteration (NULL, FALSE))
    {
    }
  while (gtk_widget_get_first_child (box) != NULL)
    gtk_box_remove (GTK_BOX (box), gtk_widget_get_first_child (box));
  while (g_main_context_iteration (NULL, FALSE))
    {
    }
}

static void
text_tags (gconstpointer data)
{
  const TagCase *test = data;
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  g_object_ref_sink (box);
  md_render (GTK_BOX (box), test->markdown);
  gboolean found = FALSE;
  for (GtkWidget *view = gtk_widget_get_first_child (box); view != NULL;
       view = gtk_widget_get_next_sibling (view))
    {
      if (!GTK_IS_TEXT_VIEW (view))
        continue;
      GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
      GtkTextIter start, end;
      gtk_text_buffer_get_bounds (buffer, &start, &end);
      gchar *text = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
      const gchar *at = strstr (text, test->text);
      if (at != NULL)
        {
          GtkTextTag *tag = gtk_text_tag_table_lookup (
              gtk_text_buffer_get_tag_table (buffer), test->tag);
          g_assert_nonnull (tag);
          gtk_text_buffer_get_iter_at_offset (
              buffer, &start, g_utf8_pointer_to_offset (text, at));
          g_assert_cmpint (gtk_text_iter_has_tag (&start, tag), ==,
                           test->active);
          found = TRUE;
        }
      g_free (text);
    }
  g_assert_true (found);
  clear_box (box);
  g_object_unref (box);
  g_assert_cmpuint (code_buffers != NULL ? code_buffers->len : 0, ==, 0);
}

static void
code_buffers_released (void)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  g_object_ref_sink (box);
  for (guint i = 0; i < 10; i++)
    {
      md_render (GTK_BOX (box), "```c\nint x;\n```\n");
      g_assert_cmpuint (code_buffers->len, ==, 1);
      clear_box (box);
      g_assert_cmpuint (code_buffers->len, ==, 0);
    }
  g_object_unref (box);
}

typedef struct
{
  const gchar *markdown, *expected;
} TextCase;

static void
list_paragraphs (gconstpointer data)
{
  const TextCase *test = data;
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  g_object_ref_sink (box);
  md_render (GTK_BOX (box), test->markdown);
  GtkWidget *view = gtk_widget_get_first_child (box);
  g_assert_true (GTK_IS_TEXT_VIEW (view));
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds (buffer, &start, &end);
  gchar *text = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
  g_assert_cmpstr (text, ==, test->expected);
  g_free (text);
  clear_box (box);
  g_object_unref (box);
}

typedef struct
{
  const gchar *markdown, *header, *body, *body_markup;
} TableCase;

static void
table_cells (gconstpointer data)
{
  const TableCase *test = data;
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  g_object_ref_sink (box);
  md_render (GTK_BOX (box), test->markdown);
  GtkWidget *card = gtk_widget_get_first_child (box);
  g_assert_true (GTK_IS_BOX (card));
  GtkWidget *grid = gtk_widget_get_first_child (card);
  g_assert_true (GTK_IS_GRID (grid));
  GtkWidget *header = gtk_grid_get_child_at (GTK_GRID (grid), 0, 0);
  GtkWidget *body = gtk_grid_get_child_at (GTK_GRID (grid), 0, 2);
  g_assert_true (GTK_IS_LABEL (header));
  g_assert_true (GTK_IS_LABEL (body));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (header)), ==, test->header);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (body)), ==, test->body);
  g_assert_nonnull (strstr (gtk_label_get_label (GTK_LABEL (header)), "<b>"));
  if (test->body_markup != NULL)
    g_assert_cmpstr (gtk_label_get_label (GTK_LABEL (body)), ==,
                     test->body_markup);
  clear_box (box);
  g_object_unref (box);
}

static void
link_targets (gconstpointer data)
{
  const TextCase *test = data;
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  g_object_ref_sink (box);
  md_render (GTK_BOX (box), test->markdown);
  GtkWidget *view = gtk_widget_get_first_child (box);
  g_assert_true (GTK_IS_TEXT_VIEW (view));
  GtkTextIter start;
  gtk_text_buffer_get_start_iter (
      gtk_text_view_get_buffer (GTK_TEXT_VIEW (view)), &start);
  GSList *tags = gtk_text_iter_get_tags (&start);
  const gchar *href = NULL;
  for (GSList *it = tags; it != NULL; it = it->next)
    if (g_object_get_data (G_OBJECT (it->data), "href") != NULL)
      href = g_object_get_data (G_OBJECT (it->data), "href");
  g_assert_cmpstr (href, ==, test->expected);
  g_slist_free (tags);
  clear_box (box);
  g_object_unref (box);
}

typedef struct
{
  const gchar *markdown, *content, *display;
} CodeBlockCase;

static void
code_block_content (gconstpointer data)
{
  const CodeBlockCase *test = data;
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  g_object_ref_sink (box);
  md_render (GTK_BOX (box), test->markdown);
  GtkWidget *card = gtk_widget_get_first_child (box);
  g_assert_true (gtk_widget_has_css_class (card, "code-card"));
  GtkWidget *copy =
      gtk_widget_get_last_child (gtk_widget_get_first_child (card));
  g_assert_cmpstr (g_object_get_data (G_OBJECT (copy), "code"), ==,
                   test->content);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (
      GTK_TEXT_VIEW (gtk_widget_get_last_child (card)));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds (buffer, &start, &end);
  gchar *text = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
  g_assert_cmpstr (text, ==, test->display);
  g_free (text);
  clear_box (box);
  g_object_unref (box);
}

static void
language_aliases (void)
{
  static const gchar *const names[][2] = {
    { "js", "js" },      { "javascript", "js" },   { "ts", "typescript" },
    { "py", "python" },  { "python3", "python" },  { "csharp", "c-sharp" },
    { "c#", "c-sharp" }, { "c-sharp", "c-sharp" },
  };
  GtkSourceLanguageManager *manager =
      gtk_source_language_manager_get_default ();
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    {
      GtkSourceLanguage *expected =
          gtk_source_language_manager_get_language (manager, names[i][1]);
      g_assert_nonnull (expected);
      g_assert_true (lookup_language (names[i][0]) == expected);
    }
  g_assert_null (lookup_language (NULL));
  g_assert_null (lookup_language (""));
  g_assert_null (
      lookup_language ("gchat-unknown-language-that-does-not-exist"));
}

int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);
  i18n_init (NULL);
  gtk_disable_setlocale ();
  if (!gtk_init_check ())
    {
      g_test_message ("需要 GTK 显示后端");
      return 77;
    }
  adw_init ();
  static const TagCase cases[] = {
    { "- before\n\n  ```c\n  int x;\n  ```\n\n  after\n", "after", "list1",
      TRUE },
    { "- before\n\n  ```c\n  int x;\n  ```\n", "before", "list1", TRUE },
    { "- before\n\n  ---\n\n  after\n", "after", "list1", TRUE },
    { "- before\n\n  | head |\n  | --- |\n  | body |\n\n  after\n", "after",
      "list1", TRUE },
    { "- outer\n  - inner\n\n    ```c\n    int x;\n    ```\n\n    after\n",
      "after", "list2", TRUE },
    { "> before\n>\n> ```c\n> int x;\n> ```\n>\n> after\n", "after", "quote1",
      TRUE },
    { "- before\n\n  ```c\n  int x;\n  ```\n\n  after\n\n  ```c\n  int y;\n  "
      "```\n\n  last\n",
      "last", "list1", TRUE },
    { "**bold** plain", "bold", "bold", TRUE },
    { "**bold** plain", "plain", "bold", FALSE },
    { "*italic* plain", "italic", "italic", TRUE },
    { "~~deleted~~ plain", "deleted", "strike", TRUE },
    { "`code` plain", "code", "code", TRUE },
    { "**outer *inner* tail** plain", "tail", "bold", TRUE },
    { "**outer *inner* tail** plain", "tail", "italic", FALSE },
  };
  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      gchar *name = g_strdup_printf ("/markdown/text-tag/%u", i);
      g_test_add_data_func (name, &cases[i], text_tags);
      g_free (name);
    }
  static const TextCase lists[] = {
    { "1. **问答与解释**\n   解释概念，分析问题。\n\n2. **写作与润色**\n   "
      "写文章，修改表达。\n",
      "1. 问答与解释\n解释概念，分析问题。\n2. "
      "写作与润色\n写文章，修改表达。" },
    { "- first\n\n- second\n", "• first\n• second" },
    { "- [x] done\n\n- [ ] pending\n", "☑ done\n☐ pending" },
    { "1. first\n2. second\n", "1. first\n2. second" },
    { "1. first\n\n   another paragraph\n\n2. second\n",
      "1. first\nanother paragraph\n2. second" },
    { "- first\n- second\n\noutside\n", "• first\n• second\noutside" },
    { "- outer\n\n  1. inner\n  2. next\n\n  after inner\n\n- last\n",
      "• outer\n1. inner\n2. next\nafter inner\n• last" },
    { "&#0; &#xD800; &#x110000;", "� � �" },
    { "0. first\n1. second\n", "0. first\n1. second" },
    { "3. first\n\n4. second\n", "3. first\n4. second" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (lists); i++)
    {
      gchar *name = g_strdup_printf ("/markdown/list-paragraphs/%u", i);
      g_test_add_data_func (name, &lists[i], list_paragraphs);
      g_free (name);
    }
  static const TableCase tables[] = {
    { "| header |\n| --- |\n| **bold *both* tail** ~~old~~ `code` |\n",
      "header", "bold both tail old code",
      "<b>bold <i>both</i> tail</b> <s>old</s> <tt>code</tt>" },
    { "| header |\n| --- |\n| [local](file:///tmp/example) |\n", "header",
      "local", "<span>local</span>" },
    { "| header |\n| --- |\n| [link](https://example.com/?a=1&amp;b=2) |\n",
      "header", "link",
      "<a href=\"https://example.com/?a=1&amp;b=2\">link</a>" },
    { "| **Name** *label* ~~old~~ `code` &amp; <b>literal</b> |\n| --- |\n| "
      "body & <i>literal</i> |\n",
      "Name label old code & <b>literal</b>", "body & <i>literal</i>", NULL },
    { "| header |\n| --- |\n| [link](<https://example.com/\"quoted\"?x=1&y=2>) "
      "|\n",
      "header", "link",
      "<a "
      "href=\"https://example.com/&quot;quoted&quot;?x=1&amp;y=2\">link</a>" },
    { "| [标题](<https://example.com/\"中文\">) |\n| --- |\n| "
      "[链接](<https://example.com/\"中文\">) |\n",
      "标题", "链接",
      "<a href=\"https://example.com/&quot;中文&quot;\">链接</a>" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (tables); i++)
    {
      gchar *name = g_strdup_printf ("/markdown/table/%u", i);
      g_test_add_data_func (name, &tables[i], table_cells);
      g_free (name);
    }
  static const TextCase links[] = {
    { "[web](https://example.com/?a=1&amp;b=2)",
      "https://example.com/?a=1&b=2" },
    { "[web](HTTP://example.com)", "HTTP://example.com" },
    { "[mail](mailto:user@example.com)", "mailto:user@example.com" },
    { "[local](file:///tmp/example)", NULL },
    { "[app](custom-app://open)", NULL },
    { "[script](javascript:alert)", NULL },
    { "[empty]()", NULL },
    { "[relative](/path)", NULL },
  };
  for (guint i = 0; i < G_N_ELEMENTS (links); i++)
    {
      gchar *name = g_strdup_printf ("/markdown/link/%u", i);
      g_test_add_data_func (name, &links[i], link_targets);
      g_free (name);
    }
  static const CodeBlockCase code_blocks[] = {
    { "```text\nline\n```\n", "line\n", "line" },
    { "```text\nline  \n\n```\n", "line  \n\n", "line  \n" },
    { "```text\nfirst\n\nlast  \n```\n", "first\n\nlast  \n",
      "first\n\nlast  " },
    { "```text\nline  ", "line  \n", "line  " },
    { "```text\r\nline\r\n```\r\n", "line\n", "line" },
    { "```text\n```\n", "", "" },
    { "```text\n\n```\n", "\n", "" },
    { "```text\n\n\n```\n", "\n\n", "\n" },
    { "    line  \r\n", "line  \n", "line  " },
    { "<div>\r\ntext\r\n</div>\r\n", "<div>\ntext\n</div>\n",
      "<div>\ntext\n</div>" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (code_blocks); i++)
    {
      gchar *name = g_strdup_printf ("/markdown/code-block/content/%u", i);
      g_test_add_data_func (name, &code_blocks[i], code_block_content);
      g_free (name);
    }
  g_test_add_func ("/markdown/language-aliases", language_aliases);
  g_test_add_func ("/markdown/code-buffers-released", code_buffers_released);
  return g_test_run ();
}
