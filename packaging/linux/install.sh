#!/bin/sh
set -eu

usage() {
	printf '%s\n' 'Usage: ./install.sh [--prefix ABSOLUTE_PATH] [--uninstall]'
}

fail() {
	printf 'OpenXR Simulator: %s\n' "$1" >&2
	exit "${2:-1}"
}

data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
prefix="$data_home/openxr-simulator"
uninstall=0
while [ "$#" -gt 0 ]; do
	case "$1" in
		--prefix)
			[ "$#" -ge 2 ] || fail 'Missing path after --prefix.'
			prefix=$2
			shift 2
			;;
		--uninstall) uninstall=1; shift ;;
		--help|-h) usage; exit 0 ;;
		*) usage >&2; fail "Unknown option: $1" ;;
	esac
done

case "$prefix" in
	/*) ;;
	*) fail 'The installation path must be absolute.' ;;
esac
case "$data_home" in
	/*) ;;
	*) fail 'XDG_DATA_HOME must be absolute.' ;;
esac
case "$prefix$data_home" in
	*'
'*) fail 'Installation and desktop paths cannot contain newlines.' ;;
esac

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
parent=$(dirname -- "$prefix")
name=$(basename -- "$prefix")
[ "$name" != '.' ] && [ "$name" != '..' ] && [ "$name" != '/' ] || fail 'Invalid installation path.'
mkdir -p -- "$parent"
parent=$(CDPATH= cd -- "$parent" && pwd -P)
prefix="$parent/$name"
home_dir=$(CDPATH= cd -- "$HOME" && pwd -P)
[ "$prefix" != "$home_dir" ] && [ "$prefix" != '/' ] || fail 'Refusing to replace the home or root directory.'
[ ! -L "$prefix" ] || fail 'The installation path cannot be a symbolic link.'

lock="$parent/.$name.install-lock"
mkdir -- "$lock" 2>/dev/null || fail "Another installation may be running; remove $lock only after checking." 2
temporary=
backup=
desktop_tmp=
installed=0
committed=0
cleanup() {
	if [ -n "$temporary" ] && [ -d "$temporary" ]; then rm -rf -- "$temporary"; fi
	if [ -n "$desktop_tmp" ] && [ -f "$desktop_tmp" ]; then rm -f -- "$desktop_tmp"; fi
	if [ "$committed" -eq 0 ]; then
		if [ "$installed" -eq 1 ] && [ -d "$prefix" ]; then rm -rf -- "$prefix"; fi
		if [ -n "$backup" ] && [ -d "$backup" ] && [ ! -e "$prefix" ]; then mv -- "$backup" "$prefix" || :; fi
	fi
	rmdir -- "$lock" 2>/dev/null || :
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

marker='openxr-simulator-install-v2'
check_in_use() {
	# Executables and the runtime library can be used by separate processes.
	# Inspect the current user's mappings, including applications loading our runtime.
	[ -d /proc/self ] || fail 'Cannot check running processes without /proc.'
	uid=$(id -u)
	for process in /proc/[0-9]*; do
		[ -d "$process" ] || continue
		owner=$(stat -c %u -- "$process" 2>/dev/null) || continue
		[ "$owner" = "$uid" ] || continue
		if [ ! -r "$process/maps" ]; then
			[ ! -d "$process" ] && continue
			fail "Cannot inspect $process; Linux denied the safety check. Installation cannot continue." 2
		fi
		if grep -F -- "$prefix/" "$process/maps" >/dev/null 2>&1; then
			fail "Files in $prefix are in use by process ${process##*/}. Close the simulator and applications using its runtime, then retry." 2
		else
			status=$?
			[ "$status" -eq 1 ] || [ ! -d "$process" ] ||
				fail "Cannot inspect $process; Linux denied the safety check. Installation cannot continue." 2
		fi
	done
}
if [ -e "$prefix" ]; then
	[ -d "$prefix" ] && [ -f "$prefix/.simxr-install" ] || fail "Refusing to replace an unmanaged directory: $prefix"
	installed_marker=$(cat -- "$prefix/.simxr-install")
	case "$installed_marker" in
		"$marker")
			command -v flock >/dev/null 2>&1 || fail 'The flock utility is required for upgrades and uninstall.'
			[ -f "$prefix/.simxr-use-lock" ] && [ ! -L "$prefix/.simxr-use-lock" ] ||
				fail 'The installation use lock is missing or invalid.'
			exec 9< "$prefix/.simxr-use-lock"
			flock -n -x 9 || fail 'The simulator or an application using its runtime is running. Close them and retry.' 2
			;;
		openxr-simulator-install-v1) check_in_use ;;
		*) fail "Refusing to replace an unmanaged directory: $prefix" ;;
	esac
