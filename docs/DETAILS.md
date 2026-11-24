# gchat details

**English** | [简体中文](DETAILS.zh-CN.md)

This document describes the codebase structure, core runtime mechanisms, packaging, and known limitations of `gchat`. For file locations, database schema, and migrations, see [Storage](STORAGE.md).

## Code structure

| Path | Responsibility |
| --- | --- |
| `meson.build` | Project version, build targets, and dependencies |
| `main.c` | Application window, sidebar, message list, settings dialog, HTTP/SSE lifecycle, and quit handling |
| `md.c` / `md.h` | Markdown parser (`md4c`) to native GTK4 widget builder, code cards, tables, and remote image loader |
| `store.c` / `store.h` | SQLite session storage, schema migrations, process lock, and atomic `config.json` writes |
| `i18n.c` / `i18n.h` | Locale matching and `gettext` initialization at startup |
| `macos-bundle.c` / `macos-bundle.h` | Resource, locale, icon, and TLS trust store setup inside a macOS `.app` bundle |
| `ui/` | Blueprint UI definitions, CSS styles, and `GResource` manifest |
| `po/` | `gettext` template (`POTFILES`) and Simplified Chinese catalog (`zh_CN.po`) |
| `packaging/` | macOS DMG assembly scripts and Linux Flatpak manifest / metadata |
| `tests/` | Regression test suites (`test-app.c`, `test-store.c`, `test-md.c`, `test-i18n.c`) |

## Core mechanisms

### Requests and streaming

- **SSE pipeline**: `main.c` streams responses asynchronously over `libsoup3`, joining multi-line `data:` fields and dispatching events on blank lines. Incoming text and reasoning deltas are coalesced and flushed to the UI at most once per GTK frame clock tick.
- **History payload**: Sending a message transmits the conversation's user and assistant messages (plus any attached images) to DeepSeek. Local drafts and saved reasoning blocks (`reasoning_content`) are not sent back as message content.
- **User metadata**: The optional, default-off setting adds a `latest_reminder` before each newly sent user message with the local date and weekday, `App`, and the active interface language. Location is empty. The marker is stored with the message and replayed unchanged when the setting is enabled; disabling it suppresses all markers. Older messages without markers are not backfilled, including when regenerated.
- **Web search and link reading**: Available on `deepseek-flash`. When enabled, DeepSeek can invoke server-side search and page-reading tools. The client displays tool progress during streaming, renders deduplicated HTTP/HTTPS source citations below the reply, and replays `server_tool_calls` in subsequent Flash turns so previously read user links remain in context.
- **Automatic titles**: On the first turn of an untitled `deepseek-flash` session, the client sets a provisional title from the first line of user text and replaces it if the stream finishes with a server-generated title.

### Markdown and media

- **Native widget tree**: `md.c` parses CommonMark and GFM extensions via `md4c` into native GTK widgets (`GtkTextView` for prose, `GtkGrid` for tables, and `GtkSourceView` inside custom cards for syntax-highlighted code blocks).
- **Remote images**: HTTPS images outside tables are fetched asynchronously without API credentials. Requests do not follow redirects, time out after 20 seconds, and enforce limits of 8 MiB and 16 megapixels (displayed within 320 × 240 logical pixels). Only `http`, `https`, and `mailto` links are opened externally.
- **Image input**: `deepseek-flash` supports attaching PNG, JPEG, GIF, and WebP images via the file picker or clipboard paste. Each image is capped at 8 MiB, 8192 pixels per side (4096 pixels when a request carries 15 or more images), and 16 megapixels; a single request supports up to 600 images and a 48 MiB body including Base64 history. Accepted files are copied to `attachments/` by UUID and unreferenced files are cleaned up after a successful database load at startup.

### Sessions and drafts

- Each session maintains its own message list, reasoning history, normal draft, and edit draft (used when editing a prior user message before resending).
- Typing triggers a debounced save after 750 ms; switching sessions, sending a message, or closing the window flushes changes immediately in a single SQLite transaction.
- Editing or forking a message preserves image references without modifying existing history until the edited message is sent.
- The message navigator lists the user messages currently shown in the transcript, in order. Its search matches the full text of each user message, not just the preview. Jumping to an earlier message pauses auto-follow; **Jump to latest** restores it.
- If a search in the message navigator finds no results, the list shows **No matching messages**.

### Localization

- The interface supports English and Simplified Chinese (`system`, `en`, or `zh_CN` in `config.json`, applied on restart).
- `i18n.c` maps `zh_CN`, `zh_SG`, `zh-Hans`, and generic `zh` to Simplified Chinese; other locales (including Traditional Chinese) fall back to English.
- To refresh translation catalogs after changing UI strings:

  ```sh
  meson compile -C build gchat-update-po
  ```

## Packaging

### macOS app bundle

The `macos-arm64` build target assembles `gchat.app` in a staging directory, copies non-system dylibs and GIO/gdk-pixbuf modules into `Contents/Frameworks`, rewrites Mach-O load commands to bundle-relative paths, and verifies dependency closure before code signing. The minimum macOS deployment target is derived from the highest target among the binary and bundled libraries.

### Flatpak packaging

The [Flatpak manifest](../packaging/flatpak/io.github.fanenr.gchat.yml) builds against GNOME SDK 51 with `-Dflatpak=true` and prefix `/app`, compiling `md4c` from a checksummed archive. The sandbox allows network access, Wayland/X11 display, desktop portals for file selection and URI opening, and host paths `xdg-data/gchat` and `xdg-config/gchat` so Flatpak and native builds share the same database and configuration.

## Tests

The regression suite in `tests/` runs against temporary directories without external network access:

- `test-app.c`: SSE framing, streaming lifecycle, request payload generation, web search / auto-title flows, image validation, and conversation state transitions.
- `test-store.c`: SQLite schema initialization, migrations, transactional saves, process locking, attachment garbage collection, and `config.json` handling.
- `test-md.c`: Markdown widget rendering, code blocks, table markup, link filtering, and image helpers.
- `test-i18n.c`: Locale resolution, fallback behavior, and catalog loading.

## Known limits

- Table cells support inline code and styles, but not multi-line code blocks or embedded images (which render as `[alt text]`).
- LaTeX math expressions (`$…$`) are displayed as raw source text.
- Text selection is bound to a single GTK text widget and cannot span across code blocks, tables, or multiple messages.
- Code highlighting matches `GtkSourceView` language IDs and a fixed alias map (such as `javascript` → `js`, `csharp` → `c-sharp`); unknown languages render as plain text.
- The SSE parser is tailored to DeepSeek's JSON stream and `[DONE]` sentinel rather than arbitrary SSE streams.
- HTML entity decoding covers numeric entities and a fixed table of common named entities.
- Drag-and-drop image import is not implemented, and encoding large image histories runs on the UI thread.
- Because draft autosave is debounced by 750 ms, an abnormal process termination (`SIGKILL` or crash) loses uncommitted edits or in-flight stream output.
