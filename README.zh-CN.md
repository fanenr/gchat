# gchat

[English](README.md) | **简体中文**

`gchat` 是 DeepSeek 桌面聊天客户端。客户端用 C 编写，界面基于 GTK4 与 libadwaita。
客户端不使用 WebView，也不使用脚本层。

## 构建与运行

客户端需要以下软件：

- Meson、Ninja 和 Blueprint Compiler
- GLib 2.66 或更高
- GTK 4.12 或更高
- libadwaita 1.5 或更高
- libsoup 3
- JSON-GLib 1.6 或更高
- SQLite 3.37 或更高
- md4c
- GtkSourceView 5
- gettext

1. 安装依赖。在 macOS 上运行：

   ```sh
   brew install meson ninja blueprint-compiler gtk4 libadwaita libsoup json-glib sqlite md4c gtksourceview5 gettext
   export PKG_CONFIG_PATH="$(brew --prefix sqlite)/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
   ```

2. 编译：

   ```sh
   meson setup build && meson compile -C build
   ```

3. 启动：

   ```sh
   ./build/gchat
   ```

### Apple Silicon DMG

在装有上述依赖和 Python 3.9 或更高版本的 Apple Silicon Mac 上运行：

```sh
brew install librsvg
meson compile -C build macos-arm64
```

将镜像中的 `gchat.app` 拖到「应用程序」。它与 `./build/gchat` 共用设置和会话。
目标 Mac 不需要装 Homebrew。构建时会打印该镜像要求的最低 macOS 版本。

输出文件为 `build/gchat-<版本>-arm64.dmg`。包内应用默认使用临时签名，下载后
可能被 macOS 拦截。如需在同一路径生成公证版，使用 Developer ID Application
证书和 `notarytool` 钥匙串配置：

```sh
GCHAT_CODESIGN_IDENTITY='Developer ID Application: Example (TEAMID)' \
GCHAT_NOTARY_PROFILE='gchat' meson compile -C build macos-arm64
```

公开发布时，按许可证要求同时提供对应源码。包内的
`Contents/Resources/Licenses` 记录了依赖版本和许可证声明。

### Linux Flatpak

安装与系统架构匹配的 `.flatpak` 文件。安装后从应用菜单打开 **gchat**，
或用应用 ID 启动。以 x86-64 为例：

```sh
flatpak install --user ./gchat-x86_64.flatpak
flatpak run io.github.fanenr.gchat
```

应用通过文件分发，不在 Flathub 上架。包使用 Flathub 提供的 GNOME 运行时，
首次安装时可能需要下载。Flatpak 版和原生版共用[配置和会话数据](docs/STORAGE.zh-CN.md)。
切换版本前先退出正在运行的版本。旧 Flatpak 隔离目录中的数据不会自动迁移。

在 Linux 上，从仓库根目录用 Flatpak 和 Flatpak Builder 构建安装包：

```sh
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub org.flatpak.Builder
mkdir -p build-flatpak
flatpak_dir="${FLATPAK_USER_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/flatpak}"
flatpak run --env=FLATPAK_USER_DIR="$flatpak_dir" \
  --command=flatpak-builder org.flatpak.Builder \
  --user --install-deps-from=flathub --force-clean \
  --state-dir=build-flatpak/state --repo=build-flatpak/repo \
  build-flatpak/app packaging/flatpak/io.github.fanenr.gchat.yml
flatpak build-bundle \
  --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
  build-flatpak/repo "build-flatpak/gchat-$(flatpak --default-arch).flatpak" \
  io.github.fanenr.gchat stable
```

