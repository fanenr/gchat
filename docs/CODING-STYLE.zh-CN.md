GTK 编码风格
-------------------------------------------------------------------------------

本文简要描述 GTK 源代码推荐使用的编码风格。它深受 Clutter 的
`CODING_STYLE` 文档启发。

编码风格关乎一致性、可读性和可维护性；它也完全是人为约定和个人偏好。
本文至少会通过示例，为常见的编码风格问题提供权威且一致的答案，并尽量
指出允许的例外情况。

示例会展示推荐的编码风格；反面示例会明确标出。请不要向 GTK 提交
类似反面示例的代码。

这些编码风格规则的部分理由可参见内核的 CodingStyle 文档或 Cairo 的
`CODING_STYLE` 文档。

有疑问时，请查看周围的代码并尽量模仿其风格。

### 行宽

源文件的行宽尽可能不超过 80 个字符。更长的行通常意味着你需要使用
函数或预处理宏。

### 缩进

每增加一级缩进，相比上一级多缩进 2 个或更多空格：

```c
  if (condition)
    single_statement ();
```

缩进只能使用空格，不能只用制表符，也不能混用空格和制表符。

不要通过修改编辑器配置来改变制表符的含义（见下文）；使用制表符缩进的
代码不会被 GTK 接受。

虽然每级缩进两个空格比八个空格允许更深的嵌套，但 GTK 倾向于使用能
自我说明的函数名，而这类名称可能相当长。因此应避免过深的代码嵌套。

### 制表符

制表符必须始终展开为空格。如果源代码中必须使用字面制表符，必须按其
传统含义解释：

```
	Advance to the next column which is a multiple of 8.
        [ these two lines should be aligned ]
```

示例中的制表符前进到下一个为 8 的倍数的列；这两行文字应当对齐。

### 花括号

单条语句的代码块不应使用花括号：

```c
  if (condition)
    single_statement ();
  else
    another_single_statement (arg1);
```

包含多条语句时，花括号应放在新的缩进级别上：

```c
  if (condition)
    {
      statement_1 ();
      statement_2 ();
      statement_3 ();
    }
```

“单条语句不使用代码块”规则只有四种例外：

  1. 单条语句跨越多行（例如参数很多的函数调用），且后面跟有
     `else` 或 `else if`：

```c
  /* valid */
  if (condition)
    {
      a_single_statement_with_many_arguments (some_lengthy_argument,
                                              another_lengthy_argument,
                                              and_another_one,
                                              plus_one);
    }

  /* invalid */
  if (condition)
    a_single_statement_with_many_arguments (some_lengthy_argument,
                                            another_lengthy_argument,
                                            and_another_one,
                                            plus_one);
```

  2. 条件表达式由多行组成：

```c
  /* valid */
  if (condition1 ||
      (condition2 && condition3) ||
      condition4 ||
      (condition5 && (condition6 || condition7)))
    {
      a_single_statement ();
    }

  /* invalid */
  if (condition1 ||
      (condition2 && condition3) ||
      condition4 ||
      (condition5 && (condition6 || condition7)))
    a_single_statement ();
```

  3. 嵌套的 `if` 语句，此时代码块应放在最外层 `if` 上：

```c
  /* valid */
  if (condition)
    {
      if (another_condition)
        single_statement ();
      else
        another_single_statement ();
    }

  /* invalid */
  if (condition)
    if (another_condition)
      single_statement ();
    else if (yet_another_condition)
      another_single_statement ();
```

  4. 如果 `if-else` 语句的一侧使用了花括号，另一侧也应使用花括号，
     以使缩进一致：

```c
  /* valid */
  if (condition)
    {
      foo ();
      bar ();
    }
  else
    {
      baz ();
    }

  /* invalid */
  if (condition)
    {
      foo ();
      bar ();
    }
  else
    baz ();
```

一般来说，新代码块应放在新的缩进级别上，例如：

```c
  int retval = 0;

  statement_1 ();
  statement_2 ();

  {
    int var1 = 42;
    gboolean res = FALSE;

    res = statement_3 (var1);

    retval = res ? -1 : 1;
  }
```

函数定义的花括号应另起一行，但不应增加缩进级别：

```c
  /* valid */
  static void
  my_function (int argument)
  {
    do_my_things ();
  }

  /* invalid */
  static void
  my_function (int argument) {
    do_my_things ();
  }

  /* invalid */
  static void
  my_function (int argument)
    {
      do_my_things ();
    }
```

花括号不得与条件放在同一行：

