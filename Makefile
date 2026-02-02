# Makefile for l directory listing tool

# Use all CPU cores for parallel builds
MAKEFLAGS += -j$(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 1)

VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo "unknown")

PREFIX ?= $(HOME)/.local
DESTBINDIR = $(PREFIX)/bin
CONFIGDIR = $(HOME)/.config/l
CONFIG_FILE = config.toml
ZSH_COMPLETIONS = /usr/local/share/zsh/site-functions
BASH_COMPLETIONS = /usr/local/share/bash-completion/completions

# Local build output
BINDIR = bin

CC = cc
CFLAGS = -O2 -Wall -Wextra -std=c99 -DVERSION=\"$(VERSION)\"
LIBS = -lsqlite3 -lpthread -lz

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

# Debug build
ifdef DEBUG
  CFLAGS = -g -O0 -Wall -Wextra -std=c99 -fsanitize=address,undefined -DDEBUG
  CFLAGS += -fopenmp
  ifeq ($(HAVE_LIBGIT2),yes)
    CFLAGS += -DHAVE_LIBGIT2 $(shell pkg-config --cflags libgit2)
  endif
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
UI_OBJS = $(SRCDIR)/ui.o $(SRCDIR)/icons.o $(SRCDIR)/fileinfo.o
DAEMON_OBJS = $(SRCDIR)/daemon.o
SELECT_OBJS = $(SRCDIR)/select.o

# Main targets
all: $(BINDIR)/l $(BINDIR)/l-cached $(BINDIR)/cl

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

$(SRCDIR)/fileinfo.o: $(SRCDIR)/fileinfo.c $(SRCDIR)/fileinfo.h $(SRCDIR)/icons.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/ui.o: $(SRCDIR)/ui.c $(SRCDIR)/ui.h $(SRCDIR)/icons.h $(SRCDIR)/fileinfo.h $(SRCDIR)/cache.h $(SRCDIR)/git.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/l.o: $(SRCDIR)/l.c $(SRCDIR)/common.h $(SRCDIR)/cache.h $(SRCDIR)/git.h $(SRCDIR)/ui.h $(SRCDIR)/daemon.h $(SRCDIR)/select.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/daemon.o: $(SRCDIR)/daemon.c $(SRCDIR)/daemon.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/select.o: $(SRCDIR)/select.c $(SRCDIR)/select.h $(SRCDIR)/ui.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/ld.o: $(SRCDIR)/ld.c $(SRCDIR)/common.h $(SRCDIR)/cache.h $(SRCDIR)/scan.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/scan.o: $(SRCDIR)/scan.c $(SRCDIR)/scan.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: all
	@mkdir -p $(DESTBINDIR)
	@mkdir -p $(CONFIGDIR)
	install -m 755 $(BINDIR)/l $(DESTBINDIR)/l
	install -m 755 $(BINDIR)/l-cached $(DESTBINDIR)/l-cached
	install -m 755 $(SRCDIR)/cl $(DESTBINDIR)/cl
	install -m 644 $(CONFIG_FILE) $(CONFIGDIR)/$(CONFIG_FILE)
	@echo "Installed l, l-cached, and cl to $(DESTBINDIR)"
	@echo "Installed $(CONFIG_FILE) to $(CONFIGDIR)"
	@if [ -w "$(ZSH_COMPLETIONS)" ] || [ -w "$$(dirname $(ZSH_COMPLETIONS))" ]; then \
		mkdir -p $(ZSH_COMPLETIONS); \
		install -m 644 completions/_l $(ZSH_COMPLETIONS)/_l; \
		install -m 644 completions/_cl $(ZSH_COMPLETIONS)/_cl; \
		echo "Installed zsh completions to $(ZSH_COMPLETIONS)"; \
	else \
		echo "Note: Run with sudo to install zsh completions to $(ZSH_COMPLETIONS)"; \
	fi
	@if [ -w "$(BASH_COMPLETIONS)" ] || [ -w "$$(dirname $(BASH_COMPLETIONS))" ]; then \
		mkdir -p $(BASH_COMPLETIONS); \
		install -m 644 completions/l.bash $(BASH_COMPLETIONS)/l; \
		ln -sf l $(BASH_COMPLETIONS)/cl; \
		echo "Installed bash completions to $(BASH_COMPLETIONS)"; \
	else \
		echo "Note: Run with sudo to install bash completions to $(BASH_COMPLETIONS)"; \
	fi

uninstall:
	rm -f $(DESTBINDIR)/l $(DESTBINDIR)/l-cached $(DESTBINDIR)/cl
	rm -f $(CONFIGDIR)/$(CONFIG_FILE)
	rmdir $(CONFIGDIR) 2>/dev/null || true
	rm -f $(ZSH_COMPLETIONS)/_l $(ZSH_COMPLETIONS)/_cl
	rm -f $(BASH_COMPLETIONS)/l $(BASH_COMPLETIONS)/cl
	@echo "Uninstalled l"

clean:
	rm -f $(SRCDIR)/*.o $(BINDIR)/l $(BINDIR)/l-cached $(BINDIR)/cl

lint:
	clang-tidy $(SRCDIR)/*.c -- -I$(SRCDIR) $(CFLAGS) -I/usr/include -I/usr/lib/gcc/aarch64-linux-gnu/13/include -fopenmp

.PHONY: all install uninstall clean lint
