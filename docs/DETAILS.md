# gchat details

**English** | [简体中文](DETAILS.zh-CN.md)

This document describes the code structure, core mechanisms, packaging, tests, and known limits of `gchat`. For file paths, database schema, and migrations, see [Storage](STORAGE.md).

## Code structure

| Path | Responsibility |
| --- | --- |
| `meson.build` | Project version, build targets, and dependencies |
| `main.c` | Window, sidebar, message list, settings dialog, HTTP/SSE lifecycle, and quit flow |
| `md.c` / `md.h` | `md4c` Markdown parser, native GTK4 widget renderer, code cards, tables, and remote image loader |
| `store.c` / `store.h` | SQLite session storage, schema migrations, process lock, and atomic `config.json` writes |
| `i18n.c` / `i18n.h` | Startup locale matching and `gettext` setup |
| `macos-bundle.c` / `macos-bundle.h` | Resource, locale, icon, and TLS trust store setup inside a macOS `.app` bundle |
| `ui/` | Blueprint UI definitions, CSS styles, and `GResource` manifest |
| `po/` | `gettext` template (`POTFILES`) and Simplified Chinese catalog (`zh_CN.po`) |
| `packaging/` | macOS DMG assembly script and Linux Flatpak manifest and metadata |
| `tests/` | Regression suites (`test-app.c`, `test-store.c`, `test-md.c`, `test-i18n.c`) |

## Core mechanisms

### Requests and streaming

- **SSE streaming**: `main.c` reads SSE streams asynchronously over `libsoup3`. It joins multi-line `data:` fields and dispatches each event on a blank line. Text and reasoning deltas coalesce and flush to the UI at most once per GTK frame clock tick.
- **History payload**: Each request sends the effective system prompt, any summary, and all uncovered user and assistant messages (including images) in order. Without a summary, `gchat` sends the full history up to the current turn. Requests exclude unsent drafts and saved `reasoning_content`.
- **User metadata**: This setting is off by default. When enabled, `gchat` stores a `latest_reminder` string with each new user message and sends it before that message. The string contains the local date, weekday, `App`, and active interface language, with an empty location. When disabled, `gchat` omits all `latest_reminder` entries. Older messages without `latest_reminder` stay empty, including when regenerated.
- **Web search and page reading**: Available on `deepseek-flash` only. When enabled, DeepSeek can run server-side search and page-reading tools. During streaming, `gchat` shows tool progress and renders deduplicated HTTP/HTTPS citations below the reply. Subsequent `deepseek-flash` requests replay `server_tool_calls` to keep previously read pages in context.
- **Automatic titles**: On the first turn of an untitled `deepseek-flash` session, `gchat` sets a provisional title from the first line of user text. If the stream finishes with a server-generated title, `gchat` replaces the provisional title.

### Context management

- **System prompt**: A session inherits the default system prompt unless it overrides or omits it. If the effective prompt is nonempty, `gchat` sends it as the first `system` message, including on regeneration.
- **Compression scope**: Manual and automatic compression keep the configured number of recent user turns verbatim (default 2). During automatic compression, the pending new message does not count toward kept turns and is sent as written. Changing this setting does not alter existing summaries.
- **Summary request**: `gchat` sends a separate non-streaming `deepseek-flash` request to compress older messages, attached images, and any prior summary. This request disables reasoning and web tools. It excludes the session's system prompt, saved reasoning, and `server_tool_calls`.
- **Storage and replay**: `gchat` stores the summary text and the count of original messages it covers. Later requests replay the summary as historical context, not as a system prompt. Original messages and attachments stay intact. A forked session inherits the summary only if the fork includes all covered messages.
- **Marker and read-only boundary**: The collapsed **Context compressed** marker records where compression happened, independent of the covered message count. Manual compression appends the marker and scrolls to the bottom. Automatic compression inserts the marker before the pending new message. Click the marker to show or hide the summary. All messages before the marker are read-only, even if the summary does not cover them: `gchat` hides edit, delete, and regenerate actions and blocks retries before the marker. Copy and fork remain available.
- **Automatic trigger and calibration**: Automatic compression is off by default. When enabled, `gchat` compares the estimated input tokens with the configured threshold before sending. Streaming chat requests set `stream_options.include_usage`. When DeepSeek returns `usage.prompt_tokens`, `gchat` calibrates in-memory estimates using that session's latest reading for the same model. Calibration is disabled in edit mode and resets on model change until the new model returns usage. The threshold is not a model context limit.
- **Context ring**: The ring and popover estimate the next request size, covering the effective system prompt, summary, active history, current draft, and attached images (1,024 tokens per image). In edit mode, they count only the history before the edited message. The ring compares against the compression threshold even when automatic compression is off. The arc caps at 100%, while the percentage label can exceed 100%. The ring turns orange at 80% and red at 100%.
- **Failure and cancellation**: If compression fails or is cancelled, `gchat` keeps the prior summary, history, and draft unchanged. Each send attempts compression at most once. The final chat request remains subject to request-size and model limits.

