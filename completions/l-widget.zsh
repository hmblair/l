# l-widget.zsh — Use `l -i` as a Tab-triggered file completion widget
#
# Source this file in your .zshrc (after compinit) to replace the default
# Tab file completion with an interactive `l -i` picker for file arguments.

_l_complete() {
  local -a tokens
  tokens=(${(z)LBUFFER})

  # First word (command position) -- fall back to normal completion
  if (( ${#tokens} == 0 )) || [[ ${#tokens} -eq 1 && "$LBUFFER" != *" " ]]; then
    zle expand-or-complete
    return
  fi

  # Current word being completed
  local current=""
  if [[ "$LBUFFER" != *" " ]]; then
    current="${tokens[-1]}"
  fi

  # Flags -- fall back to normal completion
  if [[ "$current" == -* ]]; then
    zle expand-or-complete
    return
  fi

  # Split current into directory and prefix components
  local dir prefix
  if [[ "$current" == */* ]]; then
    dir="${current%/*}/"
    prefix="${current##*/}"
  else
    dir=""
    prefix="$current"
  fi

  # Check matches with glob first
  local target="${dir:-.}"
  local -a matches
  if [[ -n "$prefix" ]]; then
    matches=("${target}"/${prefix}*(N))
  else
    matches=("${target}"/*(N))
  fi

  # No matches
  if (( ${#matches} == 0 )); then
    zle expand-or-complete
    return
  fi

  # Single match -- insert directly
  if (( ${#matches} == 1 )); then
    local selected="${matches[1]}"
    if [[ -n "$current" ]]; then
      LBUFFER="${LBUFFER%"$current"}"
    fi
    [[ -n "$LBUFFER" && "$LBUFFER" != *" " ]] && LBUFFER+=" "
    LBUFFER+="${(q)selected}"
    zle reset-prompt
    return
  fi

  # Multiple matches -- use l -i
  local -a cmd
  cmd=(l -i --tty)
  if [[ -n "$prefix" ]]; then
    cmd+=(-f "${prefix}*")
  fi
  if [[ -n "$dir" ]]; then
    cmd+=("$dir")
  fi

  local selected
  selected=$("${cmd[@]}" </dev/tty)

  if [[ -n "$selected" ]]; then
    # Remove the partial word we're completing
    if [[ -n "$current" ]]; then
      LBUFFER="${LBUFFER%"$current"}"
    fi
    [[ -n "$LBUFFER" && "$LBUFFER" != *" " ]] && LBUFFER+=" "
    LBUFFER+="${(q)selected}"
    zle reset-prompt
    zle accept-line
  else
    zle reset-prompt
  fi
}

zle -N _l_complete
bindkey '^I' _l_complete
