# gchat

[English](README.md) | **简体中文**

`gchat` 是使用 C、GTK4 与 libadwaita 编写的 DeepSeek 原生桌面客户端。

## 功能

- **模型与推理**：支持 `deepseek-flash` 与 `deepseek-v4-pro`，提供四档推理级别（`none`、`low`、`high`、`max`）与可折叠的思考过程。
- **联网与图片**：使用 `deepseek-flash` 时支持联网搜索并展示引用来源，支持选择或粘贴多张图片发送。
- **原生 Markdown**：无需 WebView，直接将标题、列表、引用、表格、GtkSourceView 语法高亮代码块和 HTTPS 图片渲染为 GTK 控件。
- **会话管理**：支持搜索、置顶、重命名、复制与删除会话；支持单条消息复制、编辑重发、重新生成或分叉为新会话；提供自动生成标题与独立会话草稿。
- **用户元信息**：可选向 DeepSeek 分享本地日期、星期、应用平台和界面语言。默认关闭；此功能不会分享位置。
- **消息导航**：点击聊天标题栏的编号列表图标，可搜索你发过的消息、跳到某一轮提问，或返回最新消息。
- **外观与语言**：支持跟随系统、浅色与深色主题；正文与代码块可分别设置字体与字号；支持 English 与简体中文界面。

## 构建与运行

依赖项：

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

在装有上述依赖和 Python 3.9 或更高版本的 Apple Silicon Mac 上：

```sh
brew install librsvg
meson compile -C build macos-arm64
```

生成的 `build/gchat-<版本>-arm64.dmg` 已打包非系统动态库，目标 Mac 无需安装
Homebrew。默认使用临时签名；如需使用 Developer ID Application 证书与
`notarytool` 钥匙串配置签名并公证：

```sh
GCHAT_CODESIGN_IDENTITY='Developer ID Application: Example (TEAMID)' \
GCHAT_NOTARY_PROFILE='gchat' meson compile -C build macos-arm64
```

打包的依赖版本与许可证声明位于应用包内的 `Contents/Resources/Licenses`。

### Linux Flatpak

安装对应架构的独立 `.flatpak` 安装包：

```sh
flatpak install --user ./gchat-x86_64.flatpak
flatpak run io.github.fanenr.gchat
```

安装包使用 Flathub 提供的 GNOME 运行时，并与原生构建共用[配置和会话数据](docs/STORAGE.zh-CN.md)。
从源码构建 `.flatpak` 安装包：

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

## 快捷键

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

macOS 消息输入框还支持 Emacs 风格光标移动（`Ctrl+F/B`、`Ctrl+P/N`、`Ctrl+A/E`）
以及独立于系统剪贴板的剪切与粘回（`Ctrl+K`、`Ctrl+Y`）。

## 数据

- 会话数据：`~/.local/share/gchat/session.db` 与同目录的 `attachments/`。
- 配置文件：`~/.config/gchat/config.json`。
- API 密钥优先读环境变量 `DEEPSEEK_API_KEY`。
- 文件以 `0600` 权限写入，内容不加密。

## 测试

```sh
meson test -C build --print-errorlogs
```

无桌面环境时可运行 `xvfb-run -a meson test -C build`。

## 更多

- [存储](docs/STORAGE.zh-CN.md)：文件路径、备份、数据库结构与版本升级。
- [实现细节](docs/DETAILS.zh-CN.md)：代码结构、核心机制与已知限制。