```c
  /* invalid */
  if (condition) {
    statement_1 ();
    statement_2 ();
  }
```

### 条件

不要使用相等比较来检查布尔值：

```c
  /* invalid */
  if (condition == TRUE)
    do_foo ();

  /* valid */
  if (another_condition)
    do_bar ();
```

即使 C 将与 NULL 的相等比较按布尔值处理，也要显式比较：

```c
  /* valid */
  if (some_pointer == NULL)
    do_blah ();

  /* invalid */
  if (some_other_pointer)
    do_blurp ();
```

条件表达式分成多行时，逻辑运算符应始终放在行尾：

```c
  /* invalid */
  if (condition1
      || condition2
      || condition3)
    {
      do_foo ();
    }

  /* valid */
  if (condition1 &&
      condition2 &&
      (condition3 || (condition4 && condition5)))
    {
      do_blah ();
    }
```

### 函数

声明函数时，返回值类型应与函数名分行：

```c
  void
  my_function (void)
  {
  }
```

参数列表必须每个参数另起一行，参数名右对齐，并考虑指针符号的位置：

```c
  void
  my_function (some_type_t     type,
               another_type_t *a_pointer,
               final_type_t    another_type)
  {
  }
```

调用函数时也要遵循对齐规则，同时不超过 80 个字符的行宽限制：

```c
  align_function_arguments (first_argument,
                            second_argument,
                            third_argument);
```

为遵守 80 个字符的行宽限制，不要将函数名与参数分到不同行：

```c
  /* invalid */
  a_very_long_function_name_with_long_parameters
    (argument_the_first, argument_the_second);

  /* valid */
  first_a = argument_the_first;
  second_a = argument_the_second;
  a_very_long_function_name_with_long_parameters (first_a, second_a);
```

### 空白

左括号前始终加空格，左括号后则不加空格：

```c
  /* valid */
  if (condition)
    do_my_things ();

  /* valid */
  switch (condition)
    {
    }

  /* invalid */
  if(condition)
    do_my_things();

  /* invalid */
  if ( condition )
    do_my_things ( );
```

`switch()` 的代码块应从新的缩进级别开始，每个 `case` 与花括号处于
同一缩进级别，`case` 的代码块则另起一级缩进：

```c
  /* valid */
  switch (condition)
    {
    case FOO:
      do_foo ();
      break;

    case BAR:
      do_bar ();
      break;
    }

  /* invalid */
  switch (condition) {
    case FOO: do_foo (); break;
    case BAR: do_bar (); break;
  }

  /* invalid */
  switch (condition)
    {
    case FOO: do_foo ();
      break;
    case BAR: do_bar ();
      break;
    }

  /* invalid */
  switch (condition)
    {
      case FOO:
      do_foo ();
      break;
      case BAR:
      do_bar ();
      break;
    }
```

建议用空行分隔各个 `case`，但这不是强制要求：

```c
  switch (condition)
    {
    case FOO:
      do_foo ();
      break;

    case BAR:
      do_bar ();
      break;

    default:
      do_default ();
    }
```

`default:` 分支中的 `break` 语句不是强制要求。

如果 `case` 代码块需要声明新变量，应遵循内部代码块的相同规则
（见上文）；`break` 语句应放在内部代码块之外：

```c
  switch (condition)
    {
    case FOO:
      {
        int foo;

        foo = do_foo ();
      }
      break;

    ...
    }
```

声明结构体类型时，用空行分隔结构体中逻辑上不同的部分：

```c
  struct _GtkWrapBoxPrivate
  {
    GtkOrientation        orientation;
    GtkWrapAllocationMode mode;

    GtkWrapBoxSpreading   horizontal_spreading;
    GtkWrapBoxSpreading   vertical_spreading;

    guint16               vertical_spacing;
    guint16               horizontal_spacing;

    guint16               minimum_line_children;
    guint16               natural_line_children;

    GList                *children;
  };
```

不要仅仅因为代码能放进 80 个字符而去掉空白和换行：

```c
  /* invalid */
  if (condition) foo (); else bar ();
```

应清除所有行尾空白，最好为此单独创建补丁或提交。文件开头或结尾
不要留空行。

请启用默认的 Git pre-commit 钩子，以便检测行尾空白，避免将其带入
GTK 代码树。启用方式如下：

```
  chmod a+x .git/hooks/pre-commit
```

`git-stripspace` 工具也可能有用：它可作为过滤器，移除行尾空白以及
开头、结尾和重复的空行。

### 命名

