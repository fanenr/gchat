# gchat 详细说明

[English](DETAILS.md) | **简体中文**

本文说明 `gchat` 的代码结构、核心运行机制、打包方式与已知限制。数据路径、数据库表结构与版本迁移见[存储说明](STORAGE.zh-CN.md)。

## 代码结构

| 路径 | 职责 |
| --- | --- |
| `meson.build` | 项目版本、构建目标与依赖声明 |
| `main.c` | 主窗口、侧栏与消息列表、设置面板、HTTP/SSE 请求生命周期与退出流程 |
| `md.c` / `md.h` | 基于 `md4c` 的原生 GTK4 控件渲染器，处理正文、代码块卡片、表格与远程图片 |
| `store.c` / `store.h` | SQLite 会话存取、结构升级、单实例进程锁与 `config.json` 原子写入 |
| `i18n.c` / `i18n.h` | 启动时的语言环境匹配与 `gettext` 初始化 |
| `macos-bundle.c` / `macos-bundle.h` | macOS `.app` 包内的资源、词典、图标与 TLS 信任库初始化 |
| `ui/` | Blueprint 界面定义、CSS 样式与 `GResource` 清单 |
| `po/` | `gettext` 翻译模板（`POTFILES`）与简体中文词典（`zh_CN.po`） |
| `packaging/` | macOS DMG 打包脚本及 Linux Flatpak 清单与元数据 |
| `tests/` | 回归测试套件（`test-app.c`、`test-store.c`、`test-md.c`、`test-i18n.c`） |

## 核心机制

### 请求与流式输出

- **SSE 流处理**：`main.c` 使用 `libsoup3` 异步读取 SSE，合并多行 `data:` 字段并在空行处分发事件。正文与推理增量按 GTK 帧时钟合并，每帧最多刷新一次界面。
- **上下文回传**：发送消息时，客户端向 DeepSeek 发送当前会话的用户与助手消息（含图片附件）。本地草稿和已保存的推理内容（`reasoning_content`）不会作为消息正文回传。
- **联网搜索与网页阅读**：仅在 `deepseek-flash` 下可用。开启后由服务端决定是否执行搜索或读取链接；客户端在流式阶段显示工具进度，在回复下方展示去重后的 HTTP/HTTPS 引用来源，并在后续 Flash 轮次中回传 `server_tool_calls` 以保留已读网页上下文。
- **自动标题**：未命名的 `deepseek-flash` 新会话首次发送时，先取用户首行文字作为临时标题，流式响应正常结束并返回标题后再替换保存。

### Markdown 与媒体

- **原生控件树**：`md.c` 通过 `md4c` 解析 CommonMark 与部分 GFM 语法，直接生成原生 GTK 控件（正文使用 `GtkTextView`，表格使用 `GtkGrid`，代码块使用带 `GtkSourceView` 的独立卡片），不依赖 WebView。
- **远程图片**：表格外的 HTTPS 图片使用不带 API 凭据的独立请求异步加载，不跟随重定向，超时时间为 20 秒，单图上限为 8 MiB 和 1600 万像素，显示尺寸不超过 320 × 240 逻辑像素。外部链接仅打开 `http`、`https` 与 `mailto` 协议。
- **图片输入**：`deepseek-flash` 支持通过文件选择器或剪贴板添加 PNG、JPEG、GIF 与 WebP 图片。单图上限为 8 MiB、单边 8192 像素（单次请求达 15 张及以上时为 4096 像素）、总像素 1600 万；单次请求最多包含 600 张图片，含历史图片与 Base64 编码在内的请求体上限为 48 MiB。导入的图片按 UUID 存入 `attachments/`，并在每次启动成功加载数据库后清理未引用的文件。

### 会话与草稿

- 每个会话独立维护消息历史、推理记录、普通草稿与编辑草稿（编辑历史消息时暂存的输入）。
- 输入停顿 750 ms 后自动保存草稿；切换会话、发送消息或关闭窗口时立即在单个 SQLite 事务内落盘。
- 编辑或分叉消息时保留图片引用，在用户真正发送前不会修改原有历史记录。

### 国际化

- 界面支持 English 与简体中文（保存在 `config.json` 的 `language` 字段，重启后生效）。
- `i18n.c` 将 `zh_CN`、`zh_SG`、`zh-Hans` 和通用 `zh` 匹配为简体中文，其余语言环境（含繁体中文）回退到英语。
- 修改界面文案后更新翻译词典：

  ```sh
  meson compile -C build gchat-update-po
  ```

## 打包机制

### macOS 应用包

`macos-arm64` 构建目标在临时目录组装 `gchat.app`，将非系统动态库与 GIO/gdk-pixbuf 模块复制到 `Contents/Frameworks`，将 Mach-O 动态库引用改写为包内相对路径，并在签名前校验依赖闭包。最低 macOS 版本取主程序与打包库中最高的部署目标版本。

### Flatpak 打包

[Flatpak 清单](../packaging/flatpak/io.github.fanenr.gchat.yml)基于 GNOME SDK 51 构建，启用 `-Dflatpak=true` 与 `/app` 安装前缀，并从带校验和的源码包编译 `md4c`。沙箱开放网络访问、Wayland/X11 显示以及文件选择与链接打开门户，并仅挂载宿主机的 `xdg-data/gchat` 与 `xdg-config/gchat`，使 Flatpak 版与原生构建共享数据库和配置文件。

## 测试

`tests/` 下的回归测试使用临时目录与本地模拟 HTTP 服务运行，不访问外部网络或用户数据：

- `test-app.c`：SSE 解析、流式生命周期、联网搜索与自动标题、设置与界面交互。
- `test-store.c`：SQLite 建库与迁移、事务保存、进程锁、附件清理与 `config.json` 读写。
- `test-md.c`：Markdown 控件渲染、代码高亮、表格布局与文本选区。
- `test-i18n.c`：语言环境匹配、回退规则与词典加载。

## 已知限制

- 表格单元格支持行内样式与行内代码，不支持多行代码块或内嵌图片（图片显示为 `[alt 文本]`）。
- LaTeX 公式（`$…$`）按源码文本显示。
- 文本选区限于单个 GTK 文本控件内，无法跨越代码块、表格或多条消息连续拖选。
- 代码块语法高亮按 `GtkSourceView` 语言 ID 及固定别名表（如 `javascript` → `js`、`csharp` → `c-sharp`）匹配，未知语言按纯文本显示。
- SSE 解析器针对 DeepSeek 的 JSON 数据流与 `[DONE]` 结束标记设计，不是通用 SSE 客户端。
- HTML 实体解码仅覆盖数字实体与常见命名实体表。
- 暂不支持拖放导入图片；构建包含大量历史图片的请求时在界面线程读取文件。
- 草稿自动保存有 750 ms 延迟，进程被强制终止（`SIGKILL` 或崩溃）时会丢失尚未提交的草稿或流式输出。
