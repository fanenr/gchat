# gchat storage

**English** | [简体中文](STORAGE.zh-CN.md)

SQLite stores conversations. Image bytes stay in the data directory, and
configuration stays separate, so a conversation backup does not include the
API key saved in settings.

## Files and backups

| File | Default location | Contents |
| --- | --- | --- |
| `session.db` | `~/.local/share/gchat/` | Conversations, messages, drafts, and selected session, model, and reasoning level |
| `attachments/` | Beside `session.db` | Image files named by UUID |
| `session.db.lock` | Beside `session.db` | Process lock, not conversation data |
| `config.json` | `~/.config/gchat/` | API key, theme, language, and reading fonts |

`XDG_DATA_HOME` and `XDG_CONFIG_HOME` override the corresponding base
directories. Settings show the actual database path and the API key source.
New files are created with `0600` permissions and new directories with `0700`.
The data is not encrypted.
`DEEPSEEK_API_KEY` overrides the key in `config.json`.

To back up conversations, quit gchat normally and copy `session.db` together
with `attachments/`. Keep the copies together. A transaction is not a backup;
gchat does not keep or restore a separate database backup automatically. It
neither imports nor deletes the old `session.json`, `session.json.bak`, or
`images/`.

Settings are written by atomically replacing `config.json`, without creating a
backup. If the file is not a valid JSON object or contains a non-string value,
the save fails and leaves it in place. A failed theme, language, font, or font-size
save restores the previous choice in the dialog. Treat any existing
`config.json.bak` as sensitive: it may contain a plaintext key.

Reading settings use string keys `message_font`, `message_font_size`,
`code_font`, and `code_font_size`. Font values are family names; empty or missing
values use the default family. Sizes are locale-independent decimal strings in
logical pixels. Missing or invalid sizes use 14 for prose and 13 for code.

## Database format: version 1

The schema in [`store.c`](../store.c) uses two SQLite `STRICT` tables. Table
`chats` stores one row per conversation:

| Column | Data |
| --- | --- |
| `id`, `title` | Nonempty text primary key and a title that can be empty |
| `created`, `pinned` | Nonnegative Unix time in milliseconds and a `0`/`1` integer |
| `draft`, `draft_images` | Normal draft text and an optional ordered JSON array of image UUIDs |
| `edit_from`, `edit_text`, `edit_images` | Zero-based user-message index, edit draft text, and an optional ordered JSON array of image UUIDs |
| `messages` | Ordered JSON array of message objects |

When not editing, `edit_from` and `edit_images` are SQL `NULL` and
`edit_text` is empty. An absent `draft_images` is also SQL `NULL`.
Every message requires a `role` (`user` or `assistant`) and text `content`.
Text `reasoning` and a nonnegative numeric `duration` in seconds are optional.
Only user messages can carry `images`. Any stored image list must be nonempty;
each entry is a UUID, not a file path.

Table `session_state` holds one row with `id = 1`: `current_id` references a
row in `chats`, and `model` and `effort` are text. `current_id` is SQL
`NULL` only when there are no conversations.

Saving a draft does not rewrite other conversations. It still serializes the
changed conversation's message array. Startup loads all conversations into
memory; there is no message-level index or on-demand loading.

## Startup and saves

The app keeps one SQLite connection and uses the default rollback journal.
It does not enable WAL.

- [`main.c`](../main.c) acquires `session.db.lock` before opening the database
  and holds it until the connection closes. A second instance using the same
  database exits if it cannot acquire the lock. Do not delete the lock file
  to bypass it.
- `store_open` enables foreign keys and checks or upgrades the schema before
  `store_load` reads it. A database that cannot be opened or validated stops
  startup. Only after a complete load does the client remove unreferenced UUID
  files from `attachments/`. It checks references from both drafts and all
  messages, so deleting one conversation does not remove a shared attachment.
- Changes mark a conversation dirty; deleted IDs are queued. `store_save`
  starts a transaction, inserts or updates dirty conversations, saves changed
  session state, then deletes queued rows. Updating the state before deletion
  keeps its current-session reference valid. Dirty flags and the queue are
  cleared only after commit, so a failed save can be retried.
- Draft text autosaves 750 ms after typing stops. Switching sessions, sending,
  or closing the window also saves. Streaming text is not written per token;
  a save during generation includes the text and reasoning received so far.
  If saving fails on window close, the client keeps the window open and the
  draft in memory.

## Changing the format

`PRAGMA user_version` records the **database format version**, not the app
version. Currently the only upgrade is `0 → 1`. Version `0` is accepted only
when the database contains no user objects. A database newer than the app is
rejected without downgrade.

1. When existing databases need a schema or data conversion, append one step
   to `upgrade_schema` in [`store.c`](../store.c). The entry at index `n`
   transforms version `n` into `n + 1`; the number of entries is the current
   version. This also applies to incompatible changes to stored message JSON.
2. Keep each released step intact. Put SQL and any needed data conversion in
   the upgrade step, not a version check in normal read or write code. Normal
   code reads and writes the current format only.
3. Keep steps inside the transaction started by `store_open` with
   `BEGIN IMMEDIATE`. Do not commit or set `user_version` in a step. The
   runner sets the final version and commits after all pending steps succeed.
   Failure rolls back the transaction and stops startup. Do not move or
   delete attachment files in a database upgrade; SQLite cannot roll them back.
4. Add a fixture for the previous format. Verify the resulting rows and
   attachment references, not just the version. When there is more than one
   upgrade step, also test skipping an app release. Test a failure and verify
   that the database remains at its original format and version.

[Storage tests](../tests/test-store.c) cover fresh creation and reopen,
incremental writes, save and upgrade rollback, invalid and newer databases,
attachment cleanup, and configuration. Run them with:

```sh
meson test -C build store --print-errorlogs
```
