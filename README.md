# gchat

**English** | [简体中文](README.zh-CN.md)

`gchat` is a DeepSeek desktop chat client written in C, with a GTK4 and libadwaita UI.
It uses no WebView and no scripting layer.

## Build and run

The client needs these packages:

- Meson, Ninja, and Blueprint Compiler
- GLib 2.66 or later
- GTK 4.12 or later
- libadwaita 1.5 or later
- libsoup 3
- JSON-GLib 1.6 or later
- SQLite 3.37 or later
- md4c
- GtkSourceView 5
- gettext

1. Install the dependencies. On macOS, run:

   ```sh
   brew install meson ninja blueprint-compiler gtk4 libadwaita libsoup json-glib sqlite md4c gtksourceview5 gettext
   export PKG_CONFIG_PATH="$(brew --prefix sqlite)/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
   ```

2. Compile:

   ```sh
   meson setup build && ninja -C build
   ```

3. Start the client:

   ```sh
   ./build/gchat
   ```

## Features

- Streaming replies: the client reads SSE asynchronously with libsoup3 and shows text as it arrives. It supports multi-line event data. A stop button interrupts generation and keeps the text received so far.
- Markdown rendering: md4c parses the text and renders it as native widgets, with headings, lists, quotes, tables, code blocks, and HTTPS images outside tables. Remote image servers can see your IP address and request time.
- Code blocks: a separate card with a language label and a copy button. GtkSourceView does the highlighting.
- Multiple sessions: search, pin, duplicate, rename, and delete sessions from the sidebar.
- Adaptive layout: messages and the input area widen together on wide windows. The window requests a minimum size of 640 × 480 logical pixels.
- Reading: choose separate fonts and sizes for messages and code blocks. Line heights scale with their fonts; headings have distinct levels, lists use hanging indents, and tables keep a light, borderless layout. Reset buttons appear only for non-default values. Changes apply immediately and persist; the sidebar and input box are unchanged.
- Message actions: copy, edit and resend, fork into a new session, regenerate, and delete.
- Image input: select or paste multiple images with `deepseek-flash`. Preview or remove them in the input box, then send with or without text.
- Web: turn on **Web** with `deepseek-flash` to let DeepSeek search the web and read links. Cited sources appear below the reply.
- Models and reasoning levels: `deepseek-flash` and `deepseek-v4-pro`, with four reasoning levels.
- Drafts: each session keeps its own draft. It is written to disk 750ms after typing stops.
- Interface language: English and Simplified Chinese. A change applies after a client restart.
- Data safety: SQLite transactions, startup database upgrades, and a single-process lock.

## Shortcuts

macOS uses Cmd. Ctrl also works. Linux uses Ctrl.

| Shortcut | Action |
| --- | --- |
| `Ctrl/Cmd+N` | New session |
| `Ctrl/Cmd+L` | Focus the input box |
| `Ctrl/Cmd+F` | Focus search |
| `Ctrl/Cmd+,` | Open settings |
| `Ctrl/Cmd+.` | Stop generation |
| `Enter` | Send the message |
| `Shift+Enter` | New line |
| `Esc` | Cancel an edit, or clear the search |

## Data

- Sessions: `~/.local/share/gchat/session.db`. Image copies are in `attachments/` beside it.
- The client does not import or delete the old `session.json` and `images/`.
- Configuration: `~/.config/gchat/config.json`. It holds the API key, the theme, and the language.
- The `DEEPSEEK_API_KEY` environment variable overrides the stored API key.
- Files are written with `0600` permissions. The content is not encrypted.

## Tests

```sh
meson test -C build --print-errorlogs
```

GTK tests need a display backend. Without a desktop environment, run `xvfb-run -a meson test -C build`.

## More

- [Storage](docs/STORAGE.md): data paths, backups, database format, and upgrades.
- [Details](docs/DETAILS.md): feature behavior, regression tests, code structure, and known limits.
