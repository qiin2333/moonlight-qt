# This script requires create-dmg to be installed from https://github.com/sindresorhus/create-dmg
BUILD_CONFIG=$1

fail()
{
	echo "$1" 1>&2
	exit 1
}

if [ "$BUILD_CONFIG" != "Debug" ] && [ "$BUILD_CONFIG" != "Release" ]; then
  fail "Invalid build configuration - expected 'Debug' or 'Release'"
fi

BUILD_ROOT=$PWD/build
SOURCE_ROOT=$PWD
BUILD_FOLDER=$BUILD_ROOT/build-$BUILD_CONFIG
INSTALLER_FOLDER=$BUILD_ROOT/installer-$BUILD_CONFIG

VERSION=$(python3 "$SOURCE_ROOT/scripts/derive-version.py" --source-root "$SOURCE_ROOT" --field artifact)

if [ "$SIGNING_PROVIDER_SHORTNAME" == "" ]; then
  SIGNING_PROVIDER_SHORTNAME=$SIGNING_IDENTITY
fi
if [ "$SIGNING_IDENTITY" == "" ]; then
  SIGNING_IDENTITY=$SIGNING_PROVIDER_SHORTNAME
fi

[ "$SIGNING_IDENTITY" == "" ] || git diff-index --quiet HEAD -- || fail "Signed release builds must not have unstaged changes!"

echo Updating dependencies
python3 $SOURCE_ROOT/setup-deps.py

echo Cleaning output directories
rm -rf $BUILD_FOLDER
rm -rf $INSTALLER_FOLDER
mkdir $BUILD_ROOT
mkdir $BUILD_FOLDER
mkdir $INSTALLER_FOLDER

# Determine target architecture.
# If MOONLIGHT_ARCH is set (e.g. by CI), build only that arch.
# Otherwise detect the native arch to avoid universal binaries,
# which cause multiple macOS TCC permission dialogs due to each
# architecture slice having a different CDHash.
if [ -z "$MOONLIGHT_ARCH" ]; then
  MOONLIGHT_ARCH=$(uname -m)
  # Normalise: Apple Silicon reports arm64, Rosetta reports x86_64
  if [ "$MOONLIGHT_ARCH" = "arm64" ]; then
    MOONLIGHT_ARCH="arm64"
  else
    MOONLIGHT_ARCH="x86_64"
  fi
fi

# Enable LTO for official builds
export CFLAGS=-flto=thin
export CXXFLAGS=-flto=thin
export LDFLAGS=-flto=thin

echo "Configuring the project for architecture: $MOONLIGHT_ARCH"
pushd $BUILD_FOLDER
qmake $SOURCE_ROOT/moonlight-qt.pro QMAKE_APPLE_DEVICE_ARCHS="$MOONLIGHT_ARCH" || fail "Qmake failed!"
popd

echo Compiling Moonlight in $BUILD_CONFIG configuration
pushd $BUILD_FOLDER
make -j$(sysctl -n hw.logicalcpu) $(echo "$BUILD_CONFIG" | tr '[:upper:]' '[:lower:]') || fail "Make failed!"
popd

# USB 转发 helper（usbipdcpp + libusb 静态构建，见 usb-helper/README.md）。
# 独立 build 目录按架构隔离（本脚本可能对不同架构各跑一次，BUILD_ROOT 是共
# 享的），configure 前清掉防陈旧缓存；主工程的 LTO 环境变量不带给它。
echo Building USB forwarding helper
USB_HELPER_BUILD=$BUILD_ROOT/usb-helper-$MOONLIGHT_ARCH
rm -rf "$USB_HELPER_BUILD"
(
  unset CFLAGS CXXFLAGS LDFLAGS
  cmake -S "$SOURCE_ROOT/usb-helper" -B "$USB_HELPER_BUILD" \
    -DCMAKE_BUILD_TYPE=$BUILD_CONFIG \
    -DCMAKE_OSX_ARCHITECTURES=$MOONLIGHT_ARCH || exit 1
  cmake --build "$USB_HELPER_BUILD" -j$(sysctl -n hw.logicalcpu) || exit 1
) || fail "USB helper build failed!"

echo Saving dSYM file
pushd $BUILD_FOLDER
dsymutil app/Moonlight.app/Contents/MacOS/Moonlight -o Moonlight-$VERSION.dsym || fail "dSYM creation failed!"
cp -R Moonlight-$VERSION.dsym $INSTALLER_FOLDER || fail "dSYM copy failed!"
popd

echo Creating app bundle
EXTRA_ARGS=
if [ "$BUILD_CONFIG" == "Debug" ]; then EXTRA_ARGS="$EXTRA_ARGS -use-debug-libs"; fi
echo Extra deployment arguments: $EXTRA_ARGS

