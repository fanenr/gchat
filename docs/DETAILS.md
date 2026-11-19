# gchat details

**English** | [简体中文](DETAILS.zh-CN.md)

This page covers what [README](../README.md) does not.

## Feature details

### Conversation layout

- The message and input areas stay aligned as the window is resized.
- The window requests a minimum size of 640 × 480 logical pixels. The platform or content may require a larger size.
- The content uses 85% of the available width up to 760 logical pixels and 75% from 1200 logical pixels. The proportion changes smoothly in between. Both sides keep at least 16 logical pixels of margin; very narrow windows prioritize the margins.

### Streaming replies

- The client reads SSE asynchronously with libsoup3 and shows text as it arrives. It joins multiple `data` lines with newlines and processes each event at the blank line.
- The button becomes a stop button during generation. A click stops at once and keeps the text and reasoning received so far.
- If a record has only reasoning, the client does not send it to the API as an empty reply.
- After layout, streaming text and the Markdown tail follow the bottom.
- When you read upward, the view does not jump back to the end.

### Web search and link reading

- The **Web** toggle sits next to the model and reasoning selector. It is available only with `deepseek-flash` and starts off each time you launch the client. The setting applies across chats and is remembered if you switch away from Flash and back.
- When the toggle is on, DeepSeek decides whether to search or read links. With thinking enabled, it can search over multiple steps. These server-side tools have no separate charge; model token charges still apply.
- While waiting for the answer, the client shows search or link-reading progress. The reply lists cited HTTP and HTTPS sources once per URL. The source list does not load site icons or open URLs automatically.
- In later Flash turns, the client resends the server tool records with chat history, even with **Web** off. This lets DeepSeek restore pages it previously read from user links. Raw search results are not restored.
- If generation stops before the final event, the client keeps the received text and reasoning but has no final sources or tool records to save.

### Markdown rendering

- md4c parses the Markdown. CommonMark and part of GFM are supported.
- The client renders Markdown as native widgets. Message prose defaults to a 14px font; the sidebar and input box keep their 14px font. Prose uses 1.3 times the selected font’s natural line height. Table text and streaming replies use the same line-height rule.
- The widgets cover headings, bold, italic, strikethrough, inline code, multi-level lists, nested quotes, rules, and tables. Bold prose uses weight 600. Headings 1–3 use 1.5, 1.375, and 1.25 times the prose size with weight 700; headings 4–6 keep the prose size with weight 600. Inline emphasis does not override a heading’s size or weight.
- Paragraph gaps are 16 logical pixels. Headings 1–3 have a 24-pixel gap before them; headings 4–6 have 16. List items have 6-pixel gaps, later paragraphs within an item have 8, and nested lists have 4. A top-level list has a 16-pixel gap before it, or 8 after a heading.
- Lists support task lists and ordered lists. An ordered list can start at 0. Wrapped lines and later paragraphs align with the text after the marker. Marker widths are measured, so multi-digit numbers and checkboxes keep their alignment.
- Tables support column alignment, row separators, and inline code. They use a transparent background without an enclosing border. Cells have 16-pixel horizontal and 10-pixel vertical padding. Headers use weight 500 at full text contrast; bold cell text uses weight 600.
- HTTPS images outside tables load automatically. Image servers can see your IP address and request time. Private-network HTTPS addresses are not filtered. Loading starts after a streaming reply finishes; history is loaded again when rendered.
- Images use a separate request without API credentials. PNG, JPEG, GIF, and WebP are supported when the system decoder is available. GIF and WebP are displayed as static images.
- Images are displayed at no more than 320 × 240 logical pixels, preserving aspect ratio without enlarging small images. The image can set a minimum width for the message, so very narrow layouts may not shrink to fit it.
- Image requests do not follow redirects. They time out after 20 seconds and are limited to 8 MiB and 16 million decoded pixels. Failed or unsupported images show `[alt text]`; empty alt text shows `[Image]`.
- Links open only HTTP, HTTPS, and mailto.
- Local files, relative links, and other app schemes show as plain text. The client does not launch external programs.

### Image input

