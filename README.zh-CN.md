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
   meson setup build && ninja -C build
   ```

3. 启动：

   ```sh
   ./build/gchat
   ```

## 功能

- 流式回复：用 libsoup3 异步读 SSE，支持多行事件数据，边生成边显示。生成中可以停止，已收到的正文保留。
- Markdown 渲染：md4c 解析，渲染成原生控件。支持标题、列表、引用、表格、代码块和表格外的 HTTPS 图片。加载远程图片时，图片服务器可以看到你的 IP 地址和请求时间。
- 代码块：独立卡片，带语言标签和复制按钮，由 GtkSourceView 高亮。
- 多会话：可以在侧栏搜索、置顶、复制、重命名和删除会话。Flash 新会话默认由 DeepSeek 生成标题，可在「设置 → 常规 → 会话 → 自动生成会话标题」关闭。
- 删除确认：默认开启。可在「设置 → 常规 → 会话 → 删除会话前确认」关闭。
- 设置：「常规」包含 API 密钥、会话选项和界面语言；「外观」包含主题和阅读字体；「其他」显示应用版本和会话数据位置。
- 自适应布局：宽窗口下，消息区与输入区一起变宽。窗口最小尺寸请求为 640 × 480 逻辑像素。
- 阅读：正文和代码块可以分别选择字体与字号。行高随字体缩放，标题层次分明，列表续行对齐正文，表格使用轻量无外框布局。偏离应用默认值时才显示重置按钮。调整即时生效并持久化，侧栏与输入框保持不变。
- 消息操作：整条复制、编辑后重发、分叉新会话、重新生成、删除单条。
- 图片输入：使用 `deepseek-flash` 选择或粘贴多张图片。在输入框内预览或逐张移除，支持图文或纯图片消息。
- 联网：用 `deepseek-flash` 时，开启「联网」可让 DeepSeek 搜索网页、阅读链接。回复下方会列出引用来源。
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
| `Enter` | `Enter` | 发送消息 |
| `Shift+Enter` | `Shift+Enter` | 换行 |
| `Esc` | `Esc` | 取消编辑，或清空搜索 |

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