### Markdown and media

- **Native widget tree**: `md.c` parses CommonMark and GFM extensions via `md4c` into native GTK4 widgets (`GtkTextView` for prose, `GtkGrid` for tables, and `GtkSourceView` inside cards for code blocks).
- **Remote images**: `gchat` fetches HTTPS images outside tables asynchronously without API credentials and without following redirects. External links open only `http`, `https`, and `mailto` URIs.
- **Image input**: On `deepseek-flash`, users can attach PNG, JPEG, GIF, and WebP images via the file picker or clipboard paste. `gchat` copies accepted files into `attachments/` by UUID and removes unreferenced files after a successful database load at startup.

| Scope | Limits |
| --- | --- |
| Remote image | 20 s total timeout, 8 MiB max size, 16 megapixels max, displayed within 320 × 240 logical pixels |
| Single uploaded image | 8 MiB max size, 8,192 px per side (4,096 px when a request carries 15 or more images), 16 megapixels max |
| Single request payload | 600 images max, 48 MiB body max including Base64 image history |

### Sessions and drafts

- Each session stores its own message list, reasoning history, normal draft, and edit draft.
- Typing triggers a debounced save after 750 ms. Switching sessions, sending a message, or closing the window flushes changes immediately in one SQLite transaction.
- Editing or forking a message preserves image references. `gchat` leaves prior history unchanged until the edited message is sent.
- The message navigator lists the user messages currently shown in the session in order. Search matches the full text of each message. Jumping to an earlier turn pauses auto-follow; **Jump to latest** resumes it.

### Localization

- `gchat` supports English and Simplified Chinese (`system`, `en`, or `zh_CN` in `config.json`, applied on restart).
- `i18n.c` maps `zh_CN`, `zh_SG`, `zh-Hans`, and generic `zh` to Simplified Chinese. Other locales, including Traditional Chinese, fall back to English.
- After changing UI strings, refresh the translation catalog:

  ```sh
  meson compile -C build gchat-update-po
  ```

## Packaging

### macOS app bundle

The `macos-arm64` target assembles `gchat.app` in a staging directory and copies non-system dylibs plus GIO and gdk-pixbuf modules into `Contents/Frameworks`. It rewrites Mach-O load commands to bundle-relative paths and verifies dependency closure before signing. The minimum macOS deployment target equals the highest target across the binary and bundled libraries.

### Flatpak

The [Flatpak manifest](../packaging/flatpak/io.github.fanenr.gchat.yml) builds against GNOME SDK 51 with `-Dflatpak=true` and prefix `/app`, compiling `md4c` from a checksummed archive. The sandbox allows network access, Wayland/X11 display, desktop portals for file picking and URI opening, and host paths `xdg-data/gchat` and `xdg-config/gchat`. Flatpak and native builds therefore share the same database and configuration.

## Tests

The regression suite in `tests/` runs against temporary directories with no external network access:

- `test-app.c`: SSE framing, streaming lifecycle, request payload generation, web search, auto-title, image validation, and session state transitions.
- `test-store.c`: SQLite initialization, migrations, transactional saves, process lock, attachment cleanup, and `config.json` handling.
- `test-md.c`: Markdown widget rendering, code blocks, table markup, link filtering, and image helpers.
- `test-i18n.c`: Locale resolution, fallback rules, and catalog loading.

## Known limits

- Table cells support inline styles and inline code, not multi-line code blocks or embedded images (which render as `[alt text]`).
- LaTeX math (`$…$`) displays as raw text.
- Text selection stays within one GTK text widget and cannot span code blocks, tables, or multiple messages.
- Syntax highlighting matches `GtkSourceView` language IDs and a fixed alias map (such as `javascript` → `js` and `csharp` → `c-sharp`). Unknown languages render as plain text.
- The SSE parser targets DeepSeek's JSON stream and `[DONE]` sentinel rather than arbitrary SSE streams.
- HTML entity decoding covers numeric entities and a fixed table of common named entities.
- Drag-and-drop image import is not implemented. Encoding large image histories runs on the UI thread.
- Because draft autosave uses a 750 ms debounce, a crash or `SIGKILL` loses uncommitted edits and in-flight stream output.