- Use the attachment button to select one or more local images. You can also paste image pixels, a screenshot, or multiple copied local image files into the input box. Each import appends to the current draft.
- Review attachments in the thumbnail strip inside the input box. Scroll horizontally if needed, and use the close button on a thumbnail to remove just that image.
- If the clipboard contains both an image and text, the image takes priority. Plain text still pastes normally. Pasted image pixels are converted to PNG and may lose metadata; pasted image files are stored unchanged.
- You can send an image without text. Images appear in messages. Normal and edit drafts keep their own images.
- PNG, JPEG, GIF, and WebP work when their system decoders are available. The format is detected from the contents. GIF and WebP previews are static.
- Each image is limited to 8 MiB, 8192 pixels per side, and 16 million pixels total. For pasted image pixels, the size is measured after PNG encoding; GTK may decode the image before the client checks its dimensions. A request can contain up to 600 images. With 15 or more images, each side of every image is limited to 4096 pixels. These limits include history. The full request body is limited to 48 MiB, including image history and Base64 overhead.
- Images are sent again with the history on each request. This can increase upload time and cost. Selected files are sent unchanged, including any metadata.
- Images require `deepseek-flash`. If the current message or history contains an image, switch back to Flash before sending or regenerating.
- Wait for the current batch to finish before adding more images or sending. Switching sessions, starting an edit, or canceling an edit cancels the current import. If any file in a batch cannot be imported, none of that batch is added. On a missing image or request limit error, the client reports the error without committing the draft.
- Image copies are stored in the data directory under `attachments/`, with `0600` permissions and no encryption. Editing, copying, and forking retain image references.
- After a successful database load at startup, the client removes unreferenced image files from `attachments/`. It does not scan the old `images/` directory.
- Dragging images is not supported. Request construction and history previews read images on the UI thread, so long conversations can briefly pause the UI.

### Code blocks

- A code block shows as a separate card with a language label and a copy button. Their header row has 12px of space above it. The language label aligns with the code’s left edge, 16px inside the card.
- The code area has 16px padding on all sides and uses 1.6 times the selected font’s natural line height. Padding and line-height rules are fixed; the font family and size remain configurable.
- The card has a subtle background and border, without a divider beneath its language label. The language label and copy button use a secondary color; the button returns to full contrast on hover or keyboard focus.
- The display drops only one trailing newline. Extra blank lines stay.
- The copy button keeps the full parsed code, including trailing spaces and the final newline.
- md4c normalizes line breaks inside a block to LF.
- GtkSourceView does the highlighting. Available languages depend on the installed version.
- The color scheme follows the light or dark theme and switches between Adwaita and Adwaita-dark.

### Multiple sessions

- The sidebar groups pinned sessions first, then other sessions as "Today / Yesterday / Earlier". Each group is sorted by creation time, newest first.
- The "New chat" button is on the top row. A delete button appears at a session row end on hover. Right-click a session to pin or unpin, copy, or rename it.
- Copying opens a new, unpinned session with independent messages, reasoning, and drafts, including any edit in progress.
- History, the current session, the model, the reasoning level, and each session draft are stored in `~/.local/share/gchat/session.db`.
- History includes reasoning and think time.
- **Settings → Conversation → Confirm conversation deletion** is on by default. The confirmation dialog defaults to Cancel. If you turn the switch off, clicking the delete button deletes the session immediately. The change takes effect at once and persists.
- A deletion also removes the messages and the draft of that session.
- Sidebar search does a local, case-insensitive literal match on titles and bodies.
- When the body matches, the client shows a nearby excerpt.
- Search does not cover reasoning or drafts.

### Automatic conversation titles

- **Settings → Conversation → Automatic conversation titles** is on by default. Changes are saved and apply to requests sent after the change.
- With the switch on, only the first send in an untitled `deepseek-flash` conversation requests a title. Existing titles, edited messages, and regenerated replies do not request a new title.
- The client first uses up to 30 characters from the first line of the user’s text. An image without text leaves the title empty. If the stream finishes normally with a usable title, the client replaces the local title and saves it. If the title is missing or the stream is stopped or interrupted, the local title stays.
- DeepSeek does not charge separately for the title. The answer streams as usual; after the answer, DeepSeek can wait up to five more seconds for the title. The stop button remains available.

### Settings

The gear at the bottom of the sidebar opens the settings page. It offers these items:

- API key.
- Theme. The options are follow system, light, and dark. A change applies at once and persists.
- Interface language.
- Reading. Message and code block font families and sizes are independent. Font pickers list installed fonts. Font and size reset buttons appear only when their own value differs from the application default; each restores only its own setting. Sizes range from 6 to 72 logical pixels. The defaults are 14px for prose and 13px for code. Changes apply immediately and persist.
- Version and data file locations.

Reading settings leave the sidebar, input box, and reasoning text unchanged. Inline code keeps its monospace family and scales with prose. Table text and streaming replies follow the prose font. Font changes keep message selections, drafts, and active requests. List margins and user-bubble widths are recalculated without replacing widgets or reloading images. On a save failure, the control returns to its previous value. Missing or invalid font settings use the defaults; unavailable fonts fall back through GTK.

### Models and reasoning levels

- The models are `deepseek-flash` and `deepseek-v4-pro`.
- The reasoning levels are `none`, `low`, `high`, and `max`.
- `none` turns thinking off. It maps to `thinking.type=disabled`.
- The client merges the model and the reasoning level into one chip at the bottom right of the input box.
- A click on the chip opens a grouped menu.
- Reasoning expands while the model thinks and collapses when the answer starts or generation stops. You can still toggle it manually. Completed reasoning is shown under "Thought (x s)" (Chinese UI: 「已思考（用时 x 秒）」).

### Message actions

