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

This installs three binaries (`l`, `l-cached`, `cl`), the default `config.toml` to `~/.config/l/`, and shell completions for zsh and bash.

### Dependencies

**Required:**
- sqlite3
- pthread
- zlib

**Optional:**
- libgit2 (faster git status)
- LLVM/Clang with OpenMP (parallel directory scanning)

On macOS with Homebrew:
```bash
brew install libgit2 llvm
```

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
| `-g` | Git-only mode (modified/untracked files, implies `-at`) |
| `-f, --filter PATTERN` | Filter files matching pattern (implies `-at`) |
| `--min-size SIZE` | Show only entries >= SIZE (e.g., `100M`, `1G`) |
| `-c, --color-all` | Don't gray out gitignored files |
| `--list` | Flat list output (no tree structure) |
| `--summary` | Show detailed summary for file/directory (auto-enabled for single file arguments) |
| `--no-icons` | Hide icons |
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
| `o` | Open file or toggle directory |
| `f` | Toggle files-only navigation |
| `Enter` | Print selected path and exit |
| `y` | Copy path to clipboard |
| `q` or `Esc` | Quit |

Text files open in `$EDITOR` (default: `vim`). Binary files (images, PDFs, videos, etc.) open with the system handler (`open` on macOS, `xdg-open` on Linux). Directories can be dynamically expanded beyond the initial depth limit.

### `cl` Command

`cl` clears the terminal and runs `l` with the same arguments. Useful as a quick refresh.

### Examples

```bash
l                    # Current directory
l -at                # All files with full tree
l -al ~/projects     # Long format, hidden files
l -g                 # Only git-modified files
l -f "*.go"          # Filter to Go files
l -i                 # Interactive selection
l -d3 --min-size 1G  # Directories/files >= 1GB, depth 3
l -Sr                # Sort by size, reversed (smallest first)
l --daemon           # Configure background caching
l --daemon status    # Check daemon status non-interactively
```

## Daemon

The size caching daemon (`l-cached`) runs in the background to pre-calculate directory sizes, making `l` display sizes instantly even for large directories.

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
- Scans from `/` periodically (default: every 30 minutes)
- Caches directories above file threshold (default: 1000+ files)
- Skips network filesystems automatically
- Live cache entry count display during scanning
- Shows last scan duration in status display
- Configurable via `~/.cache/l/config`

The daemon is managed via launchd on macOS and systemd on Linux, storing its cache in `~/.cache/l/sizes-v2.db`.

## Configuration

`l` reads configuration from `config.toml`, searched in order:
1. Same directory as the binary (development)
2. `~/.config/l/config.toml` (installed)
3. `/usr/local/share/l/config.toml` (system-wide)

### Sections

**`[icons]`** - Nerd Font icons for file types and UI elements:
```toml
[icons]
default = ""
closed_directory = ""
open_directory = ""
executable = ""
git_modified = ""
```

**`[extensions]`** - Icons for specific file extensions:
```toml
[extensions]
jpg,png,gif,webp,bmp,ico = ""
mp3,wav,flac,ogg,m4a,m4b = "󰝚"
mp4,mkv,avi,mov,webm = "󰿎"
```

**`[filetypes]`** - File type names for `--summary` output:
```toml
[filetypes]
c = "C source"
h = "C header"
cpp,cc,cxx = "C++ source"
cu = "CUDA source"
py = "Python"
rs = "Rust"
go = "Go"
```

**`[shebangs]`** - Map shebang interpreters to file types (for files without extensions):
```toml
[shebangs]
sh,bash,zsh = "Shell script"
python,python3 = "Python"
node,nodejs = "JavaScript"
```

Comma-separated extensions map to the same value. Custom entries override built-in defaults.

## License

MIT
