BUILD_CONFIG="release"

fail()
{
	echo "$1" 1>&2
	exit 1
}

BUILD_ROOT=$PWD/build
SOURCE_ROOT=$PWD
BUILD_FOLDER=$BUILD_ROOT/build-$BUILD_CONFIG
DEPLOY_FOLDER=$BUILD_ROOT/deploy-$BUILD_CONFIG
INSTALLER_FOLDER=$BUILD_ROOT/installer-$BUILD_CONFIG

VERSION=$(python3 "$SOURCE_ROOT/scripts/derive-version.py" --source-root "$SOURCE_ROOT" --field artifact)
APPIMAGE_ARCH=$(uname -m)
LINUXDEPLOY=linuxdeploy-$APPIMAGE_ARCH.AppImage
APPIMAGE_UPDATE_TAG="${APPIMAGE_UPDATE_TAG:-latest}"

case "$APPIMAGE_UPDATE_TAG" in
	latest|latest-pre) ;;
	*) fail "Invalid APPIMAGE_UPDATE_TAG: $APPIMAGE_UPDATE_TAG"
esac

command -v qmake6 >/dev/null 2>&1 || fail "Unable to find 'qmake6' in your PATH!"
command -v $LINUXDEPLOY >/dev/null 2>&1 || fail "Unable to find '$LINUXDEPLOY' in your PATH!"
command -v readelf >/dev/null 2>&1 || fail "Unable to find 'readelf' in your PATH (install binutils)!"

echo "MOONLIGHT BUILD ENVIRONMENT"
qmake --version
echo "QT_ROOT_DIR=$QT_ROOT_DIR"
echo "PATH=$PATH"
echo "LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
echo "PKG_CONFIG_PATH=$PKG_CONFIG_PATH"

echo Cleaning output directories
rm -rf $BUILD_FOLDER
rm -rf $DEPLOY_FOLDER
rm -rf $INSTALLER_FOLDER
mkdir $BUILD_ROOT
mkdir $BUILD_FOLDER
mkdir $DEPLOY_FOLDER
mkdir $INSTALLER_FOLDER

# Enable LTO for official builds
export CFLAGS=-flto=auto
export CXXFLAGS=-flto=auto
export LDFLAGS=-flto=auto

echo Configuring the project
pushd $BUILD_FOLDER
# Building with Wayland support will cause linuxdeploy to include libwayland-client.so in the AppImage.
# Since we always use the host implementation of EGL, this can cause libEGL_mesa.so to fail to load due
# to missing symbols from the host's version of libwayland-client.so that aren't present in the older
# version of libwayland-client.so from our AppImage build environment. When this happens, EGL fails to
# work even in X11. To avoid this, we will disable Wayland support for the AppImage.
#
# We disable DRM support because linuxdeploy doesn't bundle the appropriate libraries for Qt EGLFS.
qmake6 $SOURCE_ROOT/moonlight-qt.pro CONFIG+=disable-wayland CONFIG+=disable-libdrm CONFIG+=usb-host-static PREFIX=$DEPLOY_FOLDER/usr DEFINES+=APP_IMAGE || fail "Qmake failed!"
popd

echo Compiling Moonlight in $BUILD_CONFIG configuration
pushd $BUILD_FOLDER
make -j$(nproc) $(echo "$BUILD_CONFIG" | tr '[:upper:]' '[:lower:]') || fail "Make failed!"
popd

echo Deploying to staging directory
pushd $BUILD_FOLDER
make install || fail "Make install failed!"
popd

# The elevated helper runs outside AppImage's loader environment. Verify that
# it was installed and needs neither the bundled Qt nor C++ runtime libraries.
USB_HELPER="$DEPLOY_FOLDER/usr/bin/moonlight-usb-host"
env -u LD_LIBRARY_PATH "$USB_HELPER" --version || fail "Native USB helper is missing or cannot run!"
USB_HELPER_DYNAMIC=$(readelf -d "$USB_HELPER") || fail "Unable to inspect native USB helper dependencies!"
if grep -E 'NEEDED.*(libQt|libstdc\+\+|libgcc_s)' <<< "$USB_HELPER_DYNAMIC"; then
  fail "Native USB helper must not depend on AppImage runtime libraries!"
fi

