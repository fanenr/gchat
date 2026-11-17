# gchat details

**English** | [简体中文](DETAILS.zh-CN.md)

This page covers what [README](../README.md) does not.

## Feature details

### Streaming replies

- The client reads SSE asynchronously with libsoup3 and shows text as it arrives.
- The button becomes a stop button during generation. A click stops at once and keeps the text and reasoning received so far.
- If a record has only reasoning, the client does not send it to the API as an empty reply.
- After layout, streaming text and the Markdown tail follow the bottom.
- When you read upward, the view does not jump back to the end.

### Markdown rendering

- md4c parses the Markdown. CommonMark and part of GFM are supported.
- The client renders Markdown as native widgets.
- The widgets cover headings, bold, italic, strikethrough, inline code, multi-level lists, nested quotes, rules, and tables.
- Lists support task lists and ordered lists. An ordered list can start at 0.
- Tables support column alignment and row separators.
- Links open only HTTP, HTTPS, and mailto.
- Local files, relative links, and other app schemes show as plain text. The client does not launch external programs.

### Code blocks

- A code block shows as a separate card with a language label and a copy button.
- The code area has 8px padding above and below.
- The display drops only one trailing newline. Extra blank lines stay.
- The copy button keeps the full parsed code, including trailing spaces and the final newline.
- md4c normalizes line breaks inside a block to LF.
- GtkSourceView does the highlighting. Available languages depend on the installed version.
- The color scheme follows the light or dark theme and switches between Adwaita and Adwaita-dark.

### Multiple sessions

- The sidebar groups pinned sessions first, then other sessions as "Today / Yesterday / Earlier". Each group is sorted by creation time, newest first.
- The "New chat" button is on the top row. A delete button appears at a session row end on hover. Right-click a session to pin or unpin, copy, or rename it.
- Copying opens a new, unpinned session with independent messages, reasoning, and drafts, including any edit in progress.
- History, the current session, the model, the reasoning level, and each session draft are stored in `~/.local/share/gchat/session.json`.
- History includes reasoning and think time.
- You must confirm a deletion. The default action is cancel.
- A deletion also removes the messages and the draft of that session.
- Sidebar search does a local, case-insensitive literal match on titles and bodies.
- When the body matches, the client shows a nearby excerpt.
- Search does not cover reasoning or drafts.

### Settings

The gear at the bottom of the sidebar opens the settings page. It offers these items:

- API key.
- Theme. The options are follow system, light, and dark. A change applies at once and persists.
- Interface language.
- Version and data file locations.

### Models and reasoning levels

- The models are `deepseek-flash` and `deepseek-v4-pro`.
- The reasoning levels are `none`, `low`, `high`, and `max`.
- `none` turns thinking off. It maps to `thinking.type=disabled`.
- The client merges the model and the reasoning level into one chip at the bottom right of the input box.
- A click on the chip opens a grouped menu.
- Reasoning expands while the model thinks and collapses when the answer starts or generation stops. You can still toggle it manually. Completed reasoning is shown under "Thought (x s)" (Chinese UI: 「已思考（用时 x 秒）」).

### Message actions

The latest message always shows its action row. Other messages show it while hovered or while the message or one of its controls has focus. The row offers these actions:

- Copy the whole message.
- Edit and resend. The client puts the body back in the input box. It withdraws this message and all later messages only when you send.
- Fork into a new session. For a user message, the new session keeps the earlier history and puts the selected message in its draft. For a reply, it keeps the history through that reply, including reasoning. The original session stays unchanged. Forking does not send a request and is unavailable during generation.
- Regenerate. This works only on the last reply.
- Delete one message.

The rules are:

- You must not regenerate during an edit.
- The edit state survives a session switch and a restart.
- Click Cancel, or press `Esc` in the input box, to restore the previous normal draft.
- User messages and replies both render as Markdown. Pasted code blocks, tables, and lists show correctly.
- Sending keeps the indentation and trailing whitespace of the body. The client rejects only blank messages.
- A user bubble shrinks to the width of the body. Padding is fixed at 16px left and right, 10px top and bottom.
- The action row does not widen the background. Multi-line text stays left-aligned.
- You can drag a selection across paragraphs. `Ctrl/Cmd+A` selects the whole message.
- A code block has its own copy button.

### Reliability

