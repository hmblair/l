# Makefile for l directory listing tool

# Use all CPU cores for parallel builds
MAKEFLAGS += -j$(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 1)

VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo "unknown")

PREFIX ?= $(HOME)/.local
DESTBINDIR = $(PREFIX)/bin
CONFIGDIR = $(HOME)/.config/l
CONFIG_FILE = config.toml
ZSH_COMPLETIONS = $(PREFIX)/share/zsh/site-functions
BASH_COMPLETIONS = $(PREFIX)/share/bash-completion/completions

# Local build output
BINDIR = bin

CC = cc
CFLAGS = -O2 -Wall -Wextra -std=c99 -DVERSION=\"$(VERSION)\"
LIBS = -lpthread -lz

# Cap on OpenMP threads (avoids thread-pool spin-up overhead on many-core
# machines). Override with `make MAX_THREADS=N`; set to 0 for no cap.
MAX_THREADS ?= 8

# OpenMP support (use homebrew clang on macOS)
UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
  # Check for homebrew llvm (Apple clang doesn't support OpenMP)
  BREW_LLVM := $(shell brew --prefix llvm 2>/dev/null)
  ifneq ($(BREW_LLVM),)
    CC = $(BREW_LLVM)/bin/clang
    CFLAGS += -fopenmp
  endif
else
  # Linux needs _GNU_SOURCE for O_DIRECTORY, fdopendir, fstatat, etc.
  CFLAGS += -fopenmp -D_GNU_SOURCE -D_DEFAULT_SOURCE
endif

# Optional libgit2 support
HAVE_LIBGIT2 := $(shell pkg-config --exists libgit2 2>/dev/null && echo yes)
ifeq ($(HAVE_LIBGIT2),yes)
  CFLAGS += -DHAVE_LIBGIT2 $(shell pkg-config --cflags libgit2)
  LIBS += $(shell pkg-config --libs libgit2)
endif

# Optional sqlite support (auto-detected; override with `make HAVE_SQLITE=no`).
# Without it, l builds with no size cache and l-cached is not built; otherwise
# unchanged. -DHAVE_SQLITE is appended after the DEBUG reset (see below) so it
# survives both build modes; the link libs are added here (LIBS is not reset).
SQLITE_DETECTED := $(shell pkg-config --exists sqlite3 2>/dev/null && echo yes || echo no)
HAVE_SQLITE ?= $(SQLITE_DETECTED)
ifeq ($(HAVE_SQLITE),yes)
  SQLITE_CFLAGS := $(shell pkg-config --cflags sqlite3)
  LIBS += $(shell pkg-config --libs sqlite3)
endif

# Debug build
ifdef DEBUG
  CFLAGS = -g -O0 -Wall -Wextra -std=c99 -fsanitize=address,undefined -DDEBUG
  CFLAGS += -fopenmp
  ifeq ($(HAVE_LIBGIT2),yes)
    CFLAGS += -DHAVE_LIBGIT2 $(shell pkg-config --cflags libgit2)
  endif
endif

# Thread cap (applied after the DEBUG reset so it affects both builds)
CFLAGS += -DL_MAX_THREADS=$(MAX_THREADS)

# SQLite define (applied after the DEBUG reset so it affects both builds)
ifeq ($(HAVE_SQLITE),yes)
  CFLAGS += -DHAVE_SQLITE $(SQLITE_CFLAGS)
endif

# Source directory
SRCDIR = src

# Object files (in src/)
COMMON_OBJS = $(SRCDIR)/common.o
CACHE_CLIENT_OBJS = $(SRCDIR)/cache.o
CACHE_DAEMON_OBJS = $(SRCDIR)/cache_daemon.o
SCAN_OBJS = $(SRCDIR)/scan.o
GIT_OBJS = $(SRCDIR)/git.o
TREE_OBJS = $(SRCDIR)/tree.o
UI_OBJS = $(SRCDIR)/view.o $(SRCDIR)/render.o $(SRCDIR)/icons.o $(SRCDIR)/fileinfo.o $(SRCDIR)/config.o $(SRCDIR)/format.o
DAEMON_OBJS = $(SRCDIR)/daemon.o
SELECT_OBJS = $(SRCDIR)/select.o

# Main targets (l-cached is only built when SQLite is available)
BINARIES = $(BINDIR)/l $(BINDIR)/cl
ifeq ($(HAVE_SQLITE),yes)
  BINARIES += $(BINDIR)/l-cached
endif

all: $(BINARIES)

$(BINDIR):
	mkdir -p $(BINDIR)

