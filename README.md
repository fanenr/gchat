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
- md4c
- GtkSourceView 5
- gettext

1. Install the dependencies. On macOS, run:

   ```sh
   brew install meson ninja blueprint-compiler gtk4 libadwaita libsoup json-glib md4c gtksourceview5 gettext
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
- Markdown rendering: md4c parses the text and renders it as native widgets, with headings, lists, quotes, tables, and code blocks.
- Code blocks: a separate card with a language label and a copy button. GtkSourceView does the highlighting.
- Multiple sessions: search, pin, duplicate, rename, and delete sessions from the sidebar.
- Message actions: copy, edit and resend, fork into a new session, regenerate, and delete.
- Models and reasoning levels: `deepseek-flash` and `deepseek-v4-pro`, with four reasoning levels.
- Drafts: each session keeps its own draft. It is written to disk 750ms after typing stops.
- Interface language: English and Simplified Chinese. A change applies after a client restart.
- Data safety: atomic writes, backup restore, and a single-process lock.

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

- Sessions: `~/.local/share/gchat/session.json`.
- Configuration: `~/.config/gchat/config.json`. It holds the API key, the theme, and the language.
- The `DEEPSEEK_API_KEY` environment variable overrides the stored API key.
- Files are written with `0600` permissions. The content is not encrypted.

## Tests

```sh
meson test -C build --print-errorlogs
```

GTK tests need a display backend. Without a desktop environment, run `xvfb-run -a meson test -C build`.

## More

These topics are in [docs/DETAILS.md](docs/DETAILS.md):

- Session file format
- Local data and security
- Regression test coverage
- Code structure
- Known limits
