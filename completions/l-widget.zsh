# l-widget.zsh — Use `l -i` as a Tab-triggered file completion widget
#
# Source this file in your .zshrc (after compinit) to replace the default
# Tab completion menu with an interactive `l -i` picker for file/directory
# completions. Non-file completions fall back to zsh defaults.

# Global state shared between the completion widget and the zle widget
typeset -ga _l_captured_candidates=()
typeset -g  _l_captured_prefix=""

# Completion widget function — runs inside a real completion context
_l_capture_complete() {
  _l_captured_candidates=()
  _l_captured_prefix=""

  # Override compadd to capture candidates
  compadd() {
    # Extract prefix (-p) from args
    local _prefix=""
    local -a _args=("$@")
    for (( i=1; i<=${#_args}; i++ )); do
      if [[ "${_args[$i]}" == "-p" && $i -lt ${#_args} ]]; then
        _prefix="${_args[$((i+1))]}"
        break
      fi
    done
    [[ -n "$_prefix" ]] && _l_captured_prefix="$_prefix"

    # Use builtin compadd with -O to capture candidates into an array
    local -a _matches
    builtin compadd -O _matches "$@"
    _l_captured_candidates+=("${_matches[@]}")
  }

  # Run the real completion system
  _main_complete

  # Restore builtin compadd
  unfunction compadd 2>/dev/null

  # Signal: don't insert anything yet
  compstate[insert]=''
  compstate[list]=''
}

# Register as a completion widget (runs in completion context)
zle -C _l_capture complete-word _l_capture_complete

# Main zle widget bound to Tab
_l_complete() {
  # Run capture in completion context
  local -a tokens
  tokens=(${(z)LBUFFER})

  # Determine if we're completing the first word (command position)
  local is_command=0
  if (( ${#tokens} == 0 )) || [[ ${#tokens} -eq 1 && "$LBUFFER" != *" " ]]; then
    is_command=1
  fi

  # Capture candidates from zsh's completion system
  # Save terminal cursor position and buffer state since complete-word
  # redraws the line (destroying syntax highlighting)
  # Clear autosuggestion ghost text before capturing
  POSTDISPLAY=""
  _zsh_autosuggest_clear 2>/dev/null
  zle -R
  local saved_lbuffer="$LBUFFER"
  local saved_rbuffer="$RBUFFER"
  zle _l_capture
  LBUFFER="$saved_lbuffer"
  RBUFFER="$saved_rbuffer"

  local -a candidates
  candidates=("${_l_captured_candidates[@]}")

  # Deduplicate candidates
  candidates=(${(u)candidates})

  # No candidates
  if (( ${#candidates} == 0 )); then
    return
  fi

  # Current word being completed
  local current=""
  if [[ "$LBUFFER" != *" " && ${#tokens} -gt 0 ]]; then
    current="${tokens[-1]}"
  fi

  # Single candidate -- insert directly
  if (( ${#candidates} == 1 )); then
    if [[ -n "$current" ]]; then
      LBUFFER="${LBUFFER%"$current"}"
    fi
    [[ -n "$LBUFFER" && "$LBUFFER" != *" " ]] && LBUFFER+=" "
    if (( is_command )); then
      LBUFFER+="${candidates[1]} "
    else
      local _result="${_l_captured_prefix}${candidates[1]}"
      LBUFFER+="${(q)_result}"
    fi
    zle reset-prompt
    return
  fi

  # Multiple candidates -- resolve to full paths for l -i
  local -a paths
  if (( is_command )); then
    for c in "${candidates[@]}"; do
      local full=$(whence -p "$c" 2>/dev/null)
      [[ -n "$full" ]] && paths+=("$full")
    done
  else
    for c in "${candidates[@]}"; do
      paths+=("${_l_captured_prefix}${c}")
    done
  fi

  # If we couldn't resolve any paths, fall back
  if (( ${#paths} == 0 )); then
    zle expand-or-complete
    return
  fi

  # Expand tildes in paths
  paths=("${paths[@]/#\~/$HOME}")
  print -n "\n" >/dev/tty
  local selected
  selected=$(l -i --tty -d0 "${paths[@]}" </dev/tty 2>/dev/tty)
  # Clear everything from cursor to end of screen, then move to prompt line
  print -n "\033[J\033[A\r\033[K" >/dev/tty

  if [[ -n "$selected" ]]; then
    if [[ -n "$current" ]]; then
      LBUFFER="${LBUFFER%"$current"}"
    fi
    [[ -n "$LBUFFER" && "$LBUFFER" != *" " ]] && LBUFFER+=" "
    if (( is_command )); then
      LBUFFER+="${selected:t} "
    else
      LBUFFER+="${(q)selected}"
      zle accept-line
    fi
    zle reset-prompt
  else
    zle reset-prompt
  fi
}

zle -N _l_complete
bindkey '^I' _l_complete
