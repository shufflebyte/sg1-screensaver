#!/bin/zsh
# Baut den Bildschirmschoner als Universal-Bundle (Apple Silicon + Intel).
#
#   ./build.sh            -> build/Aegyptische Taenzer.saver
#   ./build.sh install    -> zusätzlich nach ~/Library/Screen Savers kopieren
#   ./build.sh snapshot   -> Testwerkzeug build/snapshot (rendert PNGs ohne Bildschirmschoner)
set -euo pipefail
cd "${0:A:h}"

# Dateiname bewusst ohne Umlaute: macOS speichert die Auswahl als URL in zerlegter
# Unicode-Form (A + ¨) und findet ein Bundle mit „Ä“ im Namen dann nicht wieder.
# Der angezeigte Name „Ägyptische Tänzer“ kommt aus Info.plist.
NAME="Aegyptische Taenzer"
OLD_NAME="Ägyptische Tänzer"
EXEC=EgyptSaver
MIN_MACOS=13.0
CORE=../core
BUILD=build

DEV="$(xcode-select -p)"
TC="$DEV/Toolchains/XcodeDefault.xctoolchain/usr/bin"
SDK="$DEV/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"
if [[ ! -x "$TC/swiftc" || ! -d "$SDK" ]]; then
  echo "Xcode wurde nicht gefunden (xcode-select -p: $DEV)." >&2
  echo "Bitte Xcode installieren und 'sudo xcode-select -s /Applications/Xcode.app' ausführen." >&2
  exit 1
fi

SWIFT_FLAGS=(-O -swift-version 5 -sdk "$SDK" -I "$CORE" -module-cache-path "$BUILD/modcache")
FRAMEWORKS=(-framework AppKit -framework Metal -framework MetalKit -framework QuartzCore)

compile_core() { # $1 = Architektur
  mkdir -p "$BUILD/$1"
  "$TC/clang" -c -O2 -std=c11 -Wall -Wextra -target "$1-apple-macos$MIN_MACOS" -isysroot "$SDK" \
    "$CORE/egypt_core.c" -o "$BUILD/$1/egypt_core.o"
}

if [[ "${1:-}" == "snapshot" ]]; then
  ARCH=$(uname -m)
  compile_core "$ARCH"
  "$TC/swiftc" "${SWIFT_FLAGS[@]}" -parse-as-library -target "$ARCH-apple-macos$MIN_MACOS" \
    Sources/EgyptRenderer.swift Tools/Snapshot.swift "$BUILD/$ARCH/egypt_core.o" \
    "${FRAMEWORKS[@]}" -o "$BUILD/snapshot"
  echo "Fertig: $BUILD/snapshot"
  exit 0
fi

for ARCH in arm64 x86_64; do
  compile_core "$ARCH"
  # Als Mach-O-Bundle linken (wie Xcode es für Bildschirmschoner macht), nicht als dylib.
  "$TC/swiftc" "${SWIFT_FLAGS[@]}" -emit-executable -Xlinker -bundle -module-name "$EXEC" \
    -target "$ARCH-apple-macos$MIN_MACOS" \
    Sources/EgyptRenderer.swift Sources/EgyptSaverView.swift "$BUILD/$ARCH/egypt_core.o" \
    "${FRAMEWORKS[@]}" -framework ScreenSaver -o "$BUILD/$ARCH/$EXEC"
done

BUNDLE="$BUILD/$NAME.saver"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources"
"$TC/lipo" -create "$BUILD/arm64/$EXEC" "$BUILD/x86_64/$EXEC" -output "$BUNDLE/Contents/MacOS/$EXEC"
cp Info.plist "$BUNDLE/Contents/Info.plist"
# Vorschaubilder für die Liste in den Systemeinstellungen
cp Resources/thumbnail.png Resources/thumbnail@2x.png "$BUNDLE/Contents/Resources/"
codesign --force --sign - "$BUNDLE"
echo "Fertig: $BUNDLE"

if [[ "${1:-}" == "install" ]]; then
  DEST="$HOME/Library/Screen Savers"
  mkdir -p "$DEST"
  rm -rf "$DEST/$NAME.saver" "$DEST/$OLD_NAME.saver"
  cp -R "$BUNDLE" "$DEST/"
  # Laufende Bildschirmschoner-Prozesse beenden, damit die neue Version geladen wird.
  # WallpaperAgent merkt sich das alte Bundle; ohne Neustart bleibt der echte Start schwarz.
  killall WallpaperAgent legacyScreenSaver 2>/dev/null || true
  echo "Installiert: $DEST/$NAME.saver"
  echo "Auswahl: Systemeinstellungen > Bildschirmschoner > Ägyptische Tänzer"
fi
