# gchat 存储

[English](STORAGE.md) | **简体中文**

`gchat` 使用 SQLite 保存会话，将图片附件以文件形式保存在数据目录，并将应用配置单独存放在 JSON 文件中，因此会话备份不会包含 API 密钥。

## 文件路径与备份

| 文件 | 默认位置 | 内容 |
| --- | --- | --- |
| `session.db` | `~/.local/share/gchat/` | 会话、消息、草稿、会话提示词和摘要、当前会话、模型和推理级别 |
| `attachments/` | `~/.local/share/gchat/attachments/` | 以 UUID 命名的图片文件 |
| `session.db.lock` | `~/.local/share/gchat/session.db.lock` | 单实例进程锁 |
| `config.json` | `~/.config/gchat/` | API 密钥与应用设置 |

- `XDG_DATA_HOME` 与 `XDG_CONFIG_HOME` 分别覆盖数据与配置的默认基准目录。在 Linux 上，Flatpak 版与原生构建共用这些宿主机路径及同一把进程锁。
- 客户端以 `0600` 权限创建文件，以 `0700` 权限创建目录，内容不加密。
- 备份会话时，先正常退出 `gchat`，再将 `session.db` 与 `attachments/` 一并复制。

## 配置文件 (`config.json`)

`config.json` 是键值均为字符串的扁平 JSON 对象。更新配置时会合并已有键值并原子替换文件，不生成备份文件；若原文件不是合法 JSON 对象或包含非字符串值，写入会失败并保留原文件。

| 键 | 取值 / 默认值 |
| --- | --- |
| `api_key` | DeepSeek API 密钥（若设置了环境变量 `DEEPSEEK_API_KEY`，运行时优先使用环境变量） |
| `theme` | `"system"`（默认）、`"light"` 或 `"dark"` |
| `language` | `"system"`（默认）、`"en"` 或 `"zh_CN"`（重启客户端后生效） |
| `web_enabled` | `"true"` 或 `"false"`（默认 `"false"`） |
| `auto_title` | `"true"`（默认）或 `"false"` |
| `confirm_delete_chat` | `"true"`（默认）或 `"false"` |
| `default_system_prompt` | 多行文本，为空或缺失时不发送默认系统提示词 |
| `auto_compact` | `"true"` 或 `"false"`（默认 `"false"`） |
| `context_threshold` | 输入 token 估算阈值，`1024`–`1000000`（默认 `64000`） |
| `context_keep_turns` | 压缩时保留原文的最近轮数，`0`–`100`（默认 `2`） |
| `message_font`、`code_font` | 字体族名称；为空或缺失时使用默认字体 |
| `message_font_size`、`code_font_size` | 逻辑像素十进制字号（`6`–`72`，正文默认 `14`，代码默认 `13`） |

## 数据库结构（版本 2）

[`store.c`](../store.c) 定义了两张 SQLite `STRICT` 表。

### `chats`

| 列 | 数据 |
| --- | --- |
| `id`、`title` | 非空文本主键、可为空字符串的标题 |
| `created`、`pinned` | 非负 Unix 毫秒时间戳、`0` 或 `1` 的整数 |
| `draft`、`draft_images` | 普通草稿正文、可选的有序图片 UUID JSON 数组 |
| `edit_from`、`edit_text`、`edit_images` | 从 0 开始的用户消息索引、编辑草稿正文、可选的有序图片 UUID JSON 数组 |
| `messages` | 有序的消息对象 JSON 数组 |
| `system_prompt` | `NULL` 表示继承，`""` 表示不发送，非空文本表示自定义 |
| `summary`、`summary_until` | 可选的摘要和它在请求中替换的原始消息数；无摘要时为 `NULL` 和 `0` |
| `summary_position` | 可见摘要标记之前的消息数；无摘要时为 `NULL` |

- 未处于消息编辑状态时，`edit_from` 与 `edit_images` 为 `NULL`，`edit_text` 为 `""`；普通草稿无图片时，`draft_images` 为 `NULL`。
- `messages` 中的每条记录必须包含 `role`（`"user"` 或 `"assistant"`）与文本 `content`。
- 助手消息可包含 `reasoning`（推理文本）、`duration`（非负秒数）、`annotations`（引用来源）和 `server_tool_calls`（联网工具记录）。
- 用户消息可包含非空的 `images` 数组（元素为附件 UUID 字符串）。

### `session_state`

仅存一行（`id = 1`），记录 `current_id`（外键引用 `chats.id`，仅当无会话时为 `NULL`）、`model` 与 `effort`。

## 生命周期与事务

应用保持单条 SQLite 连接并使用默认回滚日志（不启用 WAL）。

- **进程锁**：`main.c` 在打开数据库前获取 `session.db.lock` 并持有至退出；若锁已被占用，新启动的进程会立即退出。
- **启动与清理**：`store_open` 启用外键并在 `store_load` 载入全部会话前校验或升级表结构。只有完整读出数据库后，客户端才会清理 `attachments/` 中未被任何会话消息、普通草稿或编辑草稿引用的 UUID 文件。
- **增量保存**：变更的会话标记为脏，删除的会话 ID 加入队列。`store_save` 在单个事务内依次插入或更新变更的 `chats` 行、更新 `session_state`，最后删除队列中的 `chats` 行以保证外键有效。事务提交成功后才清空脏标记和删除队列。

## 格式升级

`PRAGMA user_version` 记录数据库结构版本（当前为 `2`）。仅空数据库接受版本 `0`；高于当前客户端支持版本的数据库会被直接拒绝，不会降级读取。版本 `1` 升级后保留会话，新字段默认为无摘要、继承系统提示词。

升级数据库格式的步骤：

1. 在 [`store.c`](../store.c) 的 `upgrade_schema` 末尾追加升级步骤（下标 `n` 对应版本 `n → n + 1`）。不要修改已发布的步骤，也不要在日常读写代码中添加版本分支。
2. 所有 SQL 与 JSON 转换均在 `store_open` 开启的 `BEGIN IMMEDIATE` 事务中执行；单步内不要自行提交、修改 `user_version` 或删除 `attachments/` 中的文件。
3. 在 [`tests/test-store.c`](../tests/test-store.c) 中补充上一版本的测试夹具，覆盖单步升级、跨版本升级和失败回滚：

```sh
meson test -C build store --print-errorlogs
```
