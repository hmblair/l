# l-widget.zsh — Use `l -i` as a Tab-triggered file completion widget
#
# Source this file in your .zshrc (after compinit) to replace the default
# Tab completion menu with an interactive `l -i` picker for file/directory
# completions. Non-file completions fall back to zsh defaults.

# Global state shared between the completion widget and the zle widget
typeset -ga _l_captured_candidates=()
typeset -g  _l_captured_prefix=""

# Debug trace: set _L_WIDGET_DEBUG=<file> to append a decision trace (and skip
# launching the interactive picker). See tools/widget-debug.zsh.
_l_debug() { [[ -n "$_L_WIDGET_DEBUG" ]] && print -r -- "$@" >> "$_L_WIDGET_DEBUG"; }

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
    _l_debug "  compadd${_prefix:+ (prefix=$_prefix)}: ${#_matches} match(es)${_matches:+ -> ${(j:, :)_matches}}"
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

# Insert a completed filesystem path into LBUFFER, replacing the current word.
# Appends "/" for directories (so completion advances) and a trailing space for
# runnable non-directory completions in command position. $1 = full path
# (prefix already applied), $2 = current word to strip, $3 = is_command flag.
_l_insert_path() {
  local _path="$1" _strip="$2" _cmd="$3"
  [[ -n "$_strip" ]] && LBUFFER="${LBUFFER%"$_strip"}"
  [[ -n "$LBUFFER" && "$LBUFFER" != *" " ]] && LBUFFER+=" "
  local _is_dir=0
  [[ -d "${_path/#\~/$HOME}" ]] && _is_dir=1
  (( _is_dir )) && _path+="/"
  _path="${(q)_path}"
  _path="${_path/#\\~/~}"
  LBUFFER+="$_path"
  (( _cmd && ! _is_dir )) && LBUFFER+=" "
}

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

  _l_debug "fire: LBUFFER=[$LBUFFER] is_command=$is_command prefix=[$_l_captured_prefix] candidates=(${(j: :)candidates})"

  # No candidates
  if (( ${#candidates} == 0 )); then
    _l_debug "-> no candidates"
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
      _l_debug "-> defer to native (no candidate is a real path)"
      zle expand-or-complete
      return
    fi
  fi

  # Single candidate -- insert directly. The prefix carries any path prefix
  # (e.g. "./" for a runnable script); a bare command name has an empty prefix.
  if (( ${#candidates} == 1 )); then
    _l_insert_path "${_l_captured_prefix}${candidates[1]}" "$current" "$is_command"
    _l_debug "-> single candidate insert: LBUFFER=[$LBUFFER]"
    zle reset-prompt
    return
  fi

  # Multiple candidates -- resolve to full paths for l -i
  local -a paths rels
  if (( is_command )); then
    for c in "${candidates[@]}"; do
      local full=$(whence -p "$c" 2>/dev/null)
      [[ -n "$full" ]] && paths+=("$full")
    done
  else
    # Only include candidates that resolve to a real path. Completion helpers
    # (e.g. _cd's named-directory probe) can inject spurious candidates; a single
    # nonexistent argument would make `l -i` abort entirely, so drop them here.
    # Keep a parallel `rels` array of the candidate strings (relative or
    # ~-prefixed) to insert: `l -i` echoes an absolute path, so we map its
    # selection back to the matching candidate to preserve the typed form.
    for c in "${candidates[@]}"; do
      local _full="${_l_captured_prefix}${c}"
      local _exp="${_full/#\~/$HOME}"
      [[ -e "$_exp" || -L "$_exp" ]] && { paths+=("$_exp"); rels+=("$_full"); }
    done
  fi

  # If we couldn't resolve any paths, fall back
  if (( ${#paths} == 0 )); then
    _l_debug "-> defer to native (no resolved paths)"
    zle expand-or-complete
    return
  fi

  # After filtering spurious candidates, a single real path should be inserted
  # directly rather than shown in a one-item picker. (Command position keeps the
  # picker, which inserts the basename of the chosen binary.)
  if (( ! is_command && ${#paths} == 1 )); then
    _l_insert_path "${rels[1]}" "$current" "$is_command"
    _l_debug "-> single filtered path insert: LBUFFER=[$LBUFFER]"
    zle reset-prompt
    return
  fi

  _l_debug "-> picker: paths=(${(j: :)paths})"
  # In debug mode, don't launch the interactive picker (it would block a
  # headless trace); report the paths it would have shown and stop.
  if [[ -n "$_L_WIDGET_DEBUG" ]]; then
    zle reset-prompt
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
      # l -i echoes an absolute path, which may be a file the user descended
      # into from a candidate directory (via 'o'/nested folds). Match the
      # selection against each candidate root as a path prefix and re-attach the
      # remainder to that candidate's display form, so the inserted text keeps
      # the relative/~-prefixed form the user typed at any nesting depth.
      local _ins="${selected/#$HOME/~}" _i _selA="${selected:A}"
      for _i in {1..${#paths}}; do
        local _pa="${paths[$_i]:A}"
        if [[ "$_selA" == "$_pa" ]]; then
          _ins="${rels[$_i]}"
          break
        elif [[ "$_selA" == "$_pa"/* ]]; then
          _ins="${rels[$_i]}/${_selA#$_pa/}"
          break
        fi
      done
      [[ -d "${_ins/#\~/$HOME}" ]] && _ins+="/"
      local _qsel="${(q)_ins}"
      _qsel="${_qsel/#\\~/~}"
      LBUFFER+="$_qsel"
    fi
    zle reset-prompt
  else
    zle reset-prompt
  fi
}

zle -N _l_complete
bindkey '^I' _l_complete
