#!/usr/bin/env bash

VER="${1:?version required (e.g. v1.2.3)}"
: "${PFX_PASS:?set PFX_PASS (pkcs12 password)}"
: "${GITEA_TOKEN:?set GITEA_TOKEN (gitea access token)}"

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT" || exit

PFX="../cert.pfx"
API="https://git.yokai.digital/api/v1/repos/deadYokai/cu3ml"
TC64="/opt/msvc/cmake/toolchain-x64.cmake"
TC86="/opt/msvc/cmake/toolchain-x86.cmake"
DIST="dist"

[ -f "$PFX" ] || { echo "cert not found: $PFX" >&2; exit 1; }
mkdir -p "$DIST"

build() {
    cmake -B "$1" -DCMAKE_TOOLCHAIN_FILE="$2" -DCMAKE_CROSSCOMPILING_EMULATOR=/usr/bin/wine -DCMAKE_BUILD_TYPE=Release
    cmake --build "$1" --config Release -j
}

sign() {
    osslsigncode -pkcs12 "$PFX" -pass "$PFX_PASS" -h sha256 \
        -in "$1" -out "$2"
}

echo ">> build x64"
build build   "$TC64"
echo ">> build x86"
build build86 "$TC86"

echo ">> sign"
sign build/dinput8.dll   "$DIST/dinput8_x64.dll"
sign build86/dinput8.dll "$DIST/dinput8_x86.dll"

echo ">> release $VER"
SHA="$(git rev-parse HEAD)"
RID="$(curl -fsSL -X POST "$API/releases" \
    -H "Authorization: token $GITEA_TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"tag_name\":\"$VER\",\"name\":\"$VER\",\"target_commitish\":\"$SHA\"}" \
    | jq -r '.id')"
[ -n "$RID" ] && [ "$RID" != null ] || { echo "release create failed" >&2; exit 1; }

for f in dinput8_x64.dll dinput8_x86.dll; do
    echo ">> upload $f"
    curl -fsSL -X POST "$API/releases/$RID/assets?name=$f" \
        -H "Authorization: token $GITEA_TOKEN" \
        -F "attachment=@$DIST/$f" >/dev/null
done

echo ">> done: $VER (release $RID)"
