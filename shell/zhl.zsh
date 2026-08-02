# zhl.zsh — self-contained zsh syntax highlighter.
#
# Source from .zshrc. Replaces zsh-syntax-highlighting: highlights the command
# line via a zle-line-pre-redraw hook, with no external dependencies.
#
# Structure: _zhl_spans is the core (string in, spans out), with no zle
# knowledge. Three consumers sit on top: _zhl_highlight applies spans to the
# live buffer via region_highlight; _zhl_ansi renders any string with ANSI
# escapes (used by the l-history pane); and _zhl_vcwd exposes the parser's
# end state — the virtual cwd and command/arg position — which the l Tab
# widget uses to complete relative to cd-chains.
#
# The parser is static analysis only — it never executes any part of the
# buffer. It tracks a "virtual cwd" through cd/pushd so that in a chain like
# `cd build && ./run`, ./run is validated against build/, not $PWD. Targets it
# cannot resolve statically (command substitutions, unknown expansions) poison
# the virtual cwd to "unknown", after which cwd-dependent words are styled as
# unknowable (yellow) rather than falsely valid or invalid. Unrecognized
# constructs are left unstyled — the parser recognizes what it can and stays
# silent otherwise.
#
# Hamish M. Blair

zmodload zsh/parameter

# Style map: span keys -> region_highlight style strings. Green = valid,
# red = invalid, yellow = statically unknowable. Empty value = no styling.
# Defined only if the user has not already customized it.
if (( ! ${+ZHL_STYLES} )); then
  typeset -gA ZHL_STYLES=(
    command          fg=green
    builtin          fg=green
    function         fg=green
    alias            fg=green
    reserved         fg=magenta
    precommand       fg=green,underline
    command-missing  fg=red
    command-unknown  fg=yellow
    assignment       ''
    option           fg=cyan
    path             ''
    path-dir         fg=blue
    path-missing     fg=red
    string           fg=yellow
    comment          fg=8
  )
fi