请注意，大多数语言的标识符不能以数字开头，而语言绑定常常通过去掉
前缀来构造标识符。因此，名为 `GDK_2BUTTON_PRESS` 的枚举值会有问题。

### 头文件

GTK 的头文件比较特殊，不必遵守 80 个字符的行宽限制。头文件的主要
规则是函数定义应按三列纵向对齐：

```c
  return value          function_name           (type   argument,
                                                 type   argument,
                                                 type   argument);
```

每列的最大宽度由该列中最长的元素决定：

```c
  void        gtk_type_set_property (GtkType      *type,
                                     const char   *value,
                                     GError      **error);
  const char *gtk_type_get_property (GtkType      *type);
```

也可以将各列对齐到下一个制表位：

```c
  void          gtk_type_set_prop           (GtkType *type,
                                             float    value);
  float         gtk_type_get_prop           (GtkType *type);
  int           gtk_type_update_foobar      (GtkType *type);
```

绝不能直接包含公共头文件：

```c
  #if !defined (__GTK_H_INSIDE__) && !defined (GTK_COMPILATION)
  #error "Only <gtk/gtk.h> can be included directly."
  #endif
```

如果存在对应的公共头文件，私有头文件应首先包含它：

```c
  #pragma once

  /* gtkfooprivate.h */

  #include "gtkfoo.h"

  ...

  #endif /* __GTK_FOO_PRIVATE_H__ */
```

所有头文件都应使用 `once` pragma 防止重复包含，而不是使用传统的
预处理器保护宏：

```c
  #pragma once
```

此外，公共头文件的声明应使用 C++ 兼容性保护宏包围：

```c
  G_BEGIN_DECLS

  GDK_AVAILABLE_IN_ALL
  GType         gtk_foo_get_type        (void);

  GDK_AVAILABLE_IN_ALL
  GtkWidget *   gtk_foo_new             (void);

  ...

  G_END_DECLS
```

### 包含头文件

GTK 源文件绝不能包含全局的 gtk.h 头文件；应改为包含所需的各个头文件。

每个源文件都必须首先包含 config.h，然后包含与该源文件对应的头文件；
如果存在对应头文件，它可以是已安装的公共头文件，也可以是私有头文件。

```c
  /* gtkfoo.c */

  #include "config.h"

  #include "gtkfoo.h"
```

接下来，源文件应按字母顺序包含项目头文件：先包含当前目录中的头文件，
然后是子目录中的头文件，最后是相对于顶层目录的路径中的头文件：

```c
  #include "config.h"

  #include "gtkfooprivate.h"

  #include "gtkbutton.h"
  #include "gtkwidget.h"

  #include "a11y/gtkwidgetaccessible.h"

  #include "gdk/gdkwindowprivate.h"
```

最后，源文件应包含系统头文件：

```c
  #include "config.h"

  #include "gtkbarprivate.h"

  #include "gtkcontainerprivate.h"

  #include "a11y/gtkcontaineraccessible.h"
  #include "a11y/gtkwidgetaccessible.h"

  #include "gdk/gdkwindowprivate.h"

  #include <graphene.h>
  #include <string.h>
```

应尽可能避免循环依赖；例如，可以使用额外的头文件来打破循环。

### GObject

GObject 类的定义和实现还需要注意一些额外的编码风格。

`typedef` 声明应放在文件开头：

```c
  typedef struct _GtkFoo          GtkFoo;
  typedef struct _GtkFooClass     GtkFooClass;
```

这也包括枚举类型：

```c
  typedef enum
  {
    GTK_SIZE_REQUEST_WIDTH_FOR_HEIGHT,
    GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH
  } GtkSizeRequestMode;
```

以及回调类型：

```c
  typedef void (* GtkCallback) (GtkWidget *widget,
                                gpointer   user_data);
```

实例结构体应只包含父类型：

```c
  struct _GtkFoo
  {
    GtkWidget parent_instance;
  };
```

新编写的头文件应使用 `G_DECLARE_DERIVABLE_TYPE()` 和
`G_DECLARE_FINAL_TYPE()` 宏。还有一个 `GDK_DECLARE_INTERNAL_TYPE()`
宏，用于声明可以在 GTK 内部派生、但不能在第三方代码中派生的类型。

在源文件中，始终使用 `G_DEFINE_TYPE()`、`G_DEFINE_TYPE_WITH_PRIVATE()`
和 `G_DEFINE_TYPE_WITH_CODE()` 宏，或其抽象类型变体
`G_DEFINE_ABSTRACT_TYPE()`、`G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE()`
和 `G_DEFINE_ABSTRACT_TYPE_WITH_CODE()`；定义接口、quark 和装箱类型时
也应使用类似的宏。

