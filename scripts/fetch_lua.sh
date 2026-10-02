#!/usr/bin/env sh
# scripts/fetch_lua.sh - one-time vendoring of Lua 5.4 for the on-device VM.
#
# Downloads the official Lua source, copies the interpreter sources into
# lib/lua/src/ and flips -DPORK_LUA=1 in platformio.ini. Run it once, then:
#
#     pio run -t upload
#
# Usage:  bash scripts/fetch_lua.sh
#         LUA_VERSION=5.4.6 bash scripts/fetch_lua.sh   (pin another 5.4.x)
#
# Needs one of curl/wget, plus tar and sed. Works on Linux, macOS and Termux.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd)
DEST="$ROOT/lib/lua/src"
INI="$ROOT/platformio.ini"
VERSION="${LUA_VERSION:-5.4.7}"

# The standalone interpreter (lua.c) and compiler (luac.c) each define main()
# and would fight the Arduino core, so they stay out. Everything else is the
# library set Lua itself compiles when embedded.
SKIP=" lua.c luac.c onelua.c "

echo "[lua] 0N3P0rK - vendoring Lua $VERSION into lib/lua/src"

TMP=$(mktemp -d 2>/dev/null || echo "$ROOT/.lua-fetch.$$")
mkdir -p "$TMP" "$DEST"
trap 'rm -rf "$TMP"' EXIT INT TERM

URL="https://www.lua.org/ftp/lua-$VERSION.tar.gz"
echo "[lua] downloading $URL"
if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$URL" -o "$TMP/lua.tar.gz"
elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$TMP/lua.tar.gz" "$URL"
else
    echo "[lua] ERROR: need curl or wget on PATH" >&2
    exit 1
fi

echo "[lua] unpacking"
tar -xzf "$TMP/lua.tar.gz" -C "$TMP"

SRC="$TMP/lua-$VERSION/src"
if [ ! -d "$SRC" ]; then
    SRC=$(find "$TMP" -maxdepth 2 -type d -name src | head -n 1)
fi
if [ ! -d "$SRC" ]; then
    echo "[lua] ERROR: no src/ folder inside the archive" >&2
    exit 1
fi

copied=0
for f in "$SRC"/*.c "$SRC"/*.h; do
    [ -f "$f" ] || continue
    b=$(basename "$f")
    case "$SKIP" in
        *" $b "*) continue ;;
    esac
    cp "$f" "$DEST/$b"
    copied=$((copied + 1))
done
echo "[lua] copied $copied files to lib/lua/src"

if [ ! -f "$DEST/lua.h" ] || [ ! -f "$DEST/lapi.c" ]; then
    echo "[lua] ERROR: lua.h / lapi.c missing - archive layout unexpected" >&2
    exit 1
fi

# Flip the build flag from its off-by-default state to on, exactly once.
if grep -q '^[[:space:]]*;-DPORK_LUA=1' "$INI" 2>/dev/null; then
    sed 's/^[[:space:]]*;-DPORK_LUA=1/    -DPORK_LUA=1/' "$INI" > "$INI.tmp"
    mv "$INI.tmp" "$INI"
    echo "[lua] enabled -DPORK_LUA=1 in platformio.ini"
elif grep -q '^[[:space:]]*-DPORK_LUA=1' "$INI" 2>/dev/null; then
    echo "[lua] -DPORK_LUA=1 already enabled"
else
    echo "[lua] WARNING: could not find the -DPORK_LUA=1 line - add it by hand" >&2
fi

echo "[lua] done.  Now run:  pio run -t upload"
echo "[lua] then copy examples/scripts/*.lua to /0N3P0rK/scripts/ on the card"