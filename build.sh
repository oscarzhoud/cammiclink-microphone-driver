#!/bin/bash
# Build, sign, package and test the CamMicLink Microphone driver.
#
# Environment:
#   CML_VERSION       driver version              (default: 0.3.0)
#   CML_BUILD         CFBundleVersion             (default: 300)
#   CML_SIGN_ID       "Developer ID Application: ..." used for distribution.
#                     Unset means an ad-hoc signature, which is enough for
#                     local use on Apple Silicon and on Intel.
#   CML_INSTALLER_ID  "Developer ID Installer: ..." used to sign the .pkg files.
#                     Unset means the packages stay unsigned.
set -euo pipefail
cd "$(dirname "$0")"

VERSION="${CML_VERSION:-0.3.0}"
BUILD_NUMBER="${CML_BUILD:-300}"
BUNDLE="CamMicLinkMic"
BUILD="$PWD/build"
ARTIFACTS="$PWD/artifacts"
DEST="$BUILD/$BUNDLE.driver"

rm -rf "$DEST"
mkdir -p "$DEST/Contents/MacOS" "$ARTIFACTS" \
  "$BUILD/pkg-root/Library/Audio/Plug-Ins/HAL" \
  "$BUILD/install-scripts" "$BUILD/uninstall-scripts"

echo "Building $BUNDLE $VERSION ($BUILD_NUMBER)"
clang -std=c11 -arch arm64 -arch x86_64 -mmacosx-version-min=12.0 \
  -Wall -Wextra -Werror -O2 -bundle \
  src/CamMicLinkDriver.c src/CMLAudioCore.c \
  -framework CoreAudio -framework CoreFoundation \
  -o "$DEST/Contents/MacOS/$BUNDLE"

cat > "$DEST/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleDevelopmentRegion</key><string>en</string>
<key>CFBundleExecutable</key><string>$BUNDLE</string>
<key>CFBundleIdentifier</key><string>com.oscarzhou.cammiclink.audiodriver</string>
<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
<key>CFBundleName</key><string>$BUNDLE</string>
<key>CFBundlePackageType</key><string>BNDL</string>
<key>CFBundleShortVersionString</key><string>$VERSION</string>
<key>CFBundleVersion</key><string>$BUILD_NUMBER</string>
<key>CFPlugInFactories</key><dict><key>65D2A6AE-3499-4B09-BB23-5E967D35CC30</key><string>CMLDriver_Create</string></dict>
<key>CFPlugInTypes</key><dict><key>443ABAB8-E7B3-491A-B985-BEB9187030DB</key><array><string>65D2A6AE-3499-4B09-BB23-5E967D35CC30</string></array></dict>
</dict></plist>
PLIST

# Developer ID signing is opt-in: never hard-code somebody else's identity and
# never require a certificate just to build. Ad-hoc code is loadable locally.
if [ -n "${CML_SIGN_ID:-}" ] &&
   security find-identity -v -p codesigning 2>/dev/null | grep -qF "$CML_SIGN_ID"; then
  echo "Signing driver with: $CML_SIGN_ID"
  codesign --force --options runtime --timestamp --sign "$CML_SIGN_ID" "$DEST"
else
  if [ -n "${CML_SIGN_ID:-}" ]; then
    echo "warning: CML_SIGN_ID not found in the keychain; using an ad-hoc signature." >&2
  else
    echo "No CML_SIGN_ID set: using an ad-hoc signature (fine for local use)."
  fi
  codesign --force --sign - "$DEST"
fi
codesign --verify --strict "$DEST"

# Replace only the staging output, never a system-installed driver.
rm -rf "$BUILD/pkg-root/Library/Audio/Plug-Ins/HAL/$BUNDLE.driver"
ditto "$DEST" "$BUILD/pkg-root/Library/Audio/Plug-Ins/HAL/$BUNDLE.driver"
chmod -R go-w "$BUILD/pkg-root"

cat > "$BUILD/install-scripts/postinstall" <<'POST'
#!/bin/sh
/bin/launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || /usr/bin/killall coreaudiod 2>/dev/null || true
exit 0
POST
cat > "$BUILD/uninstall-scripts/postinstall" <<'POST'
#!/bin/sh
# Intentionally removes only CamMicLink's driver, never other audio components.
/bin/rm -rf /Library/Audio/Plug-Ins/HAL/CamMicLinkMic.driver
/bin/launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || /usr/bin/killall coreaudiod 2>/dev/null || true
exit 0
POST
chmod 755 "$BUILD/install-scripts/postinstall" "$BUILD/uninstall-scripts/postinstall"

pkgbuild --analyze --root "$BUILD/pkg-root" "$BUILD/components.plist"
/usr/libexec/PlistBuddy -c 'Set :0:BundleIsRelocatable false' "$BUILD/components.plist" 2>/dev/null ||
  /usr/libexec/PlistBuddy -c 'Add :0:BundleIsRelocatable bool false' "$BUILD/components.plist"
pkgbuild --root "$BUILD/pkg-root" --component-plist "$BUILD/components.plist" --ownership recommended \
  --identifier com.oscarzhou.cammiclink.audiocomponent --version "$VERSION" --install-location / \
  --scripts "$BUILD/install-scripts" "$ARTIFACTS/CamMicLink-Microphone-$VERSION.pkg"
pkgbuild --nopayload --scripts "$BUILD/uninstall-scripts" --identifier com.oscarzhou.cammiclink.audiocomponent.remove \
  --version "$VERSION" "$ARTIFACTS/Uninstall-CamMicLink-Microphone.pkg"

# Installer signing is separate from pkgbuild and also opt-in.
if [ -n "${CML_INSTALLER_ID:-}" ] &&
   security find-identity -v 2>/dev/null | grep -qF "$CML_INSTALLER_ID"; then
  for PKG in "$ARTIFACTS/CamMicLink-Microphone-$VERSION.pkg" "$ARTIFACTS/Uninstall-CamMicLink-Microphone.pkg"; do
    [ -f "$PKG" ] || continue
    productsign --sign "$CML_INSTALLER_ID" "$PKG" "$PKG.signed" >/dev/null
    mv "$PKG.signed" "$PKG"
    echo "Signed package: $(basename "$PKG")"
  done
elif [ -n "${CML_INSTALLER_ID:-}" ]; then
  echo "warning: CML_INSTALLER_ID not found in the keychain; packages stay unsigned." >&2
else
  echo "No CML_INSTALLER_ID set: packages are unsigned (Gatekeeper will warn on other Macs)."
fi

clang -std=c11 -O2 -Wall -Wextra -Werror tools/hal_probe.c \
  -framework CoreAudio -framework CoreFoundation -framework AudioToolbox -o "$BUILD/hal_probe"
clang -DCML_TEST -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror \
  tests/test_driver.c src/CMLAudioCore.c -framework CoreAudio -framework CoreFoundation \
  -o "$BUILD/test_driver"
"$BUILD/test_driver" | tee "$ARTIFACTS/offline-test-results.txt"

echo "Built $VERSION in $ARTIFACTS"