# Statically expand a word to a filesystem path, or fail if that would require
# running code. Handles simple quoting, leading ~, and plain $var/${var}
# expansions of set parameters. REPLY = expanded word on success.
_zhl_expand() {
  emulate -L zsh
  setopt localoptions extendedglob
  local w="$1" name
  local -i i=0
  # Fully single-quoted: literal, no further expansion applies.
  if [[ "$w" == \'*\' && "${w[2,-2]}" != *\'* ]]; then
    REPLY="${w[2,-2]}"
    return 0
  fi
  [[ "$w" == \"*\" ]] && w="${w[2,-2]}"
  # Anything that needs evaluation or globbing is opaque.
  [[ "$w" == *('$('|\`)* ]] && return 1
  [[ "$w" == *[\*\?]* || "$w" == *\[*\]* ]] && return 1
  if [[ "$w" == '~'* ]]; then
    if [[ "$w" == '~' ]]; then w="$HOME"
    elif [[ "$w" == '~/'* ]]; then w="$HOME${w[2,-1]}"
    else return 1                      # ~user — not resolved statically
    fi
  fi
  while [[ "$w" == *'$'* ]]; do
    (( ++i > 8 )) && return 1          # value re-introduced '$'; give up
    if [[ "$w" == (#b)*'${'([A-Za-z_][A-Za-z0-9_]#)'}'* ]]; then
      name="${match[1]}"
      (( ${+parameters[$name]} )) || return 1
      w="${w/\$\{$name\}/${(P)name}}"
    elif [[ "$w" == (#b)*'$'([A-Za-z_][A-Za-z0-9_]#)* ]]; then
      name="${match[1]}"
      (( ${+parameters[$name]} )) || return 1
      w="${w/\$$name/${(P)name}}"
    else
      return 1                         # ${...} form we don't model
    fi
  done
  REPLY="$w"
  return 0
}

# Classify a word in command position via the zsh/parameter tables (no forks).
_zhl_command_kind() {
  local w="$1"
  if (( ${+aliases[$w]} )); then REPLY=alias
  elif [[ -n "${reswords[(r)$w]}" ]]; then REPLY=reserved
  elif (( ${+builtins[$w]} )); then REPLY=builtin
  elif (( ${+functions[$w]} )); then REPLY=function
  elif (( ${+commands[$w]} )); then REPLY=command
  else REPLY=none
  fi
}

# Pure highlighter: parse $1 and fill reply with "start end key" spans, where
# start/end are 0-based character offsets (end exclusive) and key indexes
# ZHL_STYLES. Never executes any part of the input.
_zhl_spans() {
  emulate -L zsh
  setopt localoptions extendedglob
  reply=()
  # The virtual cwd for resolving relative paths, and the parse position at
  # the end of the input (command vs arg), are published as globals so
  # _zhl_vcwd (and through it the l Tab widget) can read the end state; an
  # empty or oversized buffer leaves them at $PWD / command position.
  typeset -g _zhl_vcwd_path="$PWD"
  typeset -gi _zhl_vcwd_known=1
  typeset -g _zhl_end_state=command
  local buf="$1"
  local -i len=${#buf}
  (( len == 0 || len > 4096 )) && return 0

  local -a tokens
  tokens=(${(Z+c+)buf})

  # state: command = next word is a command; arg = inside a command's args.
  # pending_cd: saw cd/pushd, waiting for its target argument.
  local state=command tok key p
  local -i pending_cd=0 pos=1 start tlen is_string
  local REPLY

  for tok in "${tokens[@]}"; do
    while (( pos <= len )) && [[ "${buf[pos]}" == [[:space:]] ]]; do (( pos++ )); done
    (( pos > len )) && break
    tlen=${#tok}
    if [[ "${buf[pos,pos+tlen-1]}" != "$tok" ]]; then
      # The tokenizer transformed the text (unclosed quote, heredoc, ...).
      # Style a trailing in-progress string, then stop rather than guess.
      [[ "$tok" == [\'\"]* || "$tok" == '$'\'* ]] && reply+=("$(( pos - 1 )) $len string")
      _zhl_end_state="$state"
      return 0
    fi
    start=$(( pos - 1 ))
    (( pos += tlen ))

    case "$tok" in
      '&&'|'||'|';'|'|'|'|&'|';;'|'&'|'&!'|'&|')
        # A cd that never received an argument goes to $HOME.
        if (( pending_cd )); then
          _zhl_vcwd_path="$HOME" _zhl_vcwd_known=1 pending_cd=0
        fi
        state=command
        continue ;;
    esac

    if [[ "$tok" == '#'* ]]; then
      reply+=("$start $len comment")
      _zhl_end_state="$state"
      return 0
    fi

    # Redirection operators; their target is handled as a normal argument.
    if [[ "$tok" == ([0-9]#(\<|\>)(\>|)(\||\&[0-9]#|)|\&\>(\>|)) ]]; then
      continue
    fi

    # Quoted words get string styling, but still participate in state
    # tracking below (a quoted cd target must update the virtual cwd).
    is_string=0
    if [[ "$tok" == [\'\"]* || "$tok" == '$'\'* ]]; then
      is_string=1
      reply+=("$start $(( start + tlen )) string")
    fi

    if [[ "$state" == command ]]; then
      if (( is_string )); then
        state=arg
        continue
      fi
      # VAR=value prefixes leave the next word in command position.
      if [[ "$tok" == [A-Za-z_][A-Za-z0-9_]#=* ]]; then
        reply+=("$start $(( start + tlen )) assignment")
        continue
      fi
      case "$tok" in
        sudo|doas|command|exec|nohup|env)
          reply+=("$start $(( start + tlen )) precommand")
          continue ;;
      esac
      if [[ "$tok" == */* ]]; then
        # Path to an executable: resolve against the virtual cwd.
        if [[ "$tok" != /* && "$tok" != '~'* ]] && (( ! _zhl_vcwd_known )); then
          key=command-unknown
        elif _zhl_expand "$tok"; then
          p="$REPLY"
          [[ "$p" != /* ]] && p="$_zhl_vcwd_path/$p"
          if [[ -x "${p:A}" && ! -d "${p:A}" ]]; then key=command
          else key=command-missing
          fi
        else
          key=command-unknown
        fi
        reply+=("$start $(( start + tlen )) $key")
        state=arg
        continue
      fi
      _zhl_command_kind "$tok"
      key="$REPLY"
      [[ "$key" == none ]] && key=command-missing
      reply+=("$start $(( start + tlen )) $key")
      if [[ "$REPLY" == reserved ]]; then
        # Most reserved words are followed by another command position;
        # these introduce non-command words instead.
        case "$tok" in
          for|select|case|function|foreach|repeat) state=arg ;;
        esac
        continue
      fi
      [[ "$tok" == (cd|pushd) ]] && pending_cd=1
      state=arg
      continue
    fi

    # Argument position. cd's target comes first so `cd -` and quoted
    # targets are tracked before generic option/path handling.
    if (( pending_cd )); then
      if [[ "$tok" == '-' ]]; then
        pending_cd=0
        if (( ${+parameters[OLDPWD]} )); then _zhl_vcwd_path="$OLDPWD"
        else _zhl_vcwd_known=0
        fi
        continue
      fi
      if [[ "$tok" == -* ]]; then
        reply+=("$start $(( start + tlen )) option")
        continue                       # cd -P etc.: still awaiting the target
      fi
      pending_cd=0
      if [[ "$tok" != /* && "$tok" != '~'* && $is_string == 0 ]] && (( ! _zhl_vcwd_known )); then
        continue                       # relative target of an unknown cwd
      fi
      if _zhl_expand "$tok"; then
        p="$REPLY"
        if [[ "$p" != /* ]]; then
          (( _zhl_vcwd_known )) || continue
          p="$_zhl_vcwd_path/$p"
        fi
        p="${p:A}"
        if [[ -d "$p" ]]; then
          _zhl_vcwd_path="$p" _zhl_vcwd_known=1  # the cd will succeed; follow it
          (( is_string )) || reply+=("$start $(( start + tlen )) path-dir")
        else
          # The cd will fail: vcwd is unchanged for `;`-chains, and nothing
          # after runs in `&&`-chains — either way keep the current vcwd.
          (( is_string )) || reply+=("$start $(( start + tlen )) path-missing")
        fi
      else
        _zhl_vcwd_known=0              # cd $(...) and friends
      fi
      continue
    fi
    (( is_string )) && continue
    if [[ "$tok" == -* ]]; then
      reply+=("$start $(( start + tlen )) option")
      continue
    fi
    # Plain argument: style it if it names an existing path (dirs in blue).
    if _zhl_expand "$tok"; then
      p="$REPLY"
      if [[ "$p" != /* && "$p" != '~'* ]]; then
        (( _zhl_vcwd_known )) || continue
        p="$_zhl_vcwd_path/$p"
      fi
      p="${p:A}"
      if [[ -d "$p" ]]; then
        reply+=("$start $(( start + tlen )) path-dir")
      elif [[ -e "$p" ]]; then
        reply+=("$start $(( start + tlen )) path")
      fi
    fi
  done
  _zhl_end_state="$state"
  return 0
}

# Convert a ZHL_STYLES value ("fg=green,underline") to an SGR parameter
# string in REPLY (empty when the style carries no rendering).
_zhl_sgr() {
  local part
  local -a codes
  for part in ${(s:,:)1}; do
    case "$part" in
      fg=black)   codes+=(30) ;;
      fg=red)     codes+=(31) ;;
      fg=green)   codes+=(32) ;;
      fg=yellow)  codes+=(33) ;;
      fg=blue)    codes+=(34) ;;
      fg=magenta) codes+=(35) ;;
      fg=cyan)    codes+=(36) ;;
      fg=white)   codes+=(37) ;;
      fg=<->)     codes+=(38 5 "${part#fg=}") ;;
      bold)       codes+=(1) ;;
      underline)  codes+=(4) ;;
    esac
  done
  REPLY="${(j:;:)codes}"
}

# Render $1 with ANSI escapes from its highlight spans; REPLY = the colored
# string. Lets non-zle surfaces (e.g. the ^R history pane) reuse the exact
# live-buffer highlighting.
_zhl_ansi() {
  emulate -L zsh
  local buf="$1" out="" span key _sgr
  local -i last=0 a b
  local -a reply
  _zhl_spans "$buf"
  for span in "${reply[@]}"; do
    a=${span%% *}
    b=${${span#* }%% *}
    key=${span##* }
    _zhl_sgr "${ZHL_STYLES[$key]}"
    _sgr="$REPLY"
    out+="${buf[last+1,a]}"
    if [[ -n "$_sgr" ]]; then
      out+=$'\e['"${_sgr}m${buf[a+1,b]}"$'\e[0m'
    else
      out+="${buf[a+1,b]}"
    fi
    last=$b
  done
  out+="${buf[last+1,-1]}"
  REPLY="$out"
}

# Resolve the virtual cwd a command line establishes for whatever follows it
# (e.g. `cd build && ` resolves to .../build). REPLY = directory on success;
# returns 1 when the cwd is not statically determinable.
_zhl_vcwd() {
  local -a reply
  _zhl_spans "$1"
  (( _zhl_vcwd_known )) || return 1
  REPLY="$_zhl_vcwd_path"
  return 0
}

# zle adapter: apply the spans to the live buffer. Owns only its own entries
# in region_highlight (tagged memo=zhl) so other plugins' regions survive.
_zhl_highlight() {
  emulate -L zsh
  local -a reply
  local span style
  _zhl_spans "$BUFFER" 2>/dev/null
  region_highlight=("${(@)region_highlight:#*memo=zhl}")
  for span in "${reply[@]}"; do
    style="${ZHL_STYLES[${span##* }]}"
    [[ -n "$style" ]] && region_highlight+=("${span% *} $style memo=zhl")
  done
  return 0
}

if [[ -o zle ]]; then
  autoload -Uz add-zle-hook-widget
  add-zle-hook-widget zle-line-pre-redraw _zhl_highlight
fi
