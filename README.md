# gchat

**English** | [简体中文](README.zh-CN.md)

`gchat` is a native desktop client for DeepSeek written in C with GTK4 and libadwaita.

## Features

- **Models and reasoning**: Supports `deepseek-flash` and `deepseek-v4-pro` with four reasoning levels (`none`, `low`, `high`, `max`) and collapsible thought blocks.
- **Web search and images**: Optional web search with source citations and multi-image input (file picker or clipboard paste) on `deepseek-flash`.
- **Native Markdown**: Renders headings, lists, blockquotes, tables, syntax-highlighted code blocks, and HTTPS images directly as GTK widgets without a WebView.
- **Conversations**: Search, pin, rename, duplicate, and delete sessions; copy, edit and resend, regenerate, or fork any message into a new session; automatic titles and per-session drafts.
- **Appearance and language**: System, light, or dark theme; independent fonts and sizes for message prose and code blocks; English and Simplified Chinese interface.

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

2. Compile:

   ```sh
   meson setup build && meson compile -C build
   ```

3. Start the client:

   ```sh
   ./build/gchat
   ```

### Apple Silicon DMG

On an Apple Silicon Mac with the dependencies above and Python 3.9 or later:

```sh
brew install librsvg
meson compile -C build macos-arm64
```

This generates `build/gchat-<version>-arm64.dmg`, which bundles its non-system
libraries and does not require Homebrew on the target Mac. By default the app
is ad-hoc signed. To sign and notarize with a Developer ID Application
certificate and a `notarytool` keychain profile:

```sh
GCHAT_CODESIGN_IDENTITY='Developer ID Application: Example (TEAMID)' \
GCHAT_NOTARY_PROFILE='gchat' meson compile -C build macos-arm64
```

Bundled dependency versions and license notices are written to
`Contents/Resources/Licenses`.

### Linux Flatpak

Install the standalone `.flatpak` bundle for your architecture:

```sh
flatpak install --user ./gchat-x86_64.flatpak
flatpak run io.github.fanenr.gchat
```

The bundle uses the GNOME runtime from Flathub and shares [settings and conversations](docs/STORAGE.md)
with the native build. To build a `.flatpak` bundle from source:

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
| `Cmd+L` | `Ctrl+L` | Focus the input box |
| `Cmd+F` | `Ctrl+F` | Focus search |
| `Cmd+,` | `Ctrl+,` | Open settings |
| `Cmd+.` | `Ctrl+.` | Stop generation |
| `Cmd+W` | `Ctrl+W` | Close the window |
| `Cmd+Q` | `Ctrl+Q` | Quit the app |
| `Enter` | `Enter` | Send the message |
| `Shift+Enter` | `Shift+Enter` | New line |
| `Esc` | `Esc` | Cancel an edit, or clear the search |

On macOS, the message input also supports Emacs-style cursor keys (`Ctrl+F/B`,
`Ctrl+P/N`, `Ctrl+A/E`) and kill/yank (`Ctrl+K`, `Ctrl+Y`) using an internal
kill buffer.

## Data

- Sessions: `~/.local/share/gchat/session.db` and `attachments/`.
- Configuration: `~/.config/gchat/config.json`.
- The `DEEPSEEK_API_KEY` environment variable overrides the stored API key.
- Files are created with `0600` permissions without encryption.

## Tests

```sh
meson test -C build --print-errorlogs
```

Headless environments can run `xvfb-run -a meson test -C build`.

## More

- [Storage](docs/STORAGE.md): file paths, backups, database schema, and migrations.
- [Details](docs/DETAILS.md): architecture, key mechanisms, and known limits.