只需分享 `build-flatpak/` 下的 `.flatpak` 文件，不用分享构建目录或 OSTree 仓库。
公开发布时，按许可证要求提供对应源码。细节见 [Flatpak 打包](docs/DETAILS.zh-CN.md#flatpak-打包)。

## 功能

- 流式回复：用 libsoup3 异步读 SSE，支持多行事件数据，文字每帧最多刷新一次。向上滚动可阅读前文，回到底部后继续跟随。生成中可以停止，已收到的正文保留。
- Markdown 渲染：md4c 解析，渲染成原生控件。支持标题、列表、引用、表格、代码块和表格外的 HTTPS 图片。加载远程图片时，图片服务器可以看到你的 IP 地址和请求时间。
- 代码块：独立卡片，采用紧凑行距。语言栏有独立底色，包含语言标签和复制按钮。由 GtkSourceView 高亮。
- 多会话：可以在侧栏搜索、置顶、复制、重命名和删除会话。Flash 新会话默认由 DeepSeek 生成标题，可在「设置 → 常规 → 会话 → 自动生成会话标题」关闭。
- 删除确认：默认开启。可在「设置 → 常规 → 会话 → 删除会话前确认」关闭。
- 设置：「常规」包含 API 密钥、会话选项和界面语言；「外观」包含主题和阅读字体；「其他」显示应用版本、会话数据位置和「源代码」链接。
- 自适应布局：宽窗口下，消息区与输入区一起变宽。窗口最小尺寸请求为 640 × 480 逻辑像素。
- 阅读：正文和代码块可以分别选择字体与字号。行高随字体缩放，标题层次分明，列表续行对齐正文，表格使用轻量无外框布局，单元格内容垂直居中。代码块和表格不折行，超宽时可横向滚动且不显示滚动条。偏离应用默认值时才显示重置按钮。调整即时生效并持久化，侧栏与输入框保持不变。
- 消息操作：整条复制、编辑后重发、分叉新会话、重新生成、删除单条。
- 图片输入：使用 `deepseek-flash` 选择或粘贴多张图片。在输入框内预览或逐张移除，支持图文或纯图片消息。
- 联网：用 `deepseek-flash` 时，开启「联网」可让 DeepSeek 搜索网页、阅读链接。状态会保存，并在下次启动时恢复。回复下方会列出引用来源。
- 模型与推理级别：`deepseek-flash` 与 `deepseek-v4-pro`，推理级别四档。
- 草稿：每个会话独立保存。输入停顿 750ms 后自动写盘。
- 界面语言：English 与简体中文。切换后重启客户端生效。
- 数据可靠性：SQLite 事务、启动时数据库升级、单进程锁。

## 快捷键

应用快捷键在 macOS 上使用 Cmd，在 Linux 上使用 Ctrl。Tab 导航使用主题的焦点环。鼠标点击主窗口会清除键盘焦点环，输入框的聚焦提示不变。

| macOS | Linux | 功能 |
| --- | --- | --- |
| `Cmd+N` | `Ctrl+N` | 新建会话 |
| `Cmd+L` | `Ctrl+L` | 聚焦输入框 |
| `Cmd+F` | `Ctrl+F` | 聚焦搜索 |
| `Cmd+,` | `Ctrl+,` | 打开设置 |
| `Cmd+.` | `Ctrl+.` | 停止生成 |
| `Cmd+W` | `Ctrl+W` | 关闭窗口 |
| `Cmd+Q` | `Ctrl+Q` | 退出应用 |
| `Enter` | `Enter` | 发送消息 |
| `Shift+Enter` | `Shift+Enter` | 换行 |
| `Esc` | `Esc` | 取消编辑，或清空搜索 |

在 macOS 的消息输入框中，`Ctrl+F` 和 `Ctrl+B` 将光标向前、向后移动一个字符，`Ctrl+P` 和 `Ctrl+N` 将光标向上、向下移动一行。`Ctrl+A` 和 `Ctrl+E` 跳到当前显示行的行首和行尾。自动折行也算一行。

`Ctrl+K` 剪切选区或光标到段尾的文字，`Ctrl+Y` 粘回。这两个键不改动系统剪贴板。

## 数据

- 会话：`~/.local/share/gchat/session.db`。图片副本位于同目录的 `attachments/`。
- 客户端不导入或删除旧的 `session.json` 和 `images/`。
- 配置：`~/.config/gchat/config.json`。保存 API 密钥和应用设置。
- API 密钥优先读环境变量 `DEEPSEEK_API_KEY`。
- 文件以 `0600` 权限写入。内容不加密。

## 测试

```sh
meson test -C build --print-errorlogs
```

GTK 测试需要显示后端。无桌面环境时用 `xvfb-run -a meson test -C build`。

## 更多

- [存储](docs/STORAGE.zh-CN.md)：数据路径、备份、数据库格式和升级。
- [实现细节](docs/DETAILS.zh-CN.md)：功能行为、回归测试、代码结构和已知限制。
