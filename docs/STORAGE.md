# gchat storage

**English** | [简体中文](STORAGE.zh-CN.md)

`gchat` stores conversations in SQLite, image attachments as files in the data directory, and application settings in a separate JSON file so conversation backups do not include the API key.

## Paths and backups

| File | Default location | Contents |
| --- | --- | --- |
| `session.db` | `~/.local/share/gchat/` | Conversations, messages, drafts, per-conversation prompts and summaries, active session, model, and reasoning level |
| `attachments/` | `~/.local/share/gchat/attachments/` | Image files named by UUID |
| `session.db.lock` | `~/.local/share/gchat/session.db.lock` | Single-instance process lock |
| `config.json` | `~/.config/gchat/` | API key and app preferences |

- `XDG_DATA_HOME` and `XDG_CONFIG_HOME` override the default base directories. On Linux, the Flatpak and native builds share these host paths and the same lock file.
- Files are created with `0600` permissions and directories with `0700`, without encryption.
- To back up conversations, quit `gchat` and copy `session.db` together with `attachments/`.

## Configuration (`config.json`)

`config.json` is a flat JSON object mapping string keys to string values. Updates merge into the existing object and replace the file atomically without creating backup files. If the file is malformed or contains non-string values, writes fail and leave the existing file untouched.

| Key | Values / Default |
| --- | --- |
| `api_key` | DeepSeek API key (overridden at runtime if `DEEPSEEK_API_KEY` is set) |
| `theme` | `"system"` (default), `"light"`, or `"dark"` |
| `language` | `"system"` (default), `"en"`, or `"zh_CN"` (takes effect on restart) |
| `web_enabled` | `"true"` or `"false"` (default `"false"`) |
| `auto_title` | `"true"` (default) or `"false"` |
| `confirm_delete_chat` | `"true"` (default) or `"false"` |
| `default_system_prompt` | Multiline text; empty or absent means no default system prompt |
| `auto_compact` | `"true"` or `"false"` (default `"false"`) |
| `context_threshold` | Estimated input token threshold, `1024`–`1000000` (default `64000`) |
| `context_keep_turns` | Recent turns to keep verbatim during compression, `0`–`100` (default `2`) |
| `message_font`, `code_font` | Font family name; empty or absent uses the default family |
| `message_font_size`, `code_font_size` | Decimal size in logical pixels (`6`–`72`; defaults to `14` for prose and `13` for code) |

## Database schema (version 2)

The schema in [`store.c`](../store.c) defines two SQLite `STRICT` tables.

### `chats`

| Column | Data |
| --- | --- |
| `id`, `title` | Nonempty text primary key and a title that can be empty |
| `created`, `pinned` | Nonnegative Unix time in milliseconds and a `0`/`1` integer |
| `draft`, `draft_images` | Normal draft text and an optional ordered JSON array of image UUIDs |
| `edit_from`, `edit_text`, `edit_images` | Zero-based user-message index, edit draft text, and an optional ordered JSON array of image UUIDs |
| `messages` | Ordered JSON array of message objects |
| `system_prompt` | `NULL` to inherit, `""` to omit, or nonempty text to override |
| `summary`, `summary_until` | Optional summary and the number of original messages it replaces in requests (`NULL` and `0` without a summary) |
| `summary_position` | Number of messages before the visible summary marker; `NULL` without a summary |

- When not editing a message, `edit_from` and `edit_images` are `NULL` and `edit_text` is `""`. When a draft has no images, `draft_images` is `NULL`.
- Each element in `messages` has a required `role` (`"user"` or `"assistant"`) and string `content`.
- Assistant messages may include `reasoning` (text), `duration` (nonnegative seconds), `annotations` (citations), and `server_tool_calls`.
- User messages may include `images` (a nonempty array of attachment UUID strings).

### `session_state`

Holds a single row (`id = 1`) with `current_id` (foreign key to `chats.id`, or `NULL` only when no conversations exist), `model`, and `effort`.

## Lifecycle and transactions

`gchat` keeps a single SQLite connection using the default rollback journal (WAL is not enabled).

- **Locking**: `main.c` acquires `session.db.lock` before opening the database and holds it until shutdown. A second instance exits immediately if the lock is held.
- **Startup and cleanup**: `store_open` enables foreign keys and verifies or migrates the schema before `store_load` reads all conversations into memory. Only after a complete load does the client remove unreferenced UUID files from `attachments/` (checking all messages, normal drafts, and edit drafts across sessions).
- **Incremental saves**: Modified sessions are marked dirty and deleted session IDs are queued. `store_save` runs a single transaction that upserts dirty rows in `chats`, updates `session_state`, and then deletes queued rows from `chats` so foreign-key references remain valid. Dirty flags and the deletion queue are cleared only after the transaction commits.

## Schema migrations

`PRAGMA user_version` tracks the database schema version (currently `2`). Version `0` is accepted only on an empty database; databases with a `user_version` newer than the client supports are rejected without downgrade. The migration from version `1` preserves conversations and starts them with no summary and an inherited system prompt.

To evolve the format:

1. Append a migration step to `upgrade_schema` in [`store.c`](../store.c), where index `n` upgrades version `n` to `n + 1`. Do not alter released steps or add version branches to normal read/write code.
2. Execute all SQL and JSON conversions inside the `BEGIN IMMEDIATE` transaction started by `store_open`. Do not commit, set `user_version`, or delete files in `attachments/` inside a migration step.
3. Add regression fixtures in [`tests/test-store.c`](../tests/test-store.c) covering single-step upgrades, multi-step upgrades, and rollback on failure:

```sh
meson test -C build store --print-errorlogs
```
