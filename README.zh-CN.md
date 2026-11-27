# gchat

[English](README.md) | **简体中文**

`gchat` 是使用 C、GTK4 和 libadwaita 编写的 DeepSeek 原生桌面客户端。

## 功能

- **模型与推理**：支持 `deepseek-flash` 与 `deepseek-v4-pro`、四档推理级别（`none`、`low`、`high`、`max`）以及可折叠的推理内容。
- **联网与图片**：使用 `deepseek-flash` 时，支持联网搜索并显示引用来源，支持从文件或剪贴板添加多张图片。
- **原生 Markdown**：不依赖 WebView，将标题、列表、引用、表格、语法高亮代码块和 HTTPS 图片渲染为 GTK 控件。
- **会话管理**：支持搜索、置顶、重命名、复制和删除会话。支持复制、编辑重发、重新生成或分叉消息为新会话。提供自动标题与独立草稿。
- **上下文管理**：支持设置默认或单会话系统提示词。支持手动或自动将早期历史压缩为摘要，保留原始消息。
- **用户元信息**：默认关闭。开启后，向 DeepSeek 发送本地日期、星期、应用平台和界面语言，不包含位置。
- **消息导航**：打开标题栏的编号列表，搜索用户消息、跳到历史提问或返回最新消息。
- **外观与语言**：支持跟随系统、浅色和深色主题，正文与代码块分别设置字体与字号。支持 English 与简体中文；修改界面语言后，必须重启 `gchat` 使设置生效。

## 上下文管理

- 在「设置 → 上下文」中配置默认系统提示词、自动压缩阈值（默认 64,000 token）和「保留原文轮数」（默认 2 轮）。自动压缩默认关闭。将「保留原文轮数」设为 `0` 时，压缩全部早期轮次。
- 打开图片按钮旁的「会话上下文」，可以继承、自定义或不发送系统提示词，也可以手动执行「压缩上下文」。
- 压缩会向 `deepseek-flash` 发送一次额外的计费请求，涵盖早期消息及其图片附件。后续请求使用摘要替代这部分历史，原始消息与图片保留在会话中。
- 「上下文已压缩」标记压缩发生的位置。点击标记可以展开或收起摘要。标记之前的消息为只读（包含保留原文的轮次），但仍可以复制或分叉。
- 估算口径、圆环颜色和校准规则见[实现细节](docs/DETAILS.zh-CN.md#上下文管理)。

## 构建与运行

依赖项：

- Meson、Ninja 和 Blueprint Compiler
- GLib 2.66 或更高版本
- GTK 4.12 或更高版本
- libadwaita 1.5 或更高版本
- libsoup 3
- JSON-GLib 1.6 或更高版本
- SQLite 3.37 或更高版本
- md4c
- GtkSourceView 5
- gettext

1. 安装依赖。在 macOS 上运行：

   ```sh
   brew install meson ninja blueprint-compiler gtk4 libadwaita libsoup json-glib sqlite md4c gtksourceview5 gettext
   export PKG_CONFIG_PATH="$(brew --prefix sqlite)/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
   ```

2. 配置并编译：

   ```sh
   meson setup build && meson compile -C build
   ```

3. 启动 `gchat`：

   ```sh
   ./build/gchat
   ```

### Apple Silicon DMG

在装有上述依赖和 Python 3.9 或更高版本的 Apple Silicon Mac 上运行：

```sh
brew install librsvg
meson compile -C build macos-arm64
```

该命令生成 `build/gchat-<版本>-arm64.dmg`。安装包自带非系统动态库，目标 Mac 无需安装 Homebrew。

默认使用临时签名。使用 Developer ID Application 证书和 `notarytool` 钥匙串配置签名并公证时，运行：

```sh
GCHAT_CODESIGN_IDENTITY='Developer ID Application: Example (TEAMID)' \
GCHAT_NOTARY_PROFILE='gchat' meson compile -C build macos-arm64
```

打包的依赖版本与许可证声明保存在应用包内的 `Contents/Resources/Licenses`。

### Linux Flatpak

安装对应架构的独立 `.flatpak` 安装包时，运行：

```sh
flatpak install --user ./gchat-x86_64.flatpak
flatpak run io.github.fanenr.gchat
```

安装包使用 Flathub 的 GNOME 运行时，并与原生构建共用[存储](docs/STORAGE.zh-CN.md)路径。

从源码构建 `.flatpak` 安装包时，运行：

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

| macOS | Linux | 操作 |
| --- | --- | --- |
| `Cmd+N` | `Ctrl+N` | 新建会话 |
| `Cmd+L` | `Ctrl+L` | 聚焦输入框 |
| `Cmd+F` | `Ctrl+F` | 聚焦搜索 |
| `Cmd+,` | `Ctrl+,` | 打开设置 |
| `Cmd+.` | `Ctrl+.` | 停止生成或压缩 |
| `Cmd+W` | `Ctrl+W` | 关闭窗口 |
| `Cmd+Q` | `Ctrl+Q` | 退出 `gchat` |
| `Enter` | `Enter` | 发送消息 |
| `Shift+Enter` | `Shift+Enter` | 换行 |
| `Esc` | `Esc` | 取消编辑或清空搜索 |

在 macOS 上，消息输入框还支持 Emacs 风格光标移动（`Ctrl+F/B`、`Ctrl+P/N`、`Ctrl+A/E`）和基于内部缓冲区的剪切与粘回（`Ctrl+K`、`Ctrl+Y`）。

## 数据

- 会话数据：`~/.local/share/gchat/session.db` 与 `~/.local/share/gchat/attachments/`。
- 配置文件：`~/.config/gchat/config.json`。
- 设置环境变量 `DEEPSEEK_API_KEY` 时，运行期优先使用该变量覆盖已保存的 API 密钥。
- `gchat` 以 `0600` 权限创建数据文件，内容不加密。

## 测试

```sh
meson test -C build --print-errorlogs
```

在无桌面环境中，可以运行 `xvfb-run -a meson test -C build`。

## 更多

- [存储](docs/STORAGE.zh-CN.md)：文件路径、备份、数据库结构与格式升级。
- [实现细节](docs/DETAILS.zh-CN.md)：代码结构、核心机制、打包方式与已知限制。
