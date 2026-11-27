# gchat

**English** | [简体中文](README.zh-CN.md)

`gchat` is a native DeepSeek desktop client written in C with GTK4 and libadwaita.

## Features

- **Models and reasoning**: Supports `deepseek-flash` and `deepseek-v4-pro`, four reasoning levels (`none`, `low`, `high`, `max`), and collapsible reasoning blocks.
- **Web search and images**: On `deepseek-flash`, supports optional web search with source citations and multi-image input from files or the clipboard.
- **Native Markdown**: Renders headings, lists, blockquotes, tables, syntax-highlighted code blocks, and HTTPS images as GTK widgets without a WebView.
- **Sessions**: Search, pin, rename, duplicate, and delete sessions. Copy, edit and resend, regenerate, or fork messages into a new session. Supports automatic titles and per-session drafts.
- **Context management**: Set a default or per-session system prompt. Compress older history into a summary manually or automatically without deleting original messages.
- **User metadata**: Optionally send the local date, weekday, app platform, and interface language to DeepSeek. This option is off by default and excludes location.
- **Message navigation**: Open the numbered list in the header to search your messages, jump to an earlier turn, or return to the latest message.
- **Appearance and language**: Choose system, light, or dark theme, plus separate fonts and sizes for prose and code blocks. Supports English and Simplified Chinese; restart `gchat` after changing the language.

## Context management

- Configure the default system prompt, automatic compression threshold (default 64,000 tokens), and **Recent turns to keep** (default 2) in **Settings → Context**. Automatic compression is off by default. Set **Recent turns to keep** to `0` to compress all earlier turns.
- Open **Conversation context** next to the attachment button to inherit, override, or omit the system prompt, or to run **Compress context** manually.
- Compression sends one extra billable `deepseek-flash` request for the older messages and attached images. Later requests use the summary in place of those messages, while original messages and images stay in the session.
- **Context compressed** marks where compression happened. Click the marker to show or hide the summary. Messages before the marker are read-only, including kept turns, but you can still copy or fork them.
- For token estimation, ring colors, and calibration rules, see [Details](docs/DETAILS.md#context-management).

## Build and run

Dependencies:

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

2. Configure and build:

   ```sh
   meson setup build && meson compile -C build
   ```

3. Start `gchat`:

   ```sh
   ./build/gchat
   ```

### Apple Silicon DMG

On an Apple Silicon Mac with the build dependencies above and Python 3.9 or later, run:

```sh
brew install librsvg
meson compile -C build macos-arm64
```

This creates `build/gchat-<version>-arm64.dmg`. The bundle includes its non-system libraries, so the target Mac does not require Homebrew.

By default, the build applies an ad-hoc signature. To sign and notarize with a Developer ID Application certificate and a `notarytool` keychain profile, run:

```sh
GCHAT_CODESIGN_IDENTITY='Developer ID Application: Example (TEAMID)' \
GCHAT_NOTARY_PROFILE='gchat' meson compile -C build macos-arm64
```

Dependency versions and license notices are written to `Contents/Resources/Licenses`.

### Linux Flatpak

To install a standalone `.flatpak` bundle for your architecture, run:

```sh
flatpak install --user ./gchat-x86_64.flatpak
flatpak run io.github.fanenr.gchat
```

The bundle uses the GNOME runtime from Flathub and shares [Storage](docs/STORAGE.md) paths with native builds.

To build a `.flatpak` bundle from source, run:

```sh
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub org.flatpak.Builder
mkdir -p build-flatpak
flatpak_dir="${FLATPAK_USER_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/flatpak}"
flatpak run --env=FLATPAK_USER_DIR="$flatpak_dir" \
  --command=flatpak-builder org.flatpak.Builder \
  --user --install-deps-from=flathub --force-clean \
  --state-dir=build-flatpak/state --repo=build-flatpak/repo \
  build-flatpak/app packaging/flatpak/io.github.fanenr.gchat.yml
flatpak build-bundle \
  --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
  build-flatpak/repo "build-flatpak/gchat-$(flatpak --default-arch).flatpak" \
  io.github.fanenr.gchat stable
```

## Shortcuts

| macOS | Linux | Action |
| --- | --- | --- |
| `Cmd+N` | `Ctrl+N` | New session |
| `Cmd+L` | `Ctrl+L` | Focus input box |
| `Cmd+F` | `Ctrl+F` | Focus search |
| `Cmd+,` | `Ctrl+,` | Open settings |
| `Cmd+.` | `Ctrl+.` | Stop generation or compression |
| `Cmd+W` | `Ctrl+W` | Close window |
| `Cmd+Q` | `Ctrl+Q` | Quit `gchat` |
| `Enter` | `Enter` | Send message |
| `Shift+Enter` | `Shift+Enter` | Insert newline |
| `Esc` | `Esc` | Cancel edit or clear search |

On macOS, the message input also supports Emacs-style cursor keys (`Ctrl+F/B`, `Ctrl+P/N`, `Ctrl+A/E`) and kill/yank (`Ctrl+K`, `Ctrl+Y`) with an internal kill buffer.

## Data

- Sessions: `~/.local/share/gchat/session.db` and `~/.local/share/gchat/attachments/`.
- Configuration: `~/.config/gchat/config.json`.
- If `DEEPSEEK_API_KEY` is set, it overrides the stored API key.
- `gchat` creates data files with `0600` permissions and stores them without encryption.

## Tests

```sh
meson test -C build --print-errorlogs
```

In a headless environment, run `xvfb-run -a meson test -C build`.

## More

- [Storage](docs/STORAGE.md): file paths, backups, database schema, and migrations.
- [Details](docs/DETAILS.md): code structure, core mechanisms, packaging, and known limits.
