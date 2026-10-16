#!/bin/sh
# Turn a Dragon Quest IX ROM into the PC game. Run by BUILD.bat inside MSYS2
# (UCRT64); also works on Linux with gcc, make, python3 (+ PyYAML) and SDL2.
#
#   tools/build-game.sh <rom.nds>
#
# Everything is made inside this folder:
#   build/   working files (the extracted code, the generated C, objects)
#   game/    the result: dqix.exe and everything it needs, including a copy
#            of the ROM, so the folder can be moved anywhere
set -e
cd "$(dirname "$0")/.."
ROM="$1"
[ -f "$ROM" ] || { echo "No ROM given (or it doesn't exist): $ROM"; exit 1; }

echo "== Checking the ROM"
SUM=$(sha1sum "$ROM" | cut -d' ' -f1)
case "$SUM" in
c7c3014c237900c8281289b8bc76a781969b6278) echo "   Dragon Quest IX (USA): OK" ;;
*) echo "   This isn't the Dragon Quest IX ROM this build supports."
   echo "   Expected the USA version (SHA-1 c7c3014c237900c8281289b8bc76a781969b6278), got $SUM."
   echo "   Dump it from your own cartridge; the European and Japanese versions are not supported yet."
   exit 1 ;;
esac

PY=python3; command -v $PY >/dev/null 2>&1 || PY=python
echo "== Extracting the game's code"
$PY tools/extract_rom.py "$ROM" build/extract
echo "== Recompiling it to C (about a minute)"
$PY recomp/build.py --extract build/extract --config config/usa/arm9 --out build/recomp --hle hle.txt --interpret interpret.txt --patch patches.txt
$PY recomp/arm7.py --extract build/extract --out build/recomp7 --hle hle7.txt

echo "== Compiling (the first time takes a while; later builds only redo what changed)"
mkdir -p game
case "$(uname -s)" in
MINGW*|MSYS*)
    EXE=game/dqix.exe
    make -j"$(nproc)" -f build.mk OS=Windows_NT GEN=build/recomp GEN7=build/recomp7 OBJ=build/obj FRONTEND=sdl OUT="$EXE" \
         CFLAGS="-O1 -w" DEFS=-DRECOMP_TRACE SDL_CFLAGS="$(sdl2-config --cflags)" SDL_LIBS="-lSDL2" \
         LDFLAGS="-static-libgcc -Wl,--stack,67108864"
    cp /ucrt64/bin/SDL2.dll game/ ;;
*)
    EXE=game/dqix
    make -j"$(nproc)" -f build.mk GEN=build/recomp GEN7=build/recomp7 OBJ=build/obj FRONTEND=sdl OUT="$EXE" \
         CFLAGS="-O1 -w" DEFS=-DRECOMP_TRACE SDL_CFLAGS="${SDL_CFLAGS:-$(sdl2-config --cflags)}" SDL_LIBS="${SDL_LIBS:-$(sdl2-config --libs)}" ;;
esac

echo "== Putting the game folder together"
mkdir -p game/data game/skins
rm -rf game/data/code && cp -r build/extract game/data/code     # the overlays (the game checks which is loaded)
if ! cmp -s "$ROM" game/data/dqix.nds; then cp "$ROM" game/data/dqix.nds; fi
cp skins/*.png skins/*.txt game/skins/ 2>/dev/null || true
# optional files: the built-in Wi-Fi server's downloads and DQVC schedule
for src in extras/wfc wfc; do
    for g in "$src"/*/; do
        [ -f "$g/_list.txt" ] || continue
        mkdir -p "game/wfc/$(basename "$g")" && cp "$g"* "game/wfc/$(basename "$g")/"
    done
done
if [ -d game/wfc ]; then echo "   Wireless downloads (DQVC, quests): included"
else echo "   Wireless downloads not found (optional: put them in the extras folder and build again)"; fi
cat > game/play.bat <<'BAT'
@echo off
rem Dragon Quest IX (PC). Keys: arrows = D-pad, X = A, Z = B, S = X, A = Y, Q = L, W = R,
rem Enter = START, Right Shift = SELECT, mouse = touch screen, Tab = fast forward.
rem Every setting is in the menu bar at the top of the window.
cd /d "%~dp0"
dqix.exe data\dqix.nds data\code 0 1 dqix.sav
if errorlevel 1 pause
BAT
cat > game/play.sh <<'SH'
#!/bin/sh
cd "$(dirname "$0")" && exec ./dqix data/dqix.nds data/code 0 1 dqix.sav
SH
chmod +x game/play.sh
echo
echo "== Done. The game is in the 'game' folder: double-click play.bat."
