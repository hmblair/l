#!/usr/bin/env zsh
# widget-debug.zsh — trace the l Tab-completion widget for a given input line.
#
# Usage:
#   tools/widget-debug.zsh '<input line>' [cwd]
#
# Examples:
#   tools/widget-debug.zsh 'cd ~/.local/sh'
#   tools/widget-debug.zsh './.bo' ~/.dotfiles
#   tools/widget-debug.zsh 'l --ver'
#
# Spins up a throwaway interactive zsh (via zpty) that sources this repo's
# shell/l-widget.zsh with _L_WIDGET_DEBUG enabled, types the input
# followed by Tab, and prints the widget's decision trace: the compadd calls,
# the captured candidates/prefix, the branch taken, and the resulting buffer.
# The interactive picker is skipped in debug mode, so it never blocks.

emulate -L zsh

local input="${1:-}"
if [[ -z "$input" ]]; then
  print -u2 "usage: ${0:t} '<input line>' [cwd]"
  return 1 2>/dev/null || exit 1
fi
local cwd="${2:-$PWD}"
local repo="${0:A:h:h}"
local widget="$repo/shell/l-widget.zsh"
if [[ ! -f "$widget" ]]; then
  print -u2 "widget not found: $widget"
  return 1 2>/dev/null || exit 1
fi

zmodload zsh/zpty

local log zdotdir
log=$(mktemp)
zdotdir=$(mktemp -d)

cat > "$zdotdir/.zshrc" <<EOF
fpath=(${(q)repo}/completions \$fpath)
autoload -Uz compinit && compinit -u 2>/dev/null
export _L_WIDGET_DEBUG=${(q)log}
cd ${(q)cwd} 2>/dev/null
source ${(q)widget}
EOF

zpty WDBG "ZDOTDIR=${(q)zdotdir} zsh -i"

# Drain shell startup, then send the input + Tab, then drain the widget run.
local i discard
for i in {1..25}; do zpty -r -t WDBG discard 2>/dev/null; sleep 0.15; done
zpty -w -n WDBG "${input}"$'\t'
for i in {1..20}; do zpty -r -t WDBG discard 2>/dev/null; sleep 0.15; done
zpty -d WDBG 2>/dev/null

print -r -- "=== input: [${input}]   cwd: ${cwd} ==="
if [[ -s "$log" ]]; then
  cat "$log"
else
  print -r -- "(no trace — widget did not fire; is Tab bound to _l_complete?)"
fi

rm -rf "$log" "$zdotdir"
