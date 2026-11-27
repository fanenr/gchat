# gchat storage

**English** | [简体中文](STORAGE.zh-CN.md)

`gchat` stores sessions in SQLite, image attachments as files in the data directory, and preferences in a separate JSON file. Separating settings ensures session backups exclude the API key.

## Paths and backups

| File | Default location | Contents |
| --- | --- | --- |
| `session.db` | `~/.local/share/gchat/` | Sessions, messages, drafts, per-session prompts and summaries, active session, model, and reasoning level |
| `attachments/` | `~/.local/share/gchat/attachments/` | Image files named by UUID |
| `session.db.lock` | `~/.local/share/gchat/session.db.lock` | Single-instance process lock |
| `config.json` | `~/.config/gchat/` | API key and preferences |

- `XDG_DATA_HOME` and `XDG_CONFIG_HOME` override the default base directories. On Linux, Flatpak and native builds share these host paths and lock file.
- `gchat` creates files with `0600` permissions and directories with `0700` permissions, without encryption.
- To back up sessions, quit `gchat` and copy `session.db` together with `attachments/`.

## Configuration (`config.json`)

`config.json` is a flat JSON object mapping string keys to string values. Updates merge into the existing object and replace the file atomically without creating backup files. If the file is malformed or contains non-string values, writes fail and leave the existing file unchanged.

| Key | Values / Default |
| --- | --- |
| `api_key` | DeepSeek API key (overridden at runtime if `DEEPSEEK_API_KEY` is set) |
| `theme` | `"system"` (default), `"light"`, or `"dark"` |
| `language` | `"system"` (default), `"en"`, or `"zh_CN"` (applied on restart) |
| `web_enabled` | `"true"` or `"false"` (default `"false"`) |
| `auto_title` | `"true"` (default) or `"false"` |
| `latest_reminder` | `"true"` or `"false"` (default `"false"`); sends date, weekday, platform, and language with new user messages |
| `confirm_delete_chat` | `"true"` (default) or `"false"` |
| `default_system_prompt` | Multiline text; empty or absent disables the default system prompt |
| `auto_compact` | `"true"` or `"false"` (default `"false"`) |
| `context_threshold` | Estimated input token threshold, `1024`–`1000000` (default `64000`) |
| `context_keep_turns` | Recent turns to keep verbatim during compression, `0`–`100` (default `2`) |
| `message_font`, `code_font` | Font family name; empty or absent uses the default family |
| `message_font_size`, `code_font_size` | Decimal size in logical pixels (`6`–`72`; defaults: `14` for prose, `13` for code) |

## Database schema (version 2)

[`store.c`](../store.c) defines two SQLite `STRICT` tables.

### `chats`

| Column | Data |
| --- | --- |
| `id`, `title` | Nonempty text primary key; title string (can be empty) |
| `created`, `pinned` | Nonnegative Unix timestamp in milliseconds; `0` or `1` integer |
| `draft`, `draft_images` | Normal draft text; optional ordered JSON array of image UUIDs |
| `edit_from`, `edit_text`, `edit_images` | Zero-based user-message index, edit draft text, and optional ordered JSON array of image UUIDs |
| `messages` | Ordered JSON array of message objects |
| `system_prompt` | `NULL` to inherit, `""` to omit, or nonempty text to override |
| `summary`, `summary_until` | Optional summary text and covered original message count (`NULL` and `0` without a summary) |
| `summary_position` | Count of messages before the visible summary marker (`NULL` without a summary) |

- When not editing a message, `edit_from` and `edit_images` are `NULL` and `edit_text` is `""`. When a draft has no images, `draft_images` is `NULL`.
- Each element in `messages` must include `role` (`"user"` or `"assistant"`) and string `content`.
- Assistant messages can include `reasoning` (text), `duration` (nonnegative seconds), `annotations` (citations), and `server_tool_calls`.
- User messages can include `images` (a nonempty array of attachment UUID strings) and `latest_reminder` (a metadata string).

### `session_state`

This table holds one row (`id = 1`) with `current_id` (foreign key to `chats.id`, or `NULL` only when no sessions exist), `model`, and `effort`.

## Lifecycle and transactions

`gchat` keeps one SQLite connection using the default rollback journal (WAL is disabled).

- **Locking**: `main.c` acquires `session.db.lock` before opening the database and holds it until shutdown. If another instance holds the lock, the new process exits immediately.
- **Startup and cleanup**: `store_open` enables foreign keys and verifies or migrates the schema before `store_load` reads all sessions into memory. After a full load succeeds, `gchat` removes unreferenced UUID files from `attachments/`, checking all messages, normal drafts, and edit drafts across sessions.
- **Incremental saves**: `gchat` marks modified sessions dirty and queues deleted session IDs. `store_save` runs one transaction that upserts dirty rows in `chats`, updates `session_state`, and deletes queued rows from `chats` to keep foreign keys valid. After the transaction commits, `gchat` clears the dirty flags and deletion queue.

## Schema migrations

`PRAGMA user_version` tracks the schema version (currently `2`). `store_open` accepts version `0` only on an empty database and rejects any `user_version` newer than supported without downgrading. Upgrading from version `1` preserves all sessions with no summary and an inherited system prompt.

To add a schema migration:

1. Append a migration function to `upgrade_schema` in [`store.c`](../store.c), where index `n` upgrades version `n` to `n + 1`. Do not edit released steps or add version branches to normal read/write paths.
2. Run all SQL and JSON conversions inside the `BEGIN IMMEDIATE` transaction started by `store_open`. Do not commit, set `user_version`, or delete files in `attachments/` inside a migration step.
3. Add regression fixtures in [`tests/test-store.c`](../tests/test-store.c) for single-step upgrades, multi-step upgrades, and rollback on failure, then run:

```sh
meson test -C build store --print-errorlogs
```
