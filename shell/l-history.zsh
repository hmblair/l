# l-history.zsh — visible-list incremental history search on ^R.
#
# Source from .zshrc. Replaces history-incremental-search-backward with a
# picker that shows the matching history entries while you type. The list is
# drawn below the prompt with raw escapes (zle -M cannot render colors), and
# each visible entry is syntax-highlighted by zhl's ANSI adapter when the
# highlighter is loaded — the same colors the command gets once accepted.
#
# Keys: printable characters filter (every space-separated term must match,
# case-insensitively); ^R / Down move to older matches, ^S / Up to newer;
# Enter/Tab accept the selection into the buffer (without running it);
# Esc and ^G cancel and keep the original line; ^C restores the display and
# then aborts the line, as it does anywhere in zsh.
#
# Hamish M. Blair

zmodload zsh/parameter

# Rows of history shown below the query line.
typeset -gi L_HISTORY_ROWS=${L_HISTORY_ROWS:-10}

_l_history() {
  emulate -L zsh
  setopt localoptions extendedglob

  local orig_buffer="$BUFFER"
  local -i orig_cursor=$CURSOR

  # History entries, newest first, first occurrence wins. $history holds real
  # newlines inside multi-line entries, unlike fc -l output.
  local -a hist
  local k
  for k in ${(On)${(k)history}}; do
    hist+=("${history[$k]}")
  done
  hist=(${(u)hist})

  local query="" key seq raw disp REPLY
  local -a matches lines terms
  local -i sel=1 lo hi i n width accept=0

  # The pane starts on the line below the prompt; every frame reprints from
  # the pane top, clearing what the previous frame left behind. The terminal
  # cursor is hidden while the pane is open (the query line draws its own
  # caret), mirroring l -i.
  print -n "\n\e[?25l" > /dev/tty

  # The always block guarantees the pane is cleared and the cursor restored
  # even when ^C aborts the widget mid-read (a plain cleanup line after the
  # loop would never run in that case).
  {

  while true; do
    # Filter: every space-separated term must appear somewhere in the entry.
    matches=("${hist[@]}")
    terms=(${(s: :)query})
    for k in "${terms[@]}"; do
      matches=(${(M)matches:#(#i)*"$k"*})
    done
    n=${#matches}
    (( sel > n )) && sel=n
    (( sel < 1 )) && sel=1

    # Build the frame in l's structural language: grey rails down the left,
    # the query in the top corner with its own caret, the match count in the
    # closing corner, and a window of L_HISTORY_ROWS syntax-highlighted
    # entries around the selection, newest at the top. A one-space left
    # margin aligns the frame with the prompt.
    local count="${n}/${#hist}"
    lines=(" "$'\e[90m┌─\e[0m '"${query}"$'\e[90m▏\e[0m')
    if (( n == 0 )); then
      lines+=(" "$'\e[90m│  (no matches)\e[0m')
    else
      lo=$(( sel - L_HISTORY_ROWS / 2 ))
      (( lo < 1 )) && lo=1
      hi=$(( lo + L_HISTORY_ROWS - 1 ))
      (( hi > n )) && { hi=n; lo=$(( hi - L_HISTORY_ROWS + 1 )); (( lo < 1 )) && lo=1 }
      width=$(( COLUMNS - 8 ))
      for (( i=lo; i<=hi; i++ )); do
        # Truncate before colorizing so escape codes never count as width.
        raw="${matches[$i]//$'\n'/ }"
        raw="${raw[1,$width]}"
        if (( $+functions[_zhl_ansi] )); then
          _zhl_ansi "$raw"
          disp="$REPLY"
        else
          disp="$raw"
        fi
        # Same selection cursor as the l picker: the config.toml cursor glyph
        # in l -i's cursor color (COLOR_CYAN, \033[0;36m).
        if (( i == sel )); then
          lines+=(" "$'\e[90m│\e[0m \e[0;36m❯\e[0m '"${disp}")
        else
          lines+=(" "$'\e[90m│\e[0m   '"${disp}")
        fi
      done
    fi
    lines+=(" "$'\e[90m└─ '"${count}"$'\e[0m')
    {
      print -n "\r"
      for k in "${lines[@]}"; do
        print -rn -- "$k"$'\e[K\n'
      done
      print -rn -- $'\e[J'
      print -rn -- $'\e['"${#lines}A"
    } > /dev/tty

    if ! read -k 1 key; then
      break
    fi
    case "$key" in
      $'\C-r') (( sel < n )) && (( sel++ )) ;;
      $'\C-s') (( sel > 1 )) && (( sel-- )) ;;
      $'\r'|$'\n'|$'\t')
        accept=1
        break ;;
      $'\C-g'|$'\C-c')
        break ;;
      $'\C-?'|$'\C-h')
        query="${query[1,-2]}"
        sel=1 ;;
      $'\C-u')
        query=""
        sel=1 ;;
      $'\e')
        # Escape sequence (arrows) or a bare Esc = cancel.
        seq=""
        while read -k 1 -t 0.005 k; do seq+="$k"; done
        case "$seq" in
          '[A') (( sel > 1 )) && (( sel-- )) ;;          # up: newer
          '[B') (( sel < n )) && (( sel++ )) ;;          # down: older
          '') break ;;
        esac ;;
      *)
        if [[ "$key" == [[:print:]] ]]; then
          query+="$key"
          sel=1
        fi ;;
    esac
  done

  } always {
    # Clear the pane, restore the terminal cursor, return to the prompt line,
    # and let zle redraw it. An interrupted search behaves like a cancel.
    print -n "\033[J\033[A\r\033[K\033[?25h" > /dev/tty
    if (( accept && n )); then
      BUFFER="${matches[$sel]}"
      CURSOR=${#BUFFER}
    else
      BUFFER="$orig_buffer"
      CURSOR=$orig_cursor
    fi
    (( $+functions[_zhl_highlight] )) && _zhl_highlight
    zle reset-prompt
  }
}

zle -N _l_history
bindkey '^R' _l_history
