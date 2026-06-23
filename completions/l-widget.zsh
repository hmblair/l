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
    # Extract the compadd prefix (-p). Only scan the option portion: anything
    # after the '--' separator is a candidate word (e.g. l's own '-p' flag), not
    # a compadd option, so misreading it would corrupt the inserted text.
    local _prefix=""
    local -a _args=("$@")
    for (( i=1; i<=${#_args}; i++ )); do
      [[ "${_args[$i]}" == "--" ]] && break
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

  # Only take over for filesystem-path completions. If none of the candidates
  # resolve to a real path (e.g. they're flags, options, or subcommands), defer
  # to zsh's native completion instead of the l -i picker.
  if (( ! is_command )); then
    local _c _p _is_path=0
    for _c in "${candidates[@]}"; do
      _p="${_l_captured_prefix}${_c}"
      _p="${_p/#\~/$HOME}"
      [[ -e "$_p" || -L "$_p" ]] && { _is_path=1; break; }
    done
    if (( ! _is_path )); then
      zle expand-or-complete
      return
    fi
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
      # Append a slash for directories so completion advances (matches native
      # zsh) instead of re-inserting the same text and stalling.
      [[ -d "${_result/#\~/$HOME}" ]] && _result+="/"
      _result="${(q)_result}"
      _result="${_result/#\\~/~}"
      LBUFFER+="$_result"
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
    # Only include candidates that resolve to a real path. Completion helpers
    # (e.g. _cd's named-directory probe) can inject spurious candidates; a single
    # nonexistent argument would make `l -i` abort entirely, so drop them here.
    for c in "${candidates[@]}"; do
      local _full="${_l_captured_prefix}${c}"
      [[ -e "${_full/#\~/$HOME}" || -L "${_full/#\~/$HOME}" ]] && paths+=("$_full")
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
      selected="${selected/#$HOME/~}"
      local _qsel="${(q)selected}"
      _qsel="${_qsel/#\\~/~}"
      LBUFFER+="$_qsel"
      zle accept-line
    fi
    zle reset-prompt
  else
    zle reset-prompt
  fi
}

zle -N _l_complete
bindkey '^I' _l_complete