echo Updating metadata
perl -pi -e 's/__GITHUB_REF_NAME__/$ENV{GITHUB_REF_NAME}/' $DEPLOY_FOLDER/usr/share/metainfo/com.moonlight_stream.Moonlight.appdata.xml
perl -pi -e 's/__GITHUB_SHA__/$ENV{GITHUB_SHA}/' $DEPLOY_FOLDER/usr/share/metainfo/com.moonlight_stream.Moonlight.appdata.xml

export QML_SOURCES_PATHS=$SOURCE_ROOT/app/gui
export QMAKE=qmake6

# Stage the build-environment libva in opt/libva-fallback (outside usr/, so linuxdeploy
# does not scan it, and outside every loader search path), together with a small probe
# binary that carries the same libva ELF requirements as Moonlight. The AppRun below
# uses the probe to decide, via the dynamic loader itself, whether the host libva can
# satisfy Moonlight; when it cannot, the fallback copy is made visible. opt/ is a
# standard linuxdeploy location for application data that must not be processed.
LIBVA_FALLBACK_DIR=$DEPLOY_FOLDER/opt/libva-fallback
SYSTEM_LIBVA=$(ldconfig -p 2>/dev/null | awk '/libva\.so\.2/{print $NF; exit}')
mkdir -p $LIBVA_FALLBACK_DIR
if [ -n "$SYSTEM_LIBVA" ]; then
  cp -a "$(dirname "$SYSTEM_LIBVA")"/libva*.so* $LIBVA_FALLBACK_DIR/ || fail "Unable to stage libva fallback copy!"

  echo Compiling libva-probe
  cc -O2 -L"$(dirname "$SYSTEM_LIBVA")" -Wl,--no-as-needed -o $LIBVA_FALLBACK_DIR/libva-probe \
    $SOURCE_ROOT/app/deploy/linux/libva-probe.c -lva -lva-x11 || fail "Unable to compile libva-probe!"

  # Keep the probe honest: it must reference every VA_API_* version node that the
  # binaries shipped in the AppImage require, otherwise a host libva could pass the
  # probe and still fail to load Moonlight or the bundled FFmpeg.
  va_nodes() { LC_ALL=C readelf -V "$1" 2>/dev/null | grep -oE 'VA_API_[0-9]+\.[0-9]+\.[0-9]+' | sort -u; }
  NEEDED_NODES=$(for b in $DEPLOY_FOLDER/usr/bin/moonlight \
                          /usr/local/lib*/libav*.so* /usr/local/lib*/libsw*.so* \
                          /usr/lib/x86_64-linux-gnu/libav*.so* /usr/lib/x86_64-linux-gnu/libsw*.so*; do
                   [ -f "$b" ] && va_nodes "$b"; done | sort -u)
  PROBE_NODES=$(va_nodes $LIBVA_FALLBACK_DIR/libva-probe)
  [ -z "$(comm -13 <(echo "$PROBE_NODES") <(echo "$NEEDED_NODES"))" ] || \
    fail "libva-probe is missing version node(s): $(comm -13 <(echo "$PROBE_NODES") <(echo "$NEEDED_NODES")) - update app/deploy/linux/libva-probe.c!"
fi

APP_RUN=$BUILD_ROOT/AppRun-libva
cat > $APP_RUN <<'APPRUN_EOF'
#!/bin/bash
# AppRun: prefer the host libva; use the staged copy only if the host cannot run us.
#
# VA-API driver modules on the host are loaded by libva and export an entrypoint
# named after the libva version they were built against (__vaDriverInit_1_XX).
# A libva can only load drivers that are not newer than itself, so bundling our
# own (older) libva silently breaks hardware decoding on up-to-date distros.
# Distros keep host libva and host drivers in step, so whenever the host libva
# can satisfy Moonlight's own ELF version requirements it is the right choice.
#
# "Can satisfy" is answered here by libva-probe, a tiny program linked against
# the same versioned libva symbols as Moonlight and the bundled FFmpeg: if the
# dynamic loader can start it, the host libva works; if not (no libva installed,
# or one too old to link), we make the staged build-environment copy visible
# via LD_LIBRARY_PATH, matching the pre-existing bundled behavior.
APPDIR="${APPDIR:-$(dirname "$(readlink -f "$0")")}"
LIBVA_FALLBACK="$APPDIR/opt/libva-fallback"

if [ -d "$LIBVA_FALLBACK" ] && ! "$LIBVA_FALLBACK/libva-probe" 2>/dev/null; then
    export LD_LIBRARY_PATH="$LIBVA_FALLBACK${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export VAAPI_USE_FALLBACK_PATHS=1
