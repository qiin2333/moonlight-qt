#!/bin/bash
set -euo pipefail

fail()
{
  echo "$1" 1>&2
  exit 1
}

if [ "$#" -lt 3 ]; then
  fail "Usage: $0 <source-root> <build-folder> <app-bundle> [arch]"
fi

SOURCE_ROOT=$1
BUILD_FOLDER=$2
APP_BUNDLE=$3
MOONLIGHT_ARCH=${4:-$(uname -m)}

EXTENSION_NAME=MoonlightFileProviderExtension
MODULE_NAME=MoonlightFileProviderExtension
APP_BUNDLE_ID=com.moonlight-stream.Moonlight
EXTENSION_BUNDLE_ID=$APP_BUNDLE_ID.FileProvider
FILE_PROVIDER_APP_GROUP=${MOONLIGHT_FILE_PROVIDER_APP_GROUP:-group.com.moonlight-stream.Moonlight.FileProvider}
EXTENSION_SOURCE=$SOURCE_ROOT/file-mapping/mount/macos-fileprovider/MoonlightFileProviderExtension
EXTENSION_BUILD=$BUILD_FOLDER/fileprovider-extension
APPEX=$APP_BUNDLE/Contents/PlugIns/$EXTENSION_NAME.appex
EXECUTABLE=$APPEX/Contents/MacOS/$EXTENSION_NAME
INFO_PLIST=$APPEX/Contents/Info.plist

if [ "$MOONLIGHT_ARCH" != "arm64" ] && [ "$MOONLIGHT_ARCH" != "x86_64" ]; then
  MOONLIGHT_ARCH=x86_64
fi

command -v swiftc >/dev/null 2>&1 || fail "swiftc is required to build the File Provider extension"
command -v xcrun >/dev/null 2>&1 || fail "xcrun is required to build the File Provider extension"

VERSION=$(python3 "$SOURCE_ROOT/scripts/derive-version.py" --source-root "$SOURCE_ROOT" --field numeric)
if [ -z "$VERSION" ]; then
  VERSION=0.0.0
fi

SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-11.0}
SWIFT_TARGET=$MOONLIGHT_ARCH-apple-macos$DEPLOYMENT_TARGET

echo "Building File Provider extension for $SWIFT_TARGET"
rm -rf "$EXTENSION_BUILD"
rm -rf "$APPEX"
mkdir -p "$EXTENSION_BUILD"
mkdir -p "$APPEX/Contents/MacOS"
mkdir -p "$APPEX/Contents/Resources"

swiftc \
  -sdk "$SDKROOT" \
  -target "$SWIFT_TARGET" \
  -application-extension \
  -emit-library \
  -parse-as-library \
  -module-name "$MODULE_NAME" \
  -framework FileProvider \
  -framework Foundation \
  -framework UniformTypeIdentifiers \
  -o "$EXECUTABLE" \
  "$EXTENSION_SOURCE/FileProviderExtension.swift" \
  "$EXTENSION_SOURCE/FileProviderEnumerator.swift" \
  || fail "File Provider extension Swift build failed"

sed \
  -e "s|\$(EXECUTABLE_NAME)|$EXTENSION_NAME|g" \
  -e "s|\$(PRODUCT_BUNDLE_IDENTIFIER)|$EXTENSION_BUNDLE_ID|g" \
  -e "s|\$(MARKETING_VERSION)|$VERSION|g" \
  -e "s|\$(CURRENT_PROJECT_VERSION)|$VERSION|g" \
  -e "s|\$(PRODUCT_MODULE_NAME)|$MODULE_NAME|g" \
  -e "s|\$(MOONLIGHT_FILE_PROVIDER_APP_GROUP)|$FILE_PROVIDER_APP_GROUP|g" \
  "$EXTENSION_SOURCE/Info.plist" > "$INFO_PLIST"

if [ -n "${SIGNING_IDENTITY:-}" ]; then
  ENTITLEMENTS=$EXTENSION_BUILD/$EXTENSION_NAME.entitlements
  if [ -n "${MOONLIGHT_FILE_PROVIDER_APP_GROUP:-}" ]; then
    sed \
      -e "s|\$(MOONLIGHT_FILE_PROVIDER_APP_GROUP)|$FILE_PROVIDER_APP_GROUP|g" \
      "$EXTENSION_SOURCE/$EXTENSION_NAME.entitlements" > "$ENTITLEMENTS"
    codesign --force --options runtime --timestamp --entitlements "$ENTITLEMENTS" --sign "$SIGNING_IDENTITY" "$APPEX" || fail "File Provider extension signing failed"
  else
    echo "MOONLIGHT_FILE_PROVIDER_APP_GROUP is not set; signing File Provider extension without App Group entitlements"
    codesign --force --options runtime --timestamp --sign "$SIGNING_IDENTITY" "$APPEX" || fail "File Provider extension signing failed"
  fi
fi

echo "File Provider extension bundled at $APPEX"