# Qt 6.10+ can ship the Mimer SQL driver with an absolute dependency on
# /usr/local/lib/libmimerapi.dylib, which is not present on GitHub's macOS
# runners. Moonlight doesn't use Qt SQL plugins, so remove the problematic
# plugin in CI before macdeployqt scans Qt's plugin tree.
if [ "$GITHUB_ACTIONS" == "true" ]; then
  QT_PLUGIN_DIR=`qmake -query QT_INSTALL_PLUGINS 2>/dev/null`
  if [ -n "$QT_PLUGIN_DIR" ]; then
    rm -f "$QT_PLUGIN_DIR/sqldrivers/libqsqlmimer.dylib"
  fi
fi

echo Copying clipboard helper into app bundle
HELPER_BINARY=$BUILD_FOLDER/clipboard-helper/moonlight-clipboard-helper
if [ ! -f "$HELPER_BINARY" ]; then
  HELPER_BINARY=$BUILD_FOLDER/clipboard-helper/$BUILD_CONFIG/moonlight-clipboard-helper
fi
if [ ! -f "$HELPER_BINARY" ]; then
  HELPER_BINARY=$BUILD_FOLDER/clipboard-helper/$(echo "$BUILD_CONFIG" | tr '[:upper:]' '[:lower:]')/moonlight-clipboard-helper
fi
cp "$HELPER_BINARY" $BUILD_FOLDER/app/Moonlight.app/Contents/MacOS/ || fail "Clipboard helper copy failed!"

echo Copying USB forwarding helper into app bundle
cp "$USB_HELPER_BUILD/moonlight-usbd" $BUILD_FOLDER/app/Moonlight.app/Contents/MacOS/ || fail "USB helper copy failed!"

# macdeployqt only rewrites Qt references in the main executable and the
# plugins it deploys, so the clipboard helper has to be named explicitly with
# -executable. Otherwise it keeps the build machine's absolute Qt paths, which
# either don't exist on the user's machine or pull a second copy of Qt into the
# process alongside the bundled one. Either way the helper aborts at startup
# and clipboard sync silently disables itself.
macdeployqt $BUILD_FOLDER/app/Moonlight.app $EXTRA_ARGS -executable=$BUILD_FOLDER/app/Moonlight.app/Contents/MacOS/moonlight-clipboard-helper -executable=$BUILD_FOLDER/app/Moonlight.app/Contents/MacOS/moonlight-usbd -qmldir=$SOURCE_ROOT/app/gui -appstore-compliant || fail "macdeployqt failed!"

echo Building File Provider extension into app bundle
bash "$SOURCE_ROOT/scripts/build-macos-fileprovider-extension.sh" "$SOURCE_ROOT" "$BUILD_FOLDER" "$BUILD_FOLDER/app/Moonlight.app" "$MOONLIGHT_ARCH" || fail "File Provider extension build failed"

echo Removing dSYM files from app bundle
find $BUILD_FOLDER/app/Moonlight.app/ -name '*.dSYM' | xargs rm -rf

# The code-signing identifier must match the Info.plist bundle ID. TCC-style
# gates such as local network privacy match the running binary by its signing
# identifier, while LaunchServices and the permission UI use the bundle ID.
# Linker signing names the identifier after the executable ("Moonlight"), so
# the mismatch makes Finder launches silently fail LAN access on recent macOS
# (qiin2333/moonlight-qt#285). Sign the whole bundle ourselves with
# --identifier taken from the Info.plist instead, even without a real identity.
APP_BUNDLE=$BUILD_FOLDER/app/Moonlight.app
BUNDLE_ID=$(plutil -extract CFBundleIdentifier raw "$APP_BUNDLE/Contents/Info.plist") || fail "Cannot read CFBundleIdentifier from app bundle"

if [ "$SIGNING_IDENTITY" != "" ]; then
  echo Signing app bundle
  codesign --force --deep --options runtime --timestamp --identifier "$BUNDLE_ID" --sign "$SIGNING_IDENTITY" $APP_BUNDLE || fail "Signing failed!"
else
  echo Signing app bundle ad-hoc
  # Seals resources and gives a consistent identity. Not a substitute for
  # Developer ID signing and notarization.
  #
  # Nested code is signed before the bundle so the outer signature needs no
  # --deep, which would overwrite nested signatures with the app's
  # parameters (the File Provider extension must keep its own identifier and
  # entitlements). The extension is signed by its build step; deployed
  # dylibs and frameworks may have lost their build-time linker signatures
  # to install_name_tool patching, so anything that fails verification is
  # ad-hoc signed here as a fallback.
  while IFS= read -r item; do
    if ! codesign --verify "$item" 2>/dev/null; then
      codesign --force --sign - "$item" || fail "Ad-hoc signing of $item failed!"
    fi
  done < <(find "$APP_BUNDLE/Contents/Frameworks" "$APP_BUNDLE/Contents/PlugIns" \( -name '*.dylib' -o -name '*.framework' \) -print 2>/dev/null)
  # The deployed helpers are patched by macdeployqt after linking too and
  # depend on the same fallback. The main executable is re-signed by the
  # outer signature below and needs no separate treatment.
  for helper in moonlight-clipboard-helper moonlight-usbd; do
    item=$APP_BUNDLE/Contents/MacOS/$helper
    [ -f "$item" ] || continue
    if ! codesign --verify "$item" 2>/dev/null; then
      codesign --force --sign - "$item" || fail "Ad-hoc signing of $item failed!"
    fi
  done
  codesign --force --identifier "$BUNDLE_ID" --sign - $APP_BUNDLE || fail "Ad-hoc signing failed!"
