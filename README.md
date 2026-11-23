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
   meson setup build && meson compile -C build
   ```

3. Start the client:

   ```sh
   ./build/gchat
   ```

### Apple Silicon DMG

On an Apple Silicon Mac with the dependencies above and Python 3.9 or
later, run:

```sh
brew install librsvg
meson compile -C build macos-arm64
```

Copy `gchat.app` from the image to Applications. It uses the same settings
and conversations as `./build/gchat`. The target Mac does not need Homebrew.
The build prints the minimum macOS version for the image.

The target writes `build/gchat-<version>-arm64.dmg`. The app is ad-hoc
signed by default, so macOS can block it after download. To build a
notarized image at the same path, use a Developer ID Application identity
and a `notarytool` keychain profile:

```sh
GCHAT_CODESIGN_IDENTITY='Developer ID Application: Example (TEAMID)' \
GCHAT_NOTARY_PROFILE='gchat' meson compile -C build macos-arm64
```

For public releases, publish the corresponding sources required by the
licenses. The bundle records the dependency versions and license notices
under `Contents/Resources/Licenses`.

## Features

- Streaming replies: the client reads SSE asynchronously with libsoup3 and shows text as it arrives. It supports multi-line event data. A stop button interrupts generation and keeps the text received so far.
- Markdown rendering: md4c parses the text and renders it as native widgets, with headings, lists, quotes, tables, code blocks, and HTTPS images outside tables. Remote image servers can see your IP address and request time.
- Code blocks: a separate card with compact line spacing and a tinted header for the language label and copy button. GtkSourceView does the highlighting.
- Multiple sessions: search, pin, duplicate, rename, and delete sessions from the sidebar. New Flash conversations get an automatic title by default. Turn it off under **Settings → General → Conversation → Automatic conversation titles**.
- Deletion confirmation: on by default. Turn it off under **Settings → General → Conversation → Confirm conversation deletion**.
- Settings: **General** for the API key, conversation options, and language; **Appearance** for the theme and reading fonts; **Other** for the app version, conversation data location, and GitHub repository link.
- Adaptive layout: messages and the input area widen together on wide windows. The window requests a minimum size of 640 × 480 logical pixels.
- Reading: choose separate fonts and sizes for messages and code blocks. Line heights scale with their fonts; headings have distinct levels, lists use hanging indents, and tables keep a light, borderless layout with vertically centered cells. Code blocks and tables scroll horizontally without showing a scrollbar. Reset buttons appear only for non-default values. Changes apply immediately and persist; the sidebar and input box are unchanged.
- Message actions: copy, edit and resend, fork into a new session, regenerate, and delete.
- Image input: select or paste multiple images with `deepseek-flash`. Preview or remove them in the input box, then send with or without text.
- Web: turn on **Web** with `deepseek-flash` to let DeepSeek search the web and read links. The setting is saved and restored on the next launch. Cited sources appear below the reply.
- Models and reasoning levels: `deepseek-flash` and `deepseek-v4-pro`, with four reasoning levels.
- Drafts: each session keeps its own draft. It is written to disk 750ms after typing stops.
- Interface language: English and Simplified Chinese. A change applies after a client restart.
- Data safety: SQLite transactions, startup database upgrades, and a single-process lock.

## Shortcuts

App shortcuts use Cmd on macOS and Ctrl on Linux. Tab navigation uses the theme’s focus rings. A click clears the keyboard focus ring in the main window without changing how text fields show focus.

| macOS | Linux | Action |
| --- | --- | --- |
| `Cmd+N` | `Ctrl+N` | New session |
| `Cmd+L` | `Ctrl+L` | Focus the input box |
| `Cmd+F` | `Ctrl+F` | Focus search |
| `Cmd+,` | `Ctrl+,` | Open settings |
| `Cmd+.` | `Ctrl+.` | Stop generation |
| `Cmd+W` | `Ctrl+W` | Close the window |
| `Cmd+Q` | `Ctrl+Q` | Quit the app |
| `Enter` | `Enter` | Send the message |
| `Shift+Enter` | `Shift+Enter` | New line |
| `Esc` | `Esc` | Cancel an edit, or clear the search |

In the macOS message input, `Ctrl+F` and `Ctrl+B` move the cursor one character forward and back. `Ctrl+P` and `Ctrl+N` move it up and down one displayed line. `Ctrl+A` and `Ctrl+E` move to the start and end of the current displayed line, including wrapped lines.

`Ctrl+K` cuts the selection or the text to the end of the paragraph. `Ctrl+Y` pastes it back. These keys leave the system clipboard unchanged.

## Data

- Sessions: `~/.local/share/gchat/session.db`. Image copies are in `attachments/` beside it.
- The client does not import or delete the old `session.json` and `images/`.
- Configuration: `~/.config/gchat/config.json`. It holds the API key and app preferences.
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