The latest message always shows its action row. Other messages show it while hovered or while the message or one of its controls has focus. Visible buttons use a secondary color and return to full contrast on hover or keyboard focus. The row offers these actions:

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
- Sending keeps the indentation and trailing whitespace of the body. Without an image, the client rejects blank messages.
- A user bubble shrinks to the width of the body. Padding is fixed at 16px left and right, 10px top and bottom.
- The action row does not widen the background. Multi-line text stays left-aligned.
- You can drag a selection across paragraphs. `Ctrl/Cmd+A` selects the whole message. The selection background follows the text lines and excludes the extra spacing between paragraphs, headings, and list items.
- A code block has its own copy button.

### Reliability

- A SQLite transaction saves changed sessions, deletions, and the current session state together.
- Validation covers unique session IDs, message types, attachment IDs, and edit targets.
- If the database cannot be loaded, the client reports the error and stops. It does not recreate the database or restore a separate backup.
- One locked process writes to a data directory at a time.
- On a second start, the new process reports the lock failure and exits, so the two do not overwrite each other.
- When a request fails, the client shows a toast with a Retry button.
- After you switch to another session, an old toast does not retry the current session.
- If you close the window or quit during generation, the client saves the text and reasoning received so far.
- A repeated quit notice does not save again.
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
- When the client saves a draft during generation, the same transaction also saves the text and reasoning received so far.
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

## Storage

See [gchat storage](STORAGE.md) for file locations, backups, the database format, and upgrades.

When you send a message, the text and image history of the current session goes to DeepSeek. Local drafts and reasoning records are not sent back.

## Regression tests

The test code lives in `tests/`, split as follows:

- `test-app.c`: SSE parsing, and lookup of models and reasoning levels.
- `test-store.c`: SQLite storage, upgrades, and configuration.
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

- Database initialization, reopen, and refusal of unsupported versions.
- Incremental writes, rollback on upgrade or save failure, and image cleanup.
- Session structure validation, including invalid message roles and edit targets.
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
- SSE field types, multi-line events, CRLF, and incomplete events.
- The Web toggle, streamed tool progress, citation rendering, and replay of saved tool records.
- The automatic title switch, first-request eligibility, fallback, and title persistence.
- A list number on the same line as the first paragraph.
- An ordered list that starts at 0.
- Multiple paragraphs and nested lists.
- Prose font size, proportional prose and code line heights at default and doubled sizes, and hanging list indents in both themes, including multi-digit numbers, task lists, and later paragraphs.
- All six heading levels, heading precedence over inline emphasis, paragraph/list spacing, and code/table padding.
- Selection snapshots leave paragraph spacing unpainted in both themes at default and doubled prose sizes, including wrapped text, explicit line breaks, headings, task lists, cross-paragraph selections, and right-to-left text.
- Independent message and code fonts, saved sizes, conditional font and size reset buttons, save-failure rollback, invalid-value fallback, and font-family CSS escaping.
- Default-font comparison ignores case and does not depend on whether a value is explicitly saved. Resetting one setting leaves the other unchanged.
- Font changes that preserve selections and active requests, and update list margins across code cards without replacing text buffers.
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
| `main.c` | Window layout, sidebar and message area, settings panel, streaming requests and cancellation, session saves, and the quit lifecycle |
| `ui/` | Blueprint layouts, CSS, and the GResource manifest. The layouts compile to GtkBuilder XML and are embedded in the app |
| `i18n.c` / `i18n.h` | Language matching and gettext setup at startup |
| `po/` | gettext template, Simplified Chinese catalog, and build rules |
| `md.c` / `md.h` | md4c callbacks to GTK widgets. Body text and tables share inline styles and text decoding |
| `store.c` / `store.h` | SQLite session validation, upgrades and transactions, process lock, configuration validation and merged writes |
| `tests/` | Regression tests for session storage, app state and requests, and Markdown rendering |

## Known limits

- A macOS IME can clear the preedit text before the commit and notify GTK only after it. The search box syncs the cleared preedit state before the commit. The state and commit hooks die with the matching input method context. The client does not disable the IME.
- Table images show `[alt text]`. Table cells support inline code, but not multi-line code blocks.
- Formulas (`$…$`) show as source text.
- Code highlighting looks up a GtkSourceView language ID and explicit aliases, for example `javascript` to `js` and `csharp` to `c-sharp`.
- An unknown language name shows as plain text. The client does not do fuzzy matching.
- A selection cannot cross messages. A GTK selection range is limited to one widget. Code blocks and tables are separate widgets, so a drag selection breaks there.
- The client expects DeepSeek JSON data and the `[DONE]` marker in SSE events. It ignores invalid JSON and does not process `event`, `id`, or `retry` fields. An event without its final blank line is discarded. This is not a general SSE client.
- The client decodes only common named entities and numeric entities. A named entity outside the table stays as source text.
- Draft autosave has a 750ms delay. A forced process kill loses input or streaming output not yet committed. SQLite transactions are not a separate backup.