- To save a session, the client writes a temporary file and then renames it. The previous copy stays as `.bak`.
- If the main file does not parse, or the session structure is invalid, the client restores from a valid backup.
- Validation covers unique session IDs, message types, and edit targets.
- If both the main file and the backup are unusable, the client stops at startup with a message. It keeps the original files.
- One locked process writes to a data directory at a time.
- On a second start, the new process reports the lock failure and exits, so the two do not overwrite each other.
- When a request fails, the client shows a toast with a Retry button.
- After you switch to another session, an old toast does not retry the current session.
- If you close the window or quit during generation, the client saves the text and reasoning received so far.
- A repeated quit notice does not rotate the backup again.
- If the save at window close fails, the client keeps the window and does not drop the in-memory draft.
- If an app-level quit cannot be stopped, the client reports the save failure and returns a non-zero status.

### Error handling

- HTTP errors and network errors show as a plain-text toast with a summary of the response body.
- The client does not interpret markup in the error text.
- The client reads the error body asynchronously. The read is cancellable and does not block the UI.
- A failure leaves no completely blank message.
- The client reports a failed write of a session or of the configuration.
- If a theme or language save fails, the client restores the previous choice.

### Drafts

- Each session stores a normal draft and an edit draft separately. The edit draft is the body held while you edit a message.
- The client writes to disk 750ms after typing stops.
- The client saves at once on a session switch, on send, and on a normal quit.
- When the client saves a draft during generation, the same snapshot also holds the text and reasoning received so far.
- After an edit and resend, the client restores the previous normal draft. It does not send the draft to the API.
- If a new empty session has a draft, the "New chat" button creates another empty session.

The input box always shows "AI-generated content. Please verify important information."

### Interface language

The client supports English and Simplified Chinese.

- The default is the system language.
- If the system language is not supported, the client falls back to English.
- Simplified Chinese matches `zh_CN`, `zh_SG`, `zh-Hans`, and the generic `zh`.
- Traditional Chinese falls back to English.

Choose System / English / Simplified Chinese in "Settings → Appearance → Language".
The choice is stored in the `language` field of `~/.config/gchat/config.json`.
The value is `system`, `en`, or `zh_CN`. The choice takes effect after a **client restart**.
If the save fails, the client shows a message and restores the previous choice. It does not change the input draft or the generation state.

UI text, action hints, and error hints use gettext.
These items are not translated:

- Message bodies.
- Reasoning.
- Titles of existing sessions.
- Error bodies that DeepSeek returns.

The client also does not force the model to answer in one language through the API.
An empty title of a new session is translated at display time. The client does not write the interface language into a session title.

Translations live in `po/`. The English source is in C and Blueprint files. The Simplified Chinese translation is in `po/zh_CN.po`.
`po/POTFILES` lists these files; Blueprint strings use the same `_()` syntax as C.
The build needs `msgfmt` from gettext.
The compiled catalogs are under `po/` in the build directory. You must keep this directory at run time.
A single copied `gchat` binary will not run.
To update the template and the catalogs:

```sh
meson compile -C build gchat-update-po
```

## Session file format

The root of a session file must have `chats`, `current`, `model`, and `effort`.
Each session must contain `id`, `title`, `draft`, `created`, and `messages`. Optional boolean `pinned` defaults to false.
`role` and `content` of a message are required. `reasoning` and `duration` are optional.
`edit` exists only on an edited message.
`current` must point to an existing session. It is the string `""` for an empty list.
A missing required field makes the file invalid. If the backup is invalid too, the client stops at startup and does not overwrite the original data.

## Local data and security

The default data directory is `~/.local/share/gchat/`. The default configuration directory is `~/.config/gchat/`.
Setting `XDG_DATA_HOME` or `XDG_CONFIG_HOME` changes the matching directory.

- `session.json` holds messages, reasoning, and drafts. `session.json.bak` holds the previous session data.
- `config.json` holds the API key, the theme, and the language. Updates are atomic and create no backup.
- The UI shows the actual configuration file path.
- An existing `config.json.bak` is neither read nor deleted automatically. The key in an old copy is not encrypted, so you must treat the file as sensitive.
- Files are written with `0600` permissions. Directories are created with `0700` permissions. The content is not encrypted.
- If the configuration is not a valid JSON object, or holds a non-string value, a settings save fails and keeps the original file.
- The theme and the language fall back to the last choice saved in the dialog. The client does not read values from the broken configuration.
- `session.json.lock` is the process lock file. The lock is released after an exit or a crash. You must not delete this file to bypass the lock.
- When you send a message, the body history of the current session goes to DeepSeek. Local drafts and reasoning records are not sent back.

## Regression tests

The test code lives in `tests/`, split as follows:

