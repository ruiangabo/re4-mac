#!/bin/sh
# Packs the macOS port into "Resident Evil 4.app" (no game data inside: the player points it at their own
# disc-data folder the first time). Usage: package_app.sh <build dir> <out dir>
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=$(cd "$1" && pwd)
OUTDIR=${2:-.}
APP="$OUTDIR/Resident Evil 4.app"
C="$APP/Contents"
rm -rf "$APP"
mkdir -p "$C/MacOS/modules" "$C/Frameworks" "$C/Resources"

cp "$BUILD/re4_mac" "$BUILD/re4_coreproc" "$BUILD/libre4_main.dylib" "$BUILD/libre4_platform.dylib" "$C/MacOS/"
cp "$BUILD"/modules/*.dylib "$C/MacOS/modules/"
cp "$HERE/launcher.sh" "$C/MacOS/RE4"; chmod +x "$C/MacOS/RE4"
# the PlayStation button icons (texture pack; names from the US disc G4BE08, make_ps_buttons.swift)
mkdir -p "$C/Resources/textures"; cp -R "$HERE/textures/ps-buttons" "$C/Resources/textures/"

# SDL3 inside the bundle
SDL=$(otool -L "$BUILD/re4_mac" | awk '/libSDL3/ {print $1}')
cp "$SDL" "$C/Frameworks/libSDL3.0.dylib"; chmod u+w "$C/Frameworks/libSDL3.0.dylib"
install_name_tool -id @rpath/libSDL3.0.dylib "$C/Frameworks/libSDL3.0.dylib"
install_name_tool -change "$SDL" @rpath/libSDL3.0.dylib "$C/MacOS/re4_mac"
install_name_tool -delete_rpath /opt/homebrew/lib "$C/MacOS/re4_mac" 2>/dev/null || true
install_name_tool -add_rpath @executable_path/../Frameworks "$C/MacOS/re4_mac"
# the game's libraries find each other next to themselves, not in the build folder
for f in "$C/MacOS/libre4_main.dylib" "$C/MacOS/libre4_platform.dylib" "$C"/MacOS/modules/*.dylib; do
  install_name_tool -delete_rpath "$BUILD" "$f" 2>/dev/null || true
done
for f in "$C/MacOS/libre4_main.dylib" "$C/MacOS/libre4_platform.dylib"; do install_name_tool -add_rpath @loader_path "$f"; done
for f in "$C"/MacOS/modules/*.dylib; do install_name_tool -add_rpath @loader_path -add_rpath @loader_path/.. "$f"; done

# icon
ICON=$(mktemp -d)/AppIcon.iconset; mkdir -p "$ICON"
swift "$HERE/make_icon.swift" "$ICON/../icon.png" >/dev/null
for s in 16 32 128 256 512; do
  sips -z $s $s "$ICON/../icon.png" --out "$ICON/icon_${s}x${s}.png" >/dev/null
  sips -z $((s*2)) $((s*2)) "$ICON/../icon.png" --out "$ICON/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$ICON" -o "$C/Resources/AppIcon.icns"

cat > "$C/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Resident Evil 4</string>
  <key>CFBundleDisplayName</key><string>Resident Evil 4</string>
  <key>CFBundleIdentifier</key><string>io.github.re4mac.port</string>
  <key>CFBundleExecutable</key><string>RE4</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.action-games</string>
  <key>LSArchitecturePriority</key><array><string>arm64</string></array>
  <key>NSHighResolutionCapable</key><true/>
  <key>GCSupportsControllerUserInteraction</key><true/>
  <key>GCSupportedGameControllers</key><array><dict><key>ProfileName</key><string>ExtendedGamepad</string></dict></array>
</dict>
</plist>
EOF

# signed in a temporary folder: folders under Documents can add attributes codesign rejects
T=$(mktemp -d)
ditto --norsrc --noextattr "$APP" "$T/R.app"
codesign --force --deep --sign - "$T/R.app"
rm -rf "$APP"; ditto --norsrc --noextattr "$T/R.app" "$APP"; rm -rf "$T"
codesign --verify --deep "$APP"
echo packed "$APP" "($(du -sh "$APP" | cut -f1))"
