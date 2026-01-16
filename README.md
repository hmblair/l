# l

Enhanced directory listing with tree view, icons, and git integration.

## Features

- Tree view with Unicode box-drawing characters
- Nerd Font icons for files, directories, and git status
- Git integration (branch, modified/untracked indicators, diff stats)
- Interactive selection mode with vim-like navigation
- Background daemon for caching directory sizes
- Long format with size, line count, and modification time
- Image dimensions and megapixels (JPEG, PNG, TIFF, WebP, BMP)
- Audio/video duration (M4B, M4A, MP4, MOV)
- Configurable depth limiting, filtering, and sorting

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

### Dependencies

**Required:**
- sqlite3
- pthread

**Optional:**
- libgit2 (faster git status)
- LLVM/Clang with OpenMP (parallel directory scanning)

On macOS with Homebrew:
```bash
brew install libgit2 llvm
```

## Usage

```
l [options] [path...]
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
| `-p, --path` | Show ancestry from ~ to target |
| `-i, --interactive` | Interactive selection mode |
| `-g` | Git-only mode (modified/untracked files, implies `-at`) |
| `-f, --filter PATTERN` | Filter files matching pattern (implies `-at`) |
| `-c, --color-all` | Don't gray out gitignored files |
| `--list` | Flat list output (no tree structure) |
| `--no-icons` | Hide icons |
| `--daemon` | Manage size caching daemon |
| `--version` | Show version |
| `-h, --help` | Show help |

### Sorting

| Flag | Description |
|------|-------------|
| `-S` | Sort by size (largest first) |
| `-T` | Sort by time (newest first) |
| `-N` | Sort by name (alphabetical) |
| `-r` | Reverse sort order |

### Interactive Mode

Press `-i` to enter interactive selection mode:

| Key | Action |
|-----|--------|
| `j/k` or arrows | Navigate up/down |
| `h/l` or arrows | Collapse/expand directories |
| `o` | Open file in `$EDITOR` or toggle directory |
| `Enter` | Print selected path and exit |
| `y` | Copy path to clipboard |
| `q` or `Esc` | Quit |

Directories can be dynamically expanded beyond the initial depth limit.

### Examples

```bash
l                    # Current directory
l -at                # All files with full tree
l -al ~/projects     # Long format, hidden files
l -g                 # Only git-modified files
l -f "*.go"          # Filter to Go files
l -i                 # Interactive selection
l -Sr                # Sort by size, reversed (smallest first)
l --daemon           # Configure background caching
```

## Daemon

The size caching daemon (`l-cached`) runs in the background to pre-calculate directory sizes, making `l` display sizes instantly even for large directories.

```bash
l --daemon           # Interactive daemon management
```

The interactive menu provides:
- **Start/Stop daemon** - manage the background process
- **Refresh now** - trigger an immediate scan
- **Clear cache** - remove all cached entries (restarts daemon)
- **Configure** - adjust scan interval and minimum file threshold

Features:
- Scans from `/` periodically (default: every 30 minutes)
- Caches directories above file threshold (default: 1000+ files)
- Skips network filesystems automatically
- Live cache entry count display during scanning
- Configurable via `~/.cache/l/config`

The daemon is managed via launchd on macOS and systemd on Linux, storing its cache in `~/.cache/l/sizes.db`.

## License

MIT
