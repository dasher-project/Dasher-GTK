#!/bin/bash
# AppImage packaging script — runs inside CI on ubuntu-22.04 (x86_64 native)
# or inside an arm64v8/ubuntu container under QEMU (aarch64).
# Produces Dasher-${ARCH}.AppImage
set -ex

BUILD_DIR="${BUILD_DIR:-build}"
APPDIR="${APPDIR:-AppDir}"

# Host arch defaults to uname -m; override via the env (set by the publish
# workflow for cross-arch legs). linuxdeploy / appimagetool ship per-arch
# AppImages, and the output filename carries the arch too.
ARCH="${ARCH:-$(uname -m)}"

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

# Runtime data into share/dasher/ (AppRun will cd here so ./Data resolves).
# UIStyle.css is compiled into the binary as a GResource, so it is not copied.
cp -r "$BUILD_DIR/Dasher/Data" "$APPDIR/usr/share/dasher/"
cp -r "$BUILD_DIR/Dasher/Strings" "$APPDIR/usr/share/dasher/"
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
# AppImages ship as a static-PIE ELF wrapper around an ISO9660+squashfs
# payload. QEMU's binfmt can't exec the static-PIE wrapper on the aarch64
# leg ("Exec format error"), so we bypass the wrapper entirely: locate the
# embedded squashfs by its 'hsqs' magic and unsquashfs straight into
# squashfs-root, then run the inner AppRun (a regular static ELF that
# QEMU handles fine).
extract_appimage() {
  local src="$1" dest="$2"
  local offset
  # The 4 bytes 'hsqs' (squashfs magic) can occur by chance inside the ELF
  # payload, so we walk every match and pick the first that's followed by a
  # sane superblock (block_size one of the documented powers of two from
  # 4 KiB .. 1 MiB).
  offset="$(python3 -c "
import struct, sys
with open('${src}', 'rb') as f:
    data = f.read()
valid_bs = {4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576}
idx = 0
while True:
    idx = data.find(b'hsqs', idx)
    if idx < 0:
        sys.exit('no valid squashfs superblock in ${src}')
    bs = struct.unpack('<I', data[idx+12:idx+16])[0]
    if bs in valid_bs:
        print(idx)
        break
    idx += 4
")"
  rm -rf "$dest"
  unsquashfs -f -d "$dest" -o "$offset" "$src" >/dev/null
}

wget -q "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${ARCH}.AppImage"
chmod +x "linuxdeploy-${ARCH}.AppImage"

wget -q "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh"
chmod +x linuxdeploy-plugin-gtk.sh

export DEPLOY_GTK_VERSION=4
export ARCH="$ARCH"
export VERSION="${VERSION:-0.1.0}"

extract_appimage "linuxdeploy-${ARCH}.AppImage" squashfs-root
LINUXDEPLOY=./squashfs-root/AppRun

# linuxdeploy bundles deps into the AppDir, but we use our custom AppRun
"$LINUXDEPLOY" \
  --appdir "$APPDIR" \
  --desktop-file "$APPDIR/org.alternativeinterface.dasher.desktop" \
  --icon-file "$APPDIR/usr/share/icons/hicolor/48x48/apps/org.alternativeinterface.dasher.png" \
  --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/org.alternativeinterface.dasher.svg" \
  --plugin gtk

# Clear the squashfs-root so appimagetool's own extraction below
# doesn't collide with it.
rm -rf squashfs-root

# Re-write our custom AppRun (linuxdeploy may have overwritten it)
cat > "$APPDIR/AppRun" << 'APPRUN'
#!/bin/bash
HERE="$(dirname "$(readlink -f "${0}")")"
cd "$HERE/usr/share/dasher"
exec "$HERE/usr/bin/dasher" "$@"
APPRUN
chmod +x "$APPDIR/AppRun"

# ─── Create AppImage ──────────────────────────────────────────────────
# Same extract-avoiding-the-static-PIE-wrapper trick as linuxdeploy above.
wget -q "https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-${ARCH}.AppImage"
chmod +x "appimagetool-${ARCH}.AppImage"

extract_appimage "appimagetool-${ARCH}.AppImage" squashfs-root
./squashfs-root/AppRun "$APPDIR" "Dasher-${ARCH}.AppImage"

echo "AppImage created: Dasher-${ARCH}.AppImage"
ls -lh "Dasher-${ARCH}.AppImage"