$(BINDIR)/l: $(SRCDIR)/l.o $(COMMON_OBJS) $(CACHE_CLIENT_OBJS) $(SCAN_OBJS) $(GIT_OBJS) $(TREE_OBJS) $(UI_OBJS) $(DAEMON_OBJS) $(SELECT_OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

$(BINDIR)/l-cached: $(SRCDIR)/ld.o $(COMMON_OBJS) $(CACHE_DAEMON_OBJS) $(SCAN_OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

$(BINDIR)/cl: $(SRCDIR)/cl | $(BINDIR)
	ln -sf ../$(SRCDIR)/cl $@

# Object files
$(SRCDIR)/common.o: $(SRCDIR)/common.c $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/cache.o: $(SRCDIR)/cache.c $(SRCDIR)/cache.h $(SRCDIR)/scan.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/cache_daemon.o: $(SRCDIR)/cache_daemon.c $(SRCDIR)/cache.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/git.o: $(SRCDIR)/git.c $(SRCDIR)/git.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/tree.o: $(SRCDIR)/tree.c $(SRCDIR)/tree.h $(SRCDIR)/fileinfo.h $(SRCDIR)/git.h $(SRCDIR)/cache.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/icons.o: $(SRCDIR)/icons.c $(SRCDIR)/icons.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/config.o: $(SRCDIR)/config.c $(SRCDIR)/config.h $(SRCDIR)/tree.h $(SRCDIR)/icons.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/format.o: $(SRCDIR)/format.c $(SRCDIR)/format.h $(SRCDIR)/tree.h $(SRCDIR)/icons.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/fileinfo.o: $(SRCDIR)/fileinfo.c $(SRCDIR)/fileinfo.h $(SRCDIR)/icons.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/view.o: $(SRCDIR)/view.c $(SRCDIR)/view.h $(SRCDIR)/config.h $(SRCDIR)/format.h $(SRCDIR)/tree.h $(SRCDIR)/git.h $(SRCDIR)/icons.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/render.o: $(SRCDIR)/render.c $(SRCDIR)/render.h $(SRCDIR)/view.h $(SRCDIR)/config.h $(SRCDIR)/format.h $(SRCDIR)/icons.h $(SRCDIR)/fileinfo.h $(SRCDIR)/cache.h $(SRCDIR)/git.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/l.o: $(SRCDIR)/l.c $(SRCDIR)/common.h $(SRCDIR)/cache.h $(SRCDIR)/config.h $(SRCDIR)/git.h $(SRCDIR)/view.h $(SRCDIR)/render.h $(SRCDIR)/daemon.h $(SRCDIR)/select.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/daemon.o: $(SRCDIR)/daemon.c $(SRCDIR)/daemon.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/select.o: $(SRCDIR)/select.c $(SRCDIR)/select.h $(SRCDIR)/view.h $(SRCDIR)/render.h $(SRCDIR)/config.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/ld.o: $(SRCDIR)/ld.c $(SRCDIR)/common.h $(SRCDIR)/cache.h $(SRCDIR)/scan.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/scan.o: $(SRCDIR)/scan.c $(SRCDIR)/scan.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: all
	@mkdir -p $(DESTBINDIR)
	@mkdir -p $(CONFIGDIR)
	install -m 755 $(BINDIR)/l $(DESTBINDIR)/l
ifeq ($(HAVE_SQLITE),yes)
	install -m 755 $(BINDIR)/l-cached $(DESTBINDIR)/l-cached
endif
	install -m 755 $(SRCDIR)/cl $(DESTBINDIR)/cl
ifeq ($(HAVE_SQLITE),yes)
	@echo "Installed l, l-cached, and cl to $(DESTBINDIR)"
else
	@echo "Installed l and cl to $(DESTBINDIR) (without SQLite; l-cached not built)"
endif
	@if [ -f $(CONFIGDIR)/$(CONFIG_FILE) ] && [ -z "$(OVERWRITE_CONFIG)" ]; then \
		echo "Kept existing $(CONFIG_FILE) in $(CONFIGDIR) (not overwritten; use OVERWRITE_CONFIG=1 to replace)"; \
	else \
		install -m 644 $(CONFIG_FILE) $(CONFIGDIR)/$(CONFIG_FILE); \
		echo "Installed $(CONFIG_FILE) to $(CONFIGDIR)"; \
	fi
	@mkdir -p $(ZSH_COMPLETIONS)
	install -m 644 completions/_l $(ZSH_COMPLETIONS)/_l
	install -m 644 completions/_cl $(ZSH_COMPLETIONS)/_cl
	install -m 644 completions/l-widget.zsh $(ZSH_COMPLETIONS)/l-widget.zsh
	@echo "Installed zsh completions and widget to $(ZSH_COMPLETIONS)"
	@if command -v zsh >/dev/null 2>&1 && ! zsh -ic 'echo "$$fpath"' 2>/dev/null | grep -qF "$(ZSH_COMPLETIONS)"; then \
		printf "\033[33mWarning:\033[0m $(ZSH_COMPLETIONS) is not in your zsh fpath\n"; \
		echo "  Add this to your .zshrc: fpath=($(ZSH_COMPLETIONS) \$$fpath)"; \
	fi
	@mkdir -p $(BASH_COMPLETIONS)
	install -m 644 completions/l.bash $(BASH_COMPLETIONS)/l
	ln -sf l $(BASH_COMPLETIONS)/cl
	@echo "Installed bash completions to $(BASH_COMPLETIONS)"
	@printf "\n\033[33mNote:\033[0m completion changes need a shell reload.\n"

uninstall:
	rm -f $(DESTBINDIR)/l $(DESTBINDIR)/l-cached $(DESTBINDIR)/cl
	rm -f $(CONFIGDIR)/$(CONFIG_FILE)
	rmdir $(CONFIGDIR) 2>/dev/null || true
	rm -f $(ZSH_COMPLETIONS)/_l $(ZSH_COMPLETIONS)/_cl $(ZSH_COMPLETIONS)/l-widget.zsh
	rm -f $(BASH_COMPLETIONS)/l $(BASH_COMPLETIONS)/cl
	@echo "Uninstalled l"

clean:
	rm -f $(SRCDIR)/*.o $(BINDIR)/l $(BINDIR)/l-cached $(BINDIR)/cl

lint:
	clang-tidy $(SRCDIR)/*.c -- -I$(SRCDIR) $(CFLAGS) -I/usr/include -I/usr/lib/gcc/aarch64-linux-gnu/13/include -fopenmp

.PHONY: all install uninstall clean lint
