# Makefile for l directory listing tool

PREFIX ?= $(HOME)/.local
DESTBINDIR = $(PREFIX)/bin
CONFIGDIR = $(HOME)/.config/l
SITE_FUNCTIONS = /usr/local/share/zsh/site-functions

# Local build output
BINDIR = bin

CC = cc
CFLAGS = -O2 -Wall -Wextra -std=c99
LIBS = -lsqlite3 -lpthread

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
  CFLAGS += -fopenmp
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
GIT_OBJS = $(SRCDIR)/git.o
UI_OBJS = $(SRCDIR)/ui.o
DAEMON_OBJS = $(SRCDIR)/daemon.o

# Main targets
all: $(BINDIR)/l $(BINDIR)/l-cached

$(BINDIR):
	mkdir -p $(BINDIR)

$(BINDIR)/l: $(SRCDIR)/l.o $(COMMON_OBJS) $(CACHE_CLIENT_OBJS) $(GIT_OBJS) $(UI_OBJS) $(DAEMON_OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

$(BINDIR)/l-cached: $(SRCDIR)/ld.o $(COMMON_OBJS) $(CACHE_DAEMON_OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

# Object files
$(SRCDIR)/common.o: $(SRCDIR)/common.c $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/cache.o: $(SRCDIR)/cache.c $(SRCDIR)/cache.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/cache_daemon.o: $(SRCDIR)/cache_daemon.c $(SRCDIR)/cache.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/git.o: $(SRCDIR)/git.c $(SRCDIR)/git.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/ui.o: $(SRCDIR)/ui.c $(SRCDIR)/ui.h $(SRCDIR)/cache.h $(SRCDIR)/git.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/l.o: $(SRCDIR)/l.c $(SRCDIR)/common.h $(SRCDIR)/cache.h $(SRCDIR)/git.h $(SRCDIR)/ui.h $(SRCDIR)/daemon.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/daemon.o: $(SRCDIR)/daemon.c $(SRCDIR)/daemon.h $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/ld.o: $(SRCDIR)/ld.c $(SRCDIR)/common.h $(SRCDIR)/cache.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: all
	@mkdir -p $(DESTBINDIR)
	@mkdir -p $(CONFIGDIR)
	install -m 755 $(BINDIR)/l $(DESTBINDIR)/l
	install -m 755 $(BINDIR)/l-cached $(DESTBINDIR)/l-cached
	install -m 644 icons.toml $(CONFIGDIR)/icons.toml
	@echo "Installed l and l-cached to $(DESTBINDIR)"
	@echo "Installed icons.toml to $(CONFIGDIR)"
	@if [ -w "$(SITE_FUNCTIONS)" ] || [ -w "$$(dirname $(SITE_FUNCTIONS))" ]; then \
		mkdir -p $(SITE_FUNCTIONS); \
		install -m 644 completions/_l $(SITE_FUNCTIONS)/_l; \
		echo "Installed completions to $(SITE_FUNCTIONS)"; \
	else \
		echo "Note: Run with sudo to install completions to $(SITE_FUNCTIONS)"; \
	fi

uninstall:
	rm -f $(DESTBINDIR)/l $(DESTBINDIR)/l-cached
	rm -f $(CONFIGDIR)/icons.toml
	rmdir $(CONFIGDIR) 2>/dev/null || true
	rm -f $(SITE_FUNCTIONS)/_l
	@echo "Uninstalled l"

clean:
	rm -f $(SRCDIR)/*.o $(BINDIR)/l $(BINDIR)/l-cached

.PHONY: all install uninstall clean
