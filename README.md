# l

Enhanced directory listing with tree view, icons, and git integration.

## Features

- Tree view with Unicode box-drawing characters
- Nerd Font icons for files, directories, and git status
- Git integration (branch, modified/untracked indicators)
- Background daemon for caching directory sizes
- Long format with size, line count, and modification time
- Configurable depth limiting and sorting

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
| `-l, --long` | Long format (size, lines, time) |
| `-s, --short` | Short format (no metadata) |
| `-t, --tree` | Show full tree |
| `-d, --depth N` | Limit tree depth |
| `-p, --path` | Show ancestry from ~ to target |
| `-g` | Git-only mode (modified/untracked files) |
| `-S` | Sort by size |
| `-T` | Sort by time |
| `-N` | Sort by name |
| `-r` | Reverse sort order |
| `--daemon` | Manage size caching daemon |
| `--no-icons` | Hide icons |

### Examples

```bash
l                    # Current directory
l -at                # All files with full tree
l -al ~/projects     # Long format, hidden files
l -g                 # Only git-modified files
l -ST                # Sort by size, then time
l --daemon           # Configure background caching
```

## License

MIT
