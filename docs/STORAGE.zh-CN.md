# gchat 存储

[English](STORAGE.md) | **简体中文**

SQLite 保存会话。图片本体仍保存在数据目录中。配置单独存放，因此会话备份不含设置中保存的 API 密钥。

## 文件与备份

| 文件 | 默认位置 | 内容 |
| --- | --- | --- |
| `session.db` | `~/.local/share/gchat/` | 会话、消息、草稿、当前会话、模型和推理级别 |
| `attachments/` | 与 `session.db` 同目录 | 以 UUID 命名的图片文件 |
| `session.db.lock` | 与 `session.db` 同目录 | 进程锁，不含会话数据 |
| `config.json` | `~/.config/gchat/` | API 密钥、主题和语言 |

`XDG_DATA_HOME` 和 `XDG_CONFIG_HOME` 分别覆盖数据与配置的基准目录。
设置界面显示实际数据库路径和 API 密钥来源。新建文件的权限为 `0600`，新建目录的权限为 `0700`，
内容不加密。`DEEPSEEK_API_KEY` 优先于 `config.json` 中的密钥。

备份会话时，先正常退出 gchat，再将 `session.db` 和 `attachments/` 一起复制并保存。
事务不能替代备份。gchat 不自动保存或恢复独立的数据库备份。
旧的 `session.json`、`session.json.bak` 和 `images/` 不导入、不删除。

设置通过原子替换写入 `config.json`，不生成备份。配置不是有效的 JSON 对象，或包含非字符串值时，
保存失败，原文件保留。主题或语言保存失败时，对话框恢复上次选择。
已有的 `config.json.bak` 可能含明文密钥，须按敏感文件处理。

## 数据库格式：版本 1

[`store.c`](../store.c) 定义了两张 SQLite `STRICT` 表。`chats` 每个会话存一行：

| 列 | 数据 |
| --- | --- |
| `id`、`title` | 非空文本主键、可为空字符串的标题 |
| `created`、`pinned` | 非负 Unix 毫秒时间戳、`0` 或 `1` 的整数 |
| `draft`、`draft_images` | 普通草稿正文、可选的有序图片 UUID JSON 数组 |
| `edit_from`、`edit_text`、`edit_images` | 从 0 开始的用户消息索引、编辑草稿正文、可选的有序图片 UUID JSON 数组 |
| `messages` | 有序的消息对象 JSON 数组 |

未编辑时，`edit_from` 与 `edit_images` 为 SQL `NULL`，`edit_text` 为空字符串。
普通草稿没有附件时，`draft_images` 也为 SQL `NULL`。
每条消息必填 `role`（`user` 或 `assistant`）和文本 `content`。
文本 `reasoning` 和非负秒数 `duration` 可选。只有用户消息可以含 `images`。
已存的附件列表必须非空，每个元素是 UUID，不是文件路径。

`session_state` 只有一行，`id = 1`。`current_id` 引用 `chats` 中的一行；
`model` 和 `effort` 为文本。只有无会话时，`current_id` 才能是 SQL `NULL`。

保存草稿不会重写其他会话，但仍会重新序列化当前变更会话的消息数组。
启动时会载入全部会话，暂不支持按需加载或消息级索引。

## 启动与保存

应用使用一条 SQLite 连接和默认回滚日志，不启用 WAL。

- [`main.c`](../main.c) 在打开数据库前获取 `session.db.lock`，关闭连接后才释放锁。
  第二个使用同一数据库的实例若无法取锁，就会退出。不要删除锁文件来绕过它。
- `store_open` 启用外键，在 `store_load` 读取会话前检查或升级格式。数据库无法打开或校验失败时，
  客户端退出。只有成功读完数据库，才清理 `attachments/` 中未被引用的 UUID 文件。
  清理检查普通草稿、编辑草稿和全部消息；删除一个会话不会清除其他会话仍引用的图片。
- 修改会话时设置待保存标记，删除时将 ID 加入队列。`store_save` 在同一事务内插入或更新变更会话，
  保存已变更的会话状态，最后执行删除。先保存状态是为了保持当前会话的外键有效。
  提交成功后才清除标记和队列；失败可以重试。
- 输入停顿 750 ms 后自动保存草稿。切换会话、发送消息或关窗时也会保存。
  流式回复不逐 token 写盘；生成过程中保存时，会一并写入已收到的正文和推理。
  若关窗保存失败，客户端保留窗口和内存中的草稿。

## 修改数据格式

`PRAGMA user_version` 记录的是**数据库格式版本**，不是应用版本。当前只有 `0 → 1` 建库步骤。
只有不含用户对象的数据库才接受版本 `0`。比应用支持版本更新的数据库会被拒绝，不自动降级。

1. 已有数据库需要更改结构或转换数据时，在 [`store.c`](../store.c) 的 `upgrade_schema` 中追加一步。
   下标 `n` 对应 `n → n + 1`，步骤数量就是当前版本。已存消息 JSON 的不兼容变更也走这条路径。
2. 不修改已交付的步骤。在升级步骤中运行 SQL 或转换数据，不在正常读写中加版本分支。
   正常读写只认当前格式。
3. 在 `store_open` 的 `BEGIN IMMEDIATE` 事务内执行步骤。单步不提交，也不设置 `user_version`。
   全部成功后统一写入目标版本并提交。失败则回滚，应用停止启动。
   不在数据库升级中移动或删除图片，因为 SQLite 无法回滚文件操作。
4. 增加上一版数据库的测试夹具。验证升级后的数据和附件引用，不只检查版本号。
   有多步升级后，同时测试跳过中间版本的情况。模拟失败，确认数据库仍保留原结构和版本号。

[存储测试](../tests/test-store.c) 覆盖首次建库与重开、增量写入、保存或升级失败时的回滚、
无效或过新数据库、附件清理和配置。运行方式：

```sh
meson test -C build store --print-errorlogs
```