- `test-app.c`: SSE parsing, and lookup of models and reasoning levels.
- `test-store.c`: session storage and backup restore.
- `test-md.c`: Markdown rendering.
- `test-i18n.c`: language matching, English fallback, manual choice, and catalog loading under the C locale.

The app tests run in English and in Simplified Chinese. They check these items:

- Settings text.
- The actual configuration path.
- Version information.
- The fallback when a save fails.
- An unchanged draft and session state after a language switch.

The configuration test verifies that a merged write creates no key backup.
The Chinese search input test uses a separate IM module to replay the commit sequence. It checks these items:

- Preedit notifications.
- Prefix, selection, and mid-text insertion.
- The actual cursor layout.
- Search results.
- The lifetime of the input method context.

A separate check validates the format of the translation files.

The regression tests also cover the following areas:

- Session structure validation and backup protection.
- Duplicate IDs and invalid message roles.
- The process lock.
- Protection of a broken configuration.
- Theme and language fallback on a failed save.
- App quit and window-close save failures.
- A repeated quit.
- Snapshots during generation and stale request callbacks.
- A stale retry.
- Plain-text display of an error body.
- Edit protection.
- Markdown inline style scope and language aliases.
- Reasoning storage and request filtering.
- A slow HTTP error body and cancellation.
- SSE field types and CRLF.
- A list number on the same line as the first paragraph.
- An ordered list that starts at 0.
- Multiple paragraphs and nested lists.
- Markdown scope across widgets.
- Trailing whitespace of messages and code blocks.
- Link scheme filtering and entity decoding.
- Release of the code block buffer.

Code block content tests check display and copy from Markdown input. They cover fenced blocks, indented code, raw HTML blocks, CRLF, an unclosed fence, an empty block, and extra blank lines.
Layout tests check the bottom spacing and the visibility of the last line under the light and dark themes. They cover single-line, multi-line, and wrapped long lines.
The layout cases show a separate test window. They check bubble wrapping, fixed padding, and the centering of a short sentence.
A case also rebuilds a user bubble in a shown window. It checks that the height does not grow from an old measurement cache.
The local streaming case clicks the real Regenerate button. It checks the bubble layout during generation, at the end, and after a history re-render.
The tests also check the visible end after upward reading and after input box changes. They do not check scrolling values only.
The tests use only temporary directories and a local HTTP service. They do not contact DeepSeek and do not read user sessions or keys.

GTK tests need a display backend. They skip when no backend is available.

## Code structure

| File | Responsibility |
| --- | --- |
| `meson.build` | Project version, build directory configuration, direct dependencies of the app and of each test. The version also feeds the About page and the User-Agent |
| `main.c` | Window layout, sidebar and message area, settings panel, streaming requests and cancellation, session snapshots, and the quit lifecycle |
| `ui/` | Blueprint layouts, CSS, and the GResource manifest. The layouts compile to GtkBuilder XML and are embedded in the app |
| `i18n.c` / `i18n.h` | Language matching and gettext setup at startup |
| `po/` | gettext template, Simplified Chinese catalog, and build rules |
| `md.c` / `md.h` | md4c callbacks to GTK widgets. Body text and tables share inline styles and text decoding |
| `store.c` / `store.h` | Session validation, process lock, atomic replace and backup restore, configuration validation and merged writes |
| `tests/` | Regression tests for session storage, app state and requests, and Markdown rendering |

## Known limits

- A macOS IME can clear the preedit text before the commit and notify GTK only after it. The search box syncs the cleared preedit state before the commit. The state and commit hooks die with the matching input method context. The client does not disable the IME.
- Images do not load. The client shows `[alt text]` only.
- Formulas (`$…$`) show as source text.
- Images and code blocks inside a table cell do not render. The client keeps only the inline styles.
- Code highlighting looks up a GtkSourceView language ID and explicit aliases, for example `javascript` to `js` and `csharp` to `c-sharp`.
- An unknown language name shows as plain text. The client does not do fuzzy matching.
- A selection cannot cross messages. A GTK selection range is limited to one widget. Code blocks and tables are separate widgets, so a drag selection breaks there.
- The client reads SSE as single-line `data:` JSON events from DeepSeek. It supports LF, CRLF, and a missing space after the colon.
- Multi-line `data:` aggregation is not implemented. You must not use this code as a general SSE client.
- The client decodes only common named entities and numeric entities. A named entity outside the table stays as source text.
- Draft autosave has a 750ms delay. A forced process kill loses input not yet written and the current streaming reply. `.bak` keeps only the previous copy and is not a separate backup. The temporary-file replace does not guarantee durability after a sudden power loss.
- Image input is not connected yet. `deepseek-flash` supports image input.