fi

desktop_dir="$data_home/applications"
desktop="$desktop_dir/openxr-simulator.desktop"
if [ "$uninstall" -eq 1 ]; then
	[ -d "$prefix" ] || fail "No installation found at $prefix."
	if [ -f "$desktop" ] && grep -Fqx -- "X-Simxr-Install=$prefix" "$desktop"; then
		rm -- "$desktop"
	fi
	rm -rf -- "$prefix"
	printf 'Removed %s. User settings and recordings were preserved.\n' "$prefix"
	exit 0
fi

[ -x "$source_dir/bin/simxr_companion" ] && [ -f "$source_dir/launch-desktop.sh" ] &&
	[ -f "$source_dir/bin/openxr_simulator.json" ] &&
	[ -f "$source_dir/share/simxr/branding/openxr-simulator.png" ] || fail 'This is not a complete Linux package.'
[ ! -e "$desktop" ] || grep -Fqx -- "X-Simxr-Install=$prefix" "$desktop" ||
	fail "Refusing to replace an unrelated desktop entry: $desktop"
case "$prefix/" in "$source_dir/"*) fail 'Install outside the extracted package.' ;; esac
case "$source_dir/" in "$prefix/"*) fail 'Install from an extracted package outside the existing installation.' ;; esac

temporary=$(mktemp -d -- "$parent/.$name.new.XXXXXXXX")
cp -a -- "$source_dir/." "$temporary/"
printf '%s\n' "$marker" > "$temporary/.simxr-install"
rm -f -- "$temporary/.simxr-use-lock"
: > "$temporary/.simxr-use-lock"
command -v flock >/dev/null 2>&1 || fail 'The flock utility is required for installation.'
exec 8< "$temporary/.simxr-use-lock"
flock -n -x 8 || fail 'Cannot lock the new installation.' 2
mkdir -p -- "$desktop_dir"
desktop_tmp=$(mktemp -- "$desktop_dir/.openxr-simulator.XXXXXXXX")
escape_exec() {
	printf '%s' "$1" | sed 's/\\/\\\\\\\\/g; s/"/\\\\"/g; s/\$/\\\\$/g; s/`/\\\\`/g; s/%/%%/g'
}
escaped_exec=$(escape_exec "$prefix/launch-desktop.sh")
escaped_icon=$(printf '%s' "$prefix/share/simxr/branding/openxr-simulator.png" | sed 's/\\/\\\\/g')
{
	printf '%s\n' '[Desktop Entry]' 'Type=Application' 'Name=OpenXR Simulator'
	printf 'Exec=/bin/sh "%s"\n' "$escaped_exec"
	printf 'Icon=%s\n' "$escaped_icon"
	printf '%s\n' 'Terminal=false' 'Categories=Development;Graphics;'
	printf 'X-Simxr-Install=%s\n' "$prefix"
} > "$desktop_tmp"
chmod 644 -- "$desktop_tmp"
if [ -d "$prefix" ]; then
	if [ "$installed_marker" = 'openxr-simulator-install-v1' ]; then check_in_use; fi
	backup=$(mktemp -d -- "$parent/.$name.old.XXXXXXXX")
	rmdir -- "$backup"
	mv -- "$prefix" "$backup"
fi
mv -- "$temporary" "$prefix"
temporary=
installed=1
mv -f -- "$desktop_tmp" "$desktop"
desktop_tmp=
committed=1
if [ -n "$backup" ]; then rm -rf -- "$backup"; backup=; fi
printf 'Installed OpenXR Simulator in %s.\n' "$prefix"
