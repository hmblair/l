# l-widget.zsh — Use `l -i` as a Tab-triggered file completion widget
#
# Source this file in your .zshrc (after compinit) to replace the default Tab
# completion with: the interactive `l -i` picker for file/directory
# completions, and widget-managed listing plus repeat-Tab cycling for
# everything else.
#
# Design: zsh's completion system is used as a read-only oracle. Each new
# completion runs exactly one capture pass (a zle -C widget with compadd
# wrapped) to learn the candidate words and whether they are filenames; every
# behavior after that — picker, insertion, cycling — is driven from the
# captured list. Control is never handed back to native completion: zle tears
# down an active completion menu before running any bound non-completion
# widget (i.e. this one), so re-entering compsys on a repeat Tab restarts its
# menu at the first match instead of cycling. The one exception is drawing the
# candidate list, which is stateless display and cannot insert anything while
# AUTO_MENU is suppressed.

# Captured state shared between the completion widget and the zle widget
typeset -ga _l_captured_candidates=()
typeset -g  _l_captured_prefix=""
typeset -gi _l_captured_has_file=0

# Cycle state, armed when multiple non-file candidates are listed; repeat Tabs
# replace the current word with successive candidates until another key is
# pressed.
typeset -ga _l_cycle_cands=()
typeset -g  _l_cycle_base=""
typeset -gi _l_cycle_idx=0

# Debug trace: set _L_WIDGET_DEBUG=<file> to append a decision trace (and skip
# launching the interactive picker). See tools/widget-debug.zsh.
_l_debug() { [[ -n "$_L_WIDGET_DEBUG" ]] && print -r -- "$@" >> "$_L_WIDGET_DEBUG"; }

# Completion widget function — runs inside a real completion context
_l_capture_complete() {
  _l_captured_candidates=()
  _l_captured_prefix=""
  _l_captured_has_file=0

  # Override compadd to capture candidates
  compadd() {
    # Walk the option portion of the call to extract the hidden prefix (-p)
    # and whether the matches are filenames (-f, as _path_files passes).
    # Options may be clustered (-Qf) and an option's argument may be attached
    # (-Pfoo) or the next word (-J name) — and can itself start with '-'
    # (e.g. -J -default-), so arguments must be skipped, never scanned for
    # flag letters. A lone '-' or '--' ends the options; everything after is a
    # candidate word.
    local _prefix="" _w _c _rest
    local -i _has_f=0 _i _j
    for (( _i=1; _i<=$#; _i++ )); do
      _w="${argv[$_i]}"
      [[ "$_w" == "-" || "$_w" == "--" || "$_w" != -* ]] && break
      for (( _j=2; _j<=${#_w}; _j++ )); do
        _c="${_w[$_j]}"
        if [[ "$_c" == "f" ]]; then
          _has_f=1
        elif [[ "$_c" == [FPSpsiIWdJVXxrRDOAEMo] ]]; then
          # Option letter that takes an argument: attached or the next word.
          _rest="${_w[$((_j+1)),-1]}"
          if [[ -z "$_rest" ]]; then
            (( _i++ ))
            _rest="${argv[$_i]}"
          fi
          [[ "$_c" == "p" ]] && _prefix="$_rest"
          break
        fi
      done
    done
    [[ -n "$_prefix" ]] && _l_captured_prefix="$_prefix"

    # Use builtin compadd with -O to capture candidates into an array
    local -a _matches
    builtin compadd -O _matches "$@"
    _l_captured_candidates+=("${_matches[@]}")
    (( _has_f && ${#_matches} )) && _l_captured_has_file=1
    if [[ -n "$_L_WIDGET_DEBUG" ]]; then
      local _tag=""
      (( _has_f )) && _tag=" (file)"
      _l_debug "  compadd${_prefix:+ (prefix=$_prefix)}${_tag}: ${#_matches} match(es)${_matches:+ -> ${(j:, :)_matches}}"
    fi
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

# Show the candidate list via zsh (display only: AUTO_MENU and MENU_COMPLETE
# are off for the whole widget pass, so beyond an unambiguous common prefix
# nothing can be inserted) and arm the cycle state so repeat Tabs step through
# the candidates. $1 = current word being completed; the rest = candidates.
_l_list_and_arm() {
  local current="$1"
  shift
  if (( $# > 1 )); then
    _l_cycle_cands=(${(o)@})
    _l_cycle_base="${LBUFFER%"$current"}"
    _l_cycle_idx=0
  fi
  zle expand-or-complete
}

# Main zle widget bound to Tab
_l_complete() {
  # Repeat Tab with an armed cycle: swap in the next candidate (wrapping
  # around) without touching the completion system.
  if [[ "$LASTWIDGET" == "_l_complete" ]] && (( ${#_l_cycle_cands} )); then
    (( _l_cycle_idx = _l_cycle_idx % ${#_l_cycle_cands} + 1 ))
    LBUFFER="${_l_cycle_base}${(q)_l_cycle_cands[$_l_cycle_idx]}"
    _l_debug "-> cycle $_l_cycle_idx/${#_l_cycle_cands}: LBUFFER=[$LBUFFER]"
    return
  fi
  _l_cycle_cands=()

  # The capture pass below registers a completion attempt with zle, so the
  # listing pass in _l_list_and_arm would otherwise look like an immediate
  # second Tab and AUTO_MENU would insert the first match; MENU_COMPLETE would
  # insert on the first. Keep both off for the whole pass. Repeat Tabs never
  # reach compsys at all (cycle branch above).
  setopt localoptions noautomenu nomenucomplete

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

  # Take over only for filesystem completions, i.e. when a captured compadd
  # call was flagged as adding filenames (-f, as _path_files passes). Testing
  # whether candidates exist on disk is not a safe substitute: a subcommand or
  # hostname that coincides with a file in the cwd must not hijack a non-file
  # completion. Everything else is listed and cycled by the widget.
  if (( ! is_command && ! _l_captured_has_file )); then
    _l_debug "-> list + cycle (non-file completion)"
    _l_list_and_arm "$current" "${candidates[@]}"
    return
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
    # zsh yields command candidates in $PATH/readdir order; sort by name so the
    # picker lists them alphabetically rather than grouped by $PATH directory.
    for c in "${(@o)candidates}"; do
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

  # If no candidate survived path resolution, treat it like a non-file
  # completion: list once and cycle on repeat Tabs.
  if (( ${#paths} == 0 )); then
    _l_debug "-> list + cycle (no resolved paths)"
    _l_list_and_arm "$current" "${candidates[@]}"
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
  # When completing a command, the candidates are binaries on $PATH; don't gray
  # out any that happen to live in a gitignored directory (-c, --color-all).
  local -a _picker_opts=(-i --tty -d0)
  (( is_command )) && _picker_opts+=(-c)
  selected=$(l "${_picker_opts[@]}" "${paths[@]}" </dev/tty 2>/dev/tty)
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
