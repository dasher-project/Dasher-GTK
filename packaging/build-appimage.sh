#!/bin/bash
# AppImage packaging script — runs inside CI on ubuntu-22.04
# Produces Dasher-x86_64.AppImage
set -ex

BUILD_DIR="${BUILD_DIR:-build}"
APPDIR="${APPDIR:-AppDir}"

# ─── Build ─────────────────────────────────────────────────────────────
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(nproc)"

# ─── Create AppDir ────────────────────────────────────────────────────
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin"
mkdir -p "$APPDIR/usr/share/dasher"
mkdir -p "$APPDIR/usr/share/applications"
mkdir -p "$APPDIR/usr/share/icons/hicolor/48x48/apps"
mkdir -p "$APPDIR/usr/share/icons/hicolor/scalable/apps"

# Binary + shared libraries (flat layout, matching the build output)
cp "$BUILD_DIR/Dasher/dasher" "$APPDIR/usr/bin/"
cp "$BUILD_DIR/Dasher/libdasher.so" "$APPDIR/usr/bin/" 2>/dev/null || true
cp "$BUILD_DIR/Dasher/librust_tts_wrapper.so" "$APPDIR/usr/bin/" 2>/dev/null || true

# Runtime data into share/dasher/ (AppRun will cd here so ./Data resolves)
cp -r "$BUILD_DIR/Dasher/Data" "$APPDIR/usr/share/dasher/"
cp -r "$BUILD_DIR/Dasher/Strings" "$APPDIR/usr/share/dasher/"
cp "$BUILD_DIR/Dasher/UIStyle.css" "$APPDIR/usr/share/dasher/"
cp -r "$BUILD_DIR/Dasher/Resources" "$APPDIR/usr/share/dasher/"

# Desktop entry + icons
cp packaging/org.alternativeinterface.dasher.desktop \
   "$APPDIR/usr/share/applications/"
cp Resources/icon.png \
   "$APPDIR/usr/share/icons/hicolor/48x48/apps/org.alternativeinterface.dasher.png"
cp DasherCore/Data/dasher.svg \
   "$APPDIR/usr/share/icons/hicolor/scalable/apps/org.alternativeinterface.dasher.svg"

# Desktop file at AppDir root for linuxdeploy
cat > "$APPDIR/org.alternativeinterface.dasher.desktop" << 'DESKTOP'
[Desktop Entry]
Type=Application
Name=Dasher
GenericName=Predictive Text Entry
Comment=Enter text without a keyboard using zooming predictive input
Exec=dasher
Icon=org.alternativeinterface.dasher
Terminal=false
StartupNotify=true
Categories=Utility;Accessibility;
Keywords=text;accessibility;AAC;predictive;input;dwell;switch;
DESKTOP

# ─── Custom AppRun (cd to data dir so relative paths resolve) ─────────
cat > "$APPDIR/AppRun" << 'APPRUN'
#!/bin/bash
HERE="$(dirname "$(readlink -f "${0}")")"
cd "$HERE/usr/share/dasher"
exec "$HERE/usr/bin/dasher" "$@"
APPRUN
chmod +x "$APPDIR/AppRun"

# ─── Bundle shared library deps with linuxdeploy ──────────────────────
wget -q "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
chmod +x linuxdeploy-x86_64.AppImage

wget -q "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh"
chmod +x linuxdeploy-plugin-gtk.sh

export DEPLOY_GTK_VERSION=4
export ARCH=x86_64
export VERSION="${VERSION:-0.1.0}"

# linuxdeploy bundles deps into the AppDir, but we use our custom AppRun
./linuxdeploy-x86_64.AppImage \
  --appdir "$APPDIR" \
  --desktop-file "$APPDIR/org.alternativeinterface.dasher.desktop" \
  --icon-file "$APPDIR/usr/share/icons/hicolor/48x48/apps/org.alternativeinterface.dasher.png" \
  --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/org.alternativeinterface.dasher.svg" \
  --plugin gtk

# Re-write our custom AppRun (linuxdeploy may have overwritten it)
cat > "$APPDIR/AppRun" << 'APPRUN'
#!/bin/bash
HERE="$(dirname "$(readlink -f "${0}")")"
cd "$HERE/usr/share/dasher"
exec "$HERE/usr/bin/dasher" "$@"
APPRUN
chmod +x "$APPDIR/AppRun"

# ─── Create AppImage ──────────────────────────────────────────────────
# appimagetool needs libfuse2; extract-and-run avoids FUSE requirement
wget -q "https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage"
chmod +x appimagetool-x86_64.AppImage

# Extract appimagetool so it doesn't need FUSE at runtime
./appimagetool-x86_64.AppImage --appimage-extract
./squashfs-root/AppRun "$APPDIR" "Dasher-x86_64.AppImage"

echo "AppImage created: Dasher-x86_64.AppImage"
ls -lh Dasher-x86_64.AppImage