所有属性都应存储在私有数据结构中。该结构定义在源文件中，必要时也可以
定义在私有头文件中；私有头文件的文件名必须以 `private.h` 结尾，且不得
安装。

只能在内部访问私有数据结构：对于旧代码，可使用实例结构体中的指针；
否则使用为该类型生成的实例私有数据 getter 函数。绝不能使用
`G_TYPE_INSTANCE_GET_PRIVATE()` 宏或 `g_type_instance_get_private()`
函数。

接口类型应始终包含用于类型转换的占位 `typedef`：

```c
        typedef struct _GtkFoo                  GtkFoo;
```

接口结构体的名称应在占位 `typedef` 名称后加上 `Interface`：

```c
        typedef struct _GtkFooInterface         GtkFooInterface;
```

接口必须具备以下宏：

| 宏 | 展开为 |
|------------------------------|---------------------------------|
| `GTK_TYPE_<iface_name>`      | `<iface_name>_get_type`         |
| `GTK_<iface_name>`           | `G_TYPE_CHECK_INSTANCE_CAST`    |
| `GTK_IS_<iface_name>`        | `G_TYPE_CHECK_INSTANCE_TYPE`    |
| `GTK_<iface_name>_GET_IFACE` | `G_TYPE_INSTANCE_GET_INTERFACE` |

`G_DECLARE` 宏会将它们定义为静态内联函数。

### 内存分配

在堆上动态分配数据时使用 `g_new()`。

公共结构体类型在返回前必须清零，可以显式清零每个成员，也可以使用
`g_new0()`。新代码中不要使用 `g_slice`。

只在函数作用域内需要的内存可以使用 `g_newa()` 或 `g_alloca()` 在栈上
分配。但应限制以这种方式消耗的栈内存，特别是在递归函数中。

在 Windows 上，可用的默认栈大小为 1M。

### 宏

除非确有必要，否则尽量避免使用私有宏。记得在需要这些宏的代码块或一组
函数末尾使用 `#undef` 取消定义。

通常优先使用内联函数，而非私有宏。

公共宏只能用于求值结果为常量的情况。

### 符号可见性

任何未使用 `GDK_AVAILABLE_IN_*` 或 `GDK_DEPRECATED_IN_*` 宏显式标注的
符号都被视为内部符号，不会从共享库中导出。

绝不要将变量作为公共 API 导出，因为这在某些平台上使用不便。应始终
优先提供 getter 和 setter。

多个源文件需要使用的非导出函数，应在名称以 `private.h` 结尾的私有
头文件中声明。

仅供单个源文件使用的非导出函数应声明为 `static`。

### 文档

所有公共 API 都必须有文档注释。函数的文档注释应放在源文件中，紧邻
函数上方。

```c
  /* valid */
  /**
   * gtk_get_flow:
   * @widget: a `GtkWidget`
   *
   * Gets the flow of a widget.
   *
   * Note that flows may be laminar or turbulent...
   *
   * Returns: (transfer none): the flow of @widget
   */
  GtkFlow *
  gtk_get_flow (GtkWidget *widget)
  {

    ...

  }
```

宏、函数类型、类结构体等的文档注释应放在定义旁边，通常是在头文件中。

文档注释的主要内容使用 Markdown，可以包含行内格式、章节、表格、图片和
链接。链接到符号、类等时，使用如下 Markdown 扩展语法：

[class@Gtk.Widget], [method@Gtk.ListView.get_factory],...

每条文档注释都应以一个单句段落开头，作为概要（它通常会与指向相应
符号、类等完整文档的链接一起显示）。概要中不应包含链接。

类的长篇文档应放在结构体的文档注释中，通常位于源文件顶部的许可证
头注释和头文件包含语句之后：

```c
  /**
   * GtkSizeRequest:
   *
   * The GtkSizeRequest interface is GTK's height-for-width geometry
   * geometry management system.
   *
   * # Geometry management
   *
   * ...
   */
```

有关文档风格和贡献指南的更多信息，请参阅[相应的贡献指南](./reference/README.md)。

### 旧代码

新加入 GTK 的代码应遵循上述风格。GTK 现有代码大体遵循这些约定，但也
存在一些差异，例如使用了制表符等。

在修改代码块或函数时顺便更新其风格是可以的，但大范围的空白变更会掩盖
源码的修订历史，应当避免。