fi

exec "$APPDIR/usr/bin/moonlight" "$@"
APPRUN_EOF
chmod +x $APP_RUN

echo Creating AppImage
# Remove SQL driver plugins that depend on unavailable system libraries
# (e.g. libqsqlmimer.so -> libmimerapi.so) to prevent linuxdeploy/plugin failures
QT_PLUGIN_PATH=$(qmake6 -query QT_INSTALL_PLUGINS 2>/dev/null)
if [ -n "$QT_PLUGIN_PATH" ] && [ -d "$QT_PLUGIN_PATH/sqldrivers" ]; then
  echo "Removing problematic SQL driver plugins..."
  rm -f "$QT_PLUGIN_PATH/sqldrivers/libqsqlmimer.so"
fi
# Same deal for Qt's GStreamer multimedia backend: linuxdeploy tries to bundle it and
# dies on libgstplay-1.0.so.0 if the build host doesn't have gst-plugins-bad (the
# x86_64 runner image happens to, the aarch64 one doesn't). We only ever use the
# FFmpeg backend -- it's Qt's default on Linux since 6.4 and it's what backs the
# QMediaDevices/QAudioSource path in micstream.cpp -- so just don't ship the GStreamer one.
if [ -n "$QT_PLUGIN_PATH" ] && [ -d "$QT_PLUGIN_PATH/multimedia" ]; then
  echo "Removing GStreamer multimedia backend..."
  rm -f "$QT_PLUGIN_PATH/multimedia/libgstreamermediaplugin.so"
fi

APPIMAGE_PATH="$INSTALLER_FOLDER/Moonlight-VPlus-$VERSION-$APPIMAGE_ARCH.AppImage"
APPIMAGE_UPDATE_INFORMATION="gh-releases-zsync|qiin2333|moonlight-qt|$APPIMAGE_UPDATE_TAG|Moonlight-VPlus-*-${APPIMAGE_ARCH}.AppImage.zsync"

pushd "$INSTALLER_FOLDER" || fail "Unable to enter install folder: $INSTALLER_FOLDER"
# Don't bundle libva (upstream 2026-09): the bundled build-environment libva cannot
# dlopen GPU drivers compiled against a newer libva, so VAAPI silently fell back to
# software decoding on up-to-date hosts. Exclude it and let the AppRun shim above
# decide between host libva and the staged opt/libva-fallback last-resort copy.
LDAI_UPDATE_INFORMATION="$APPIMAGE_UPDATE_INFORMATION" LDAI_OUTPUT="$APPIMAGE_PATH"   VERSION="$VERSION" "$LINUXDEPLOY" --appdir "$DEPLOY_FOLDER"   --library=/usr/local/lib/libSDL3.so.0   --exclude-library=libva.so*   --exclude-library=libva-drm.so*   --exclude-library=libva-wayland.so*   --exclude-library=libva-x11.so*   --custom-apprun "$APP_RUN"   --plugin qt --output appimage || fail "linuxdeploy failed!"
popd || fail "Unable to leave install folder"

echo Verifying AppImage update metadata
[ -s "$APPIMAGE_PATH" ] || fail "AppImage is missing or empty: $APPIMAGE_PATH"

ZSYNC_PATH="${APPIMAGE_PATH}.zsync"
[ -s "$ZSYNC_PATH" ] || fail "zsync metadata is missing or empty: $ZSYNC_PATH"

ACTUAL_UPDATE_INFORMATION=$(env -u APPIMAGE_EXTRACT_AND_RUN \
  "$APPIMAGE_PATH" --appimage-updateinformation) || \
  fail "Unable to read AppImage update information"
[ "$ACTUAL_UPDATE_INFORMATION" = "$APPIMAGE_UPDATE_INFORMATION" ] || \
  fail "Unexpected AppImage update information: $ACTUAL_UPDATE_INFORMATION"

APPIMAGE_FILENAME=$(basename "$APPIMAGE_PATH")
grep -Fqx "Filename: $APPIMAGE_FILENAME" "$ZSYNC_PATH" || \
  fail "zsync Filename does not match $APPIMAGE_FILENAME"
grep -Fqx "URL: $APPIMAGE_FILENAME" "$ZSYNC_PATH" || \
  fail "zsync URL does not match $APPIMAGE_FILENAME"

echo Build successful
