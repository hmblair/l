# l

Enhanced directory listing with tree view, icons, and git integration.

## Features

- Tree view with Unicode box-drawing characters
- Nerd Font icons for files, directories, and git status
- Git integration (branch, modified/untracked indicators, diff stats, clickable remote URL)
- Interactive selection mode with vim-like navigation
- Background daemon for caching directory sizes
- Long format with size, line count, and modification time
- Image dimensions and megapixels (JPEG, PNG, TIFF, WebP, BMP)
- Audio/video duration (M4B, M4A, MP3, WAV, MP4, MOV, MKV, WebM)
- PDF page counts
- Configurable depth limiting, filtering, and sorting
- Automatic network filesystem detection
- Shell completions for zsh and bash
- Optional zsh Tab widget that turns path completion into an `l -i` picker

## Installation

```bash
git clone https://github.com/hmblair/l.git
cd l
make && make install
```

Installs to `~/.local/bin` by default. Override with `PREFIX`:

```bash
make install PREFIX=/usr/local
```

This installs three binaries (`l`, `l-cached`, `cl`), the default `config.toml` to `~/.config/l/`, and shell completions for zsh and bash. An existing `config.toml` is left untouched, so reinstalling never clobbers local customizations.

### Dependencies

**Required:**
- pthread
- zlib

**Optional:**
- sqlite3 (size caching daemon; auto-detected at build time)
- libgit2 (faster git status)
- LLVM/Clang with OpenMP (parallel directory scanning)

On macOS with Homebrew:
```bash
brew install libgit2 llvm
```

sqlite3 is detected automatically via `pkg-config`. When present, the size
caching daemon (`l-cached`) is built and `l` reads cached directory sizes. When
absent, `l` still builds and runs — it just computes sizes with a live scan
every time, and `l --daemon` reports that the daemon is unavailable. To skip
sqlite even when it is installed, build with `make HAVE_SQLITE=no`.

