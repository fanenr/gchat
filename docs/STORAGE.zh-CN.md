# gchat 存储

[English](STORAGE.md) | **简体中文**

`gchat` 使用 SQLite 保存会话，将图片附件存于数据目录，并将偏好设置单独保存在 JSON 文件中。将设置与会话分离后，备份会话不会包含 API 密钥。

## 文件路径与备份

| 文件 | 默认位置 | 内容 |
| --- | --- | --- |
| `session.db` | `~/.local/share/gchat/` | 会话、消息、草稿、单会话提示词与摘要、当前会话、模型与推理级别 |
| `attachments/` | `~/.local/share/gchat/attachments/` | 以 UUID 命名的图片文件 |
| `session.db.lock` | `~/.local/share/gchat/session.db.lock` | 单实例进程锁 |
| `config.json` | `~/.config/gchat/` | API 密钥与偏好设置 |

- `XDG_DATA_HOME` 与 `XDG_CONFIG_HOME` 覆盖默认基准目录。在 Linux 上，Flatpak 版与原生构建共用这些宿主机路径和同一把进程锁。
- `gchat` 以 `0600` 权限创建文件、以 `0700` 权限创建目录，内容不加密。
- 备份会话时，先正常退出 `gchat`，再同时复制 `session.db` 与 `attachments/`。

## 配置文件（`config.json`）

`config.json` 是键值均为字符串的扁平 JSON 对象。更新配置时，`gchat` 合并已有键值并原子替换文件，不生成备份文件。如果原文件不是合法 JSON 对象或包含非字符串值，写入失败并保留原文件。

| 键 | 取值 / 默认值 |
| --- | --- |
| `api_key` | DeepSeek API 密钥（设置环境变量 `DEEPSEEK_API_KEY` 时，运行期优先使用环境变量） |
| `theme` | `"system"`（默认）、`"light"` 或 `"dark"` |
| `language` | `"system"`（默认）、`"en"` 或 `"zh_CN"`（重启后生效） |
| `web_enabled` | `"true"` 或 `"false"`（默认 `"false"`） |
| `auto_title` | `"true"`（默认）或 `"false"` |
| `latest_reminder` | `"true"` 或 `"false"`（默认 `"false"`）；开启时随新用户消息发送日期、星期、平台和语言 |
| `confirm_delete_chat` | `"true"`（默认）或 `"false"` |
| `default_system_prompt` | 多行文本；为空或缺失时不发送默认系统提示词 |
| `auto_compact` | `"true"` 或 `"false"`（默认 `"false"`） |
| `context_threshold` | 输入 token 估算阈值，`1024`–`1000000`（默认 `64000`） |
| `context_keep_turns` | 压缩时保留原文的最近轮数，`0`–`100`（默认 `2`） |
| `message_font`、`code_font` | 字体族名称；为空或缺失时使用默认字体 |
| `message_font_size`、`code_font_size` | 逻辑像素十进制字号（`6`–`72`；正文默认 `14`，代码默认 `13`） |

## 数据库结构（版本 2）

[`store.c`](../store.c) 定义了两张 SQLite `STRICT` 表。

### `chats`

| 列 | 数据 |
| --- | --- |
| `id`、`title` | 非空文本主键；标题文本（可以为空字符串） |
| `created`、`pinned` | 非负 Unix 毫秒时间戳；`0` 或 `1` 的整数 |
| `draft`、`draft_images` | 普通草稿正文；可选的有序图片 UUID JSON 数组 |
| `edit_from`、`edit_text`、`edit_images` | 从 0 开始的用户消息索引、编辑草稿正文与可选的有序图片 UUID JSON 数组 |
| `messages` | 有序的消息对象 JSON 数组 |
| `system_prompt` | `NULL` 表示继承，`""` 表示省略，非空文本表示自定义 |
| `summary`、`summary_until` | 可选摘要文本及其在请求中替换的原始消息数（无摘要时为 `NULL` 和 `0`） |
| `summary_position` | 可见摘要标记之前的消息数（无摘要时为 `NULL`） |

- 未处于编辑状态时，`edit_from` 与 `edit_images` 为 `NULL`，`edit_text` 为 `""`。普通草稿无图片时，`draft_images` 为 `NULL`。
- `messages` 中的每条记录必须包含 `role`（`"user"` 或 `"assistant"`）与字符串 `content`。
- 助手消息可以包含 `reasoning`（推理内容）、`duration`（非负秒数）、`annotations`（引用来源）和 `server_tool_calls`（联网工具记录）。
- 用户消息可以包含非空的 `images` 数组（附件 UUID 字符串列表）和 `latest_reminder`（元信息字符串）。

### `session_state`

该表只存一行（`id = 1`），记录 `current_id`（引用 `chats.id` 的外键，仅无会话时为 `NULL`）、`model` 和 `effort`。

## 生命周期与事务

`gchat` 保持单条 SQLite 连接并使用默认回滚日志（不启用 WAL）。

- **进程锁**：`main.c` 在打开数据库前获取 `session.db.lock` 并持有至退出。另一个实例持有锁时，新进程立即退出。
- **启动与清理**：`store_open` 启用外键并在 `store_load` 载入全部会话前校验或升级表结构。完整读出数据库后，`gchat` 扫描全部会话的消息、普通草稿和编辑草稿，并删除 `attachments/` 中没有引用的 UUID 文件。
- **增量保存**：`gchat` 将改动的会话标记为脏，并将待删除的会话 ID 加入队列。`store_save` 在单个事务内依次更新或插入脏 `chats` 行、更新 `session_state`、删除队列中的 `chats` 行以维持外键有效。事务提交成功后，`gchat` 清空脏标记和删除队列。

## 格式升级

`PRAGMA user_version` 记录数据库结构版本（当前为 `2`）。`store_open` 仅在空数据库上接受版本 `0`，并拒绝高于当前支持版本的数据库，不做降级读取。从版本 `1` 升级后，原有会话保持完整，新字段默认为无摘要和继承系统提示词。

新增格式升级步骤：

1. 在 [`store.c`](../store.c) 的 `upgrade_schema` 末尾追加升级函数（下标 `n` 将版本 `n` 升级为 `n + 1`）。不得修改已发布的步骤，也不得在日常读写路径中添加版本分支。
2. 所有 SQL 与 JSON 转换必须在 `store_open` 开启的 `BEGIN IMMEDIATE` 事务中执行。单步函数内不得提交事务、修改 `user_version` 或删除 `attachments/` 中的文件。
3. 在 [`tests/test-store.c`](../tests/test-store.c) 中补充测试夹具，覆盖单步升级、跨版本升级和失败回滚，再运行：

```sh
meson test -C build store --print-errorlogs
```
