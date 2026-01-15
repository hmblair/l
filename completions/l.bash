_l() {
    local cur prev opts
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    # Options that take arguments
    case "$prev" in
        -d|--depth)
            return 0  # User provides depth number
            ;;
        -f|--filter)
            return 0  # User provides pattern
            ;;
    esac

    # Complete options
    if [[ "$cur" == -* ]]; then
        opts="-a -l --long -s --short -t --tree -d --depth -p --path
              -e --expand-all --list --no-icons -c --color-all -g
              -f --filter -i --interactive -S -T -N -r
              -h --help --version --daemon"
        COMPREPLY=( $(compgen -W "$opts" -- "$cur") )
        return 0
    fi

    # Complete files/directories, including hidden if -a was specified
    if [[ " ${COMP_WORDS[*]} " =~ " -"[^\ ]*"a" ]] || [[ " ${COMP_WORDS[*]} " =~ " -a " ]]; then
        COMPREPLY=( $(compgen -A file -o plusdirs -- "$cur") )
    else
        COMPREPLY=( $(compgen -A file -o plusdirs -X '.*' -- "$cur") )
    fi
}

complete -F _l l
complete -F _l cl
