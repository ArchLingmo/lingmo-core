#!/bin/bash
# End-to-end checks of lingmo-userdirs in throw-away homes (never the real one).
#   usage: tests/cli-test.sh <path to lingmo-userdirs>
# The cross-filesystem cases mount small tmpfs folders inside an unprivileged
# user+mount namespace (unshare -rm); they are skipped when that is not allowed.
set -u
BIN=$(realpath "${1:?usage: cli-test.sh <lingmo-userdirs>}")
ROOT=$(mktemp -d "${TMPDIR:-/tmp}/userdirs-cli-XXXXXX")
fail=0
ok() { echo "ok   - $*"; }
bad() { echo "FAIL - $*"; fail=1; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

newhome() { # <name>: a home like the one xdg-user-dirs-update makes in English
    H="$ROOT/$1"; mkdir -p "$H/.config" "$H/.local/share"
    cat > "$H/.config/user-dirs.dirs" <<'EOF'
XDG_DESKTOP_DIR="$HOME/Desktop"
XDG_DOWNLOAD_DIR="$HOME/Downloads"
XDG_TEMPLATES_DIR="$HOME/Templates"
XDG_PUBLICSHARE_DIR="$HOME/Public"
XDG_DOCUMENTS_DIR="$HOME/Documents"
XDG_MUSIC_DIR="$HOME/Music"
XDG_PICTURES_DIR="$HOME/Pictures"
XDG_VIDEOS_DIR="$HOME/Videos"
XDG_PROJECTS_DIR="$HOME/"
EOF
    printf en_US > "$H/.config/user-dirs.locale"
    mkdir -p "$H"/{Desktop,Downloads,Templates,Public,Documents,Music,Pictures,Videos}
}
run() { HOME="$H" XDG_CONFIG_HOME="$H/.config" XDG_DATA_HOME="$H/.local/share" "$BIN" "$@"; }

# 1. The situation xdg-user-dirs-gtk leaves behind: English folders in the
#    config, Portuguese twins next to them
newhome dup
mkdir -p "$H/Documentos" "$H/Vídeos"
echo en > "$H/Documents/notes.txt";  echo pt > "$H/Documentos/notes.txt"
echo a > "$H/Videos/a.mp4";          echo b > "$H/Vídeos/b.mp4"
echo m > "$H/Music/m.ogg";           mkdir -p "$H/Músicas/Rock"; echo r > "$H/Músicas/Rock/r.ogg"
mkdir -p "$H/Imagens" "$H/Projetos"
mkdir -p "$H/.config/gtk-3.0"
printf 'file://%s/Videos\nfile://%s/V%%C3%%ADdeos\n' "$H" "$H" > "$H/.config/gtk-3.0/bookmarks"
check "4 duplicate folders found" '[ "$(run --count-duplicates)" = 4 ]'
before=$(cd "$H" && find . -path ./.local -prune -o -print | sort | md5sum)
run --dry-run --duplicates > "$ROOT/dry.txt"
check "dry run changes nothing" '[ "$(cd "$H" && find . -path ./.local -prune -o -print | sort | md5sum)" = "$before" ]'
check "dry run shows the clash name" 'grep -q "Documents/notes (2).txt" "$ROOT/dry.txt"'
run --apply --duplicates > "$ROOT/apply.txt"
check "merged into the configured folders" '[ "$(cat "$H/Documents/notes.txt")" = en ] && [ "$(cat "$H/Documents/notes (2).txt")" = pt ]'
check "twins removed" '[ ! -e "$H/Documentos" ] && [ ! -e "$H/Vídeos" ] && [ ! -e "$H/Músicas" ] && [ ! -e "$H/Imagens" ]'
check "nested folder moved" '[ "$(cat "$H/Music/Rock/r.ogg")" = r ]'
check "Projetos left alone (XDG_PROJECTS_DIR points at home)" '[ -d "$H/Projetos" ]'
check "bookmarks deduplicated" '[ "$(wc -l < "$H/.config/gtk-3.0/bookmarks")" = 1 ]'
run --undo > "$ROOT/undo.txt"
check "undo brings the twins back" '[ "$(cat "$H/Documentos/notes.txt")" = pt ] && [ "$(cat "$H/Vídeos/b.mp4")" = b ] && [ ! -e "$H/Documents/notes (2).txt" ]'
check "undo restores bookmarks" '[ "$(wc -l < "$H/.config/gtk-3.0/bookmarks")" = 2 ]'

# 2. Language change: rename to Portuguese, merging the twins
run --apply --locale pt_BR > "$ROOT/lang.txt"
check "renamed to Portuguese" '[ -d "$H/Área de trabalho" ] && [ -d "$H/Modelos" ] && [ -d "$H/Público" ] && [ ! -e "$H/Desktop" ]'
check "merged into Documentos" '[ "$(cat "$H/Documentos/notes.txt")" = pt ] && [ "$(cat "$H/Documentos/notes (2).txt")" = en ]'
check "config points at Portuguese names" 'grep -q "XDG_VIDEOS_DIR=\"\$HOME/Vídeos\"" "$H/.config/user-dirs.dirs" && grep -q "XDG_PROJECTS_DIR=\"\$HOME/\"" "$H/.config/user-dirs.dirs"'
check "user-dirs.locale written" '[ "$(cat "$H/.config/user-dirs.locale")" = pt_BR ]'
check "no duplicates left" '[ "$(run --count-duplicates)" = 0 ]'

# 3. Other filesystems (needs unprivileged user namespaces)
if unshare -rm true 2>/dev/null; then
    newhome space
    head -c 6M /dev/urandom > "$H/Videos/big.bin"
    mkdir -p "$H/Vídeos"
    out=$(unshare -rm bash -c '
        mount -t tmpfs -o size=2m none "$1/Vídeos" && echo keep > "$1/Vídeos/k" &&
        HOME="$1" XDG_CONFIG_HOME="$1/.config" XDG_DATA_HOME="$1/.local/share" "$2" --apply --locale pt_BR;
        ls "$1/Vídeos"' _ "$H" "$BIN")
    check "refused: not enough space on the other filesystem" 'echo "$out" | grep -q "skip .*Videos.*reason" && [ -f "$H/Videos/big.bin" ]'
    check "config keeps the folder that was not moved" 'grep -q "XDG_VIDEOS_DIR=\"\$HOME/Videos\"" "$H/.config/user-dirs.dirs"'

    newhome xdev
    out=$(unshare -rm bash -c '
        mount -t tmpfs -o size=64m none "$1/Music" || exit 1
        echo song > "$1/Music/song.ogg"; chmod 444 "$1/Music/song.ogg"
        mkdir -p "$1/Music/album"; echo t > "$1/Music/album/t.ogg"; ln -s album/t.ogg "$1/Music/link.ogg"
        HOME="$1" XDG_CONFIG_HOME="$1/.config" XDG_DATA_HOME="$1/.local/share" "$2" --apply --locale pt_BR
        echo "--- mounted Music now:"; ls -A "$1/Music"' _ "$H" "$BIN")
    check "mount point folder moved entry by entry" '[ "$(cat "$H/Músicas/song.ogg")" = song ] && [ "$(cat "$H/Músicas/album/t.ogg")" = t ] && [ "$(readlink "$H/Músicas/link.ogg")" = album/t.ogg ] && [ ! -w "$H/Músicas/song.ogg" ]'
    check "mount point left empty and reported" 'echo "$out" | grep -q "error: .*Music" && [ -z "$(echo "$out" | sed -n "/--- mounted Music now:/,\$p" | tail -n +2)" ]'
else
    echo "skip - cross-filesystem cases (no user namespaces)"
fi

rm -rf "$ROOT"
exit $fail