fi

echo Verifying app bundle signature
codesign --verify --deep --strict $APP_BUNDLE || fail "App bundle signature verification failed!"
SIGNED_IDENTIFIER=$(codesign -dv $APP_BUNDLE 2>&1 | sed -n 's/^Identifier=\(.*\)$/\1/p')
if [ "$SIGNED_IDENTIFIER" != "$BUNDLE_ID" ]; then
  fail "Signing identifier '$SIGNED_IDENTIFIER' does not match CFBundleIdentifier '$BUNDLE_ID'"
fi

# The extension must keep its own signing identity through either outer
# signing mode; a bundle-level identifier check cannot see it, so check the
# extension itself.
APPEX=$APP_BUNDLE/Contents/PlugIns/MoonlightFileProviderExtension.appex
if [ -d "$APPEX" ]; then
  APPEX_ID=$(plutil -extract CFBundleIdentifier raw "$APPEX/Contents/Info.plist") || fail "Cannot read File Provider extension bundle ID"
  APPEX_SIGNED_ID=$(codesign -dv "$APPEX" 2>&1 | sed -n 's/^Identifier=\(.*\)$/\1/p')
  if [ "$APPEX_SIGNED_ID" != "$APPEX_ID" ]; then
    fail "File Provider extension signing identifier '$APPEX_SIGNED_ID' does not match its CFBundleIdentifier '$APPEX_ID'"
  fi
  if [ -n "${MOONLIGHT_FILE_PROVIDER_APP_GROUP:-}" ] && ! codesign -d --entitlements - "$APPEX" 2>/dev/null | grep -aq "application-groups"; then
    echo "WARNING: File Provider extension signature carries no App Group entitlements" >&2
  fi
fi

echo Creating DMG
if [ "$SIGNING_IDENTITY" != "" ]; then
  create-dmg $BUILD_FOLDER/app/Moonlight.app $INSTALLER_FOLDER --identity="$SIGNING_IDENTITY" --no-version-in-filename || fail "create-dmg failed!"
else
  if ! create-dmg "$BUILD_FOLDER/app/Moonlight.app" "$INSTALLER_FOLDER" --no-version-in-filename --no-code-sign; then
    echo "create-dmg failed; falling back to hdiutil"
    rm -f "$INSTALLER_FOLDER/Moonlight.dmg"
    hdiutil create -volname "Moonlight V+" -srcfolder "$BUILD_FOLDER/app/Moonlight.app" -ov -format UDZO "$INSTALLER_FOLDER/Moonlight.dmg" || fail "fallback hdiutil DMG creation failed!"
  fi
fi

# create-dmg names the image after CFBundleDisplayName, while the hdiutil
# fallback uses Moonlight.dmg. The installer directory was cleaned above, so
# require exactly one generated image and use its real path from here on.
DMG_CANDIDATES=("$INSTALLER_FOLDER"/*.dmg)
if [ "${#DMG_CANDIDATES[@]}" -ne 1 ] || [ ! -f "${DMG_CANDIDATES[0]}" ]; then
  fail "Expected exactly one generated DMG in $INSTALLER_FOLDER"
fi
GENERATED_DMG="${DMG_CANDIDATES[0]}"

if [ "$NOTARY_KEYCHAIN_PROFILE" != "" ]; then
  echo Uploading to App Notary service
  xcrun notarytool submit --keychain-profile "$NOTARY_KEYCHAIN_PROFILE" --wait "$GENERATED_DMG" || fail "Notary submission failed"

  echo Stapling notary ticket to DMG
  xcrun stapler staple -v "$GENERATED_DMG" || fail "Notary ticket stapling failed!"
fi

# 名字里带上架构。这里只出一个架构的包（见上面 MOONLIGHT_ARCH 的注释），叫
# Moonlight-<版本>.dmg 的话下载的人无法从名字判断能不能装 —— Intel Mac 上装了
# 才发现打不开。app 侧的更新器也靠这个后缀挑对应架构的资产。
mv "$GENERATED_DMG" "$INSTALLER_FOLDER/Moonlight-VPlus-$VERSION-$MOONLIGHT_ARCH.dmg"
echo Build successful