A [Nerd Font](https://www.nerdfonts.com/) is recommended for proper icon display.

## Usage

```
l [OPTIONS] [FILE ...]
```

### Options

| Flag | Description |
|------|-------------|
| `-a` | Show hidden files |
| `-l, --long` | Long format (size, lines, time) - default |
| `-s, --short` | Short format (auto-enabled on network filesystems) |
| `-t, --tree` | Show full tree (depth 50) |
| `-d, --depth N` | Limit tree depth |
| `-e, --expand-all` | Expand all directories (ignore skip list) |
| `-p, --path` | Show ancestry from `~` (or `/`) to target |
| `-i, --interactive` | Interactive selection mode |
| `-m` | Git-changed files, rooted at the repo (errors outside a repo; shows just the repo root when clean) |
| `-g` | Hide gitignored files and folders |
| `-f, --filter PATTERN` | Filter files matching pattern (implies `-at`) |
| `--min-size SIZE` | Show only entries >= SIZE (e.g., `100M`, `1G`) |
| `--dir-only` | Show only directories |
| `-c, --color-all` | Don't gray out gitignored files |
| `--list` | Flat list output (no tree structure) |
| `--summary` | Show detailed summary for file/directory (auto-enabled for single file arguments) |
| `--no-icons` | Hide icons |
| `--tty` | Force TTY mode (colors, icons) even when piped |
| `--daemon [SUBCMD]` | Manage size caching daemon (must be first argument) |
| `--version` | Show version (must be first argument) |
| `-h, --help` | Show help |

### Sorting

| Flag | Description |
|------|-------------|
| `-S` | Sort by size (largest first) |
| `-T` | Sort by time (newest first) |
| `-N` | Sort by name (alphabetical) |
| `-r` | Reverse sort order |

### Interactive Mode

Use `-i` to enter interactive selection mode:

| Key | Action |
|-----|--------|
| `j/k` or `↑/↓` | Navigate up/down |
| `h/l` or `←/→` | Collapse/expand directories |
| `o` | Open file (picker stays open) or toggle directory |
| `f` | Toggle files-only navigation |
| `/` | Filter entries by name (type to narrow, `Esc` to cancel) |
| `r` | Reload the tree from scratch (like reopening the picker; cursor is kept) |
| `Enter` | Print selected path and exit |
| `y` | Copy path to clipboard |
| `q` or `Esc` | Quit |

Press `/` to filter the listing by name: type to narrow the entries live (substring match, or a glob if the query contains `*`, `?`, or `[`), use `↑/↓` to move through the matches, `Enter` to select the highlighted one, and `Esc` to clear the filter and return to the full listing. Matching is smart-case, like vim: case-insensitive unless the query contains an uppercase letter.

Text files open in `$EDITOR` (default: `vim`). Binary files (images, PDFs, videos, etc.) open with the system handler (`open` on macOS, `xdg-open` on Linux). Directories can be dynamically expanded beyond the initial depth limit.

The listing is a snapshot from when the picker opened: expanding a directory reads it on first open, and nothing updates automatically while the picker is idle. Press `r` to re-read everything (equivalent to quitting and re-running `l -i`).

### Tab Completion Widget (zsh)

An optional zsh widget rebinds Tab to use `l -i` as an interactive picker for file and directory completions. When a completion resolves to multiple paths it opens the `l -i` picker instead of the default menu; a single match inserts directly (directories gain a trailing slash), and non-path completions (flags, subcommands) fall through to zsh's normal completion.

It is installed but not enabled automatically. Source it from your `.zshrc`, after `compinit`:

```zsh
source ~/.local/share/l/shell/l-widget.zsh
```

Completion changes need a shell reload (`exec zsh`) to take effect.

### `cl` Command

`cl` clears the terminal and runs `l` with the same arguments. Useful as a quick refresh.

### Examples

```bash
l                    # Current directory
l -at                # All files with full tree
l -al ~/projects     # Long format, hidden files
l -m                 # Git-changed files, as a tree from the repo root
l -g                 # Hide gitignored files and folders
l -f "*.go"          # Filter to Go files
l -i                 # Interactive selection
l -d3 --min-size 1G  # Directories/files >= 1GB, depth 3
l -Sr                # Sort by size, reversed (smallest first)
l --daemon           # Configure background caching
l --daemon status    # Check daemon status non-interactively
```

## Daemon

The size caching daemon (`l-cached`) runs in the background to pre-calculate directory sizes, making `l` display sizes instantly even for large directories.

> Requires sqlite3 at build time. If `l` was built without sqlite (see
> [Dependencies](#dependencies)), the daemon is unavailable: `l --daemon`
> reports this and exits, and `l` computes sizes with a live scan instead.

```bash
l --daemon           # Interactive daemon management
```

The interactive menu provides:
- **Start/Stop daemon** - manage the background process
- **Refresh now** - trigger an immediate scan via SIGUSR1
- **Clear cache** - remove all cached entries (restarts daemon if running)
- **Configure** - adjust scan interval and minimum file threshold

Non-interactive subcommands are also available for scripting:

```bash
l --daemon start     # Start the daemon
l --daemon stop      # Stop and uninstall the daemon
l --daemon status    # Show daemon and cache status
l --daemon refresh   # Trigger an immediate cache refresh
l --daemon clear     # Clear all cached entries
```

Features:
- Scans from `/` periodically (default: hourly)
- Caches directories above file threshold (default: 1000+ files)
- Skips network filesystems automatically
- Live cache entry count display during scanning
- Shows last scan duration in status display
- Configurable via `~/.config/l/daemon.conf`

The daemon is managed via launchd on macOS and systemd on Linux, storing its cache in `~/.cache/l/sizes-v3.db`.

## Configuration

`l` reads configuration from `config.toml`, searched in order:
1. Same directory as the binary (development)
2. `~/.config/l/config.toml` (installed)
3. `/usr/local/share/l/config.toml` (system-wide)

### Sections

| Section | Description |
|---------|-------------|
| `[display]` | Rendering options: `column_separator` (glyph between long-mode columns; blank for a plain two-space gap) and UI icons (git status, git branch/commit/tag, counts, cursor, cwd marker, symlink arrow, readonly) |
| `[icons]` | Nerd Font icons for file types (directories, executables, devices, ...) |
| `[extensions]` | Icons for specific file extensions |
| `[filetypes]` | File type names for `--summary` output |
| `[shebangs]` | Map shebang interpreters to file types (for files without extensions) |

Comma-separated keys map to the same value. Custom entries override built-in defaults. See `config.toml` for all available options.

## License

MIT
