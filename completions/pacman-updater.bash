_pacman_updater_complete()
{
	local cur prev opts
	COMPREPLY=()
	cur=${COMP_WORDS[COMP_CWORD]}
	prev=${COMP_WORDS[COMP_CWORD - 1]}

	if [[ $prev == --config ]]; then
		COMPREPLY=( $(compgen -f -- "$cur") )
		return 0
	fi

	case ${COMP_WORDS[0]} in
		pacman-updater)
			opts='--dry-run --self-test --force-update --config'
			;;
		pacman-updater-check)
			opts='--config --wait-cpu --cpu-window'
			;;
		*)
			return 0
			;;
	esac
	COMPREPLY=( $(compgen -W "$opts" -- "$cur") )
}

complete -F _pacman_updater_complete pacman-updater
complete -F _pacman_updater_complete pacman-updater-check
