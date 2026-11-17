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
- md4c
- GtkSourceView 5
- gettext

1. 安装依赖。在 macOS 上运行：

   ```sh
   brew install meson ninja blueprint-compiler gtk4 libadwaita libsoup json-glib md4c gtksourceview5 gettext
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
- 多会话：可以在侧栏搜索、置顶、复制、重命名和删除会话。
- 消息操作：整条复制、编辑后重发、分叉新会话、重新生成、删除单条。
- 模型与推理级别：`deepseek-flash` 与 `deepseek-v4-pro`，推理级别四档。
- 草稿：每个会话独立保存。输入停顿 750ms 后自动写盘。
- 界面语言：English 与简体中文。切换后重启客户端生效。
- 数据可靠性：原子写入、备份恢复、单进程锁。

## 快捷键

macOS 使用 Cmd，也支持 Ctrl。Linux 使用 Ctrl。

| 快捷键 | 功能 |
| --- | --- |
| `Ctrl/Cmd+N` | 新建会话 |
| `Ctrl/Cmd+L` | 聚焦输入框 |
| `Ctrl/Cmd+F` | 聚焦搜索 |
| `Ctrl/Cmd+,` | 打开设置 |
| `Ctrl/Cmd+.` | 停止生成 |
| `Enter` | 发送消息 |
| `Shift+Enter` | 换行 |
| `Esc` | 取消编辑，或清空搜索 |

## 数据

- 会话：`~/.local/share/gchat/session.json`。
- 配置：`~/.config/gchat/config.json`。保存 API 密钥、主题和语言。
- API 密钥优先读环境变量 `DEEPSEEK_API_KEY`。
- 文件以 `0600` 权限写入。内容不加密。

## 测试

```sh
meson test -C build --print-errorlogs
```

GTK 测试需要显示后端。无桌面环境时用 `xvfb-run -a meson test -C build`。

## 更多

以下内容见 [docs/DETAILS.zh-CN.md](docs/DETAILS.zh-CN.md)：

- 会话文件格式
- 本地数据与安全
- 回归测试覆盖
- 代码结构
- 已知限制
