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
command -v pkg-config >/dev/null 2>&1 || fail "Unable to find 'pkg-config' in your PATH!"
pkg-config --exists wayland-client || fail "Wayland client development files are required for AppImage builds!"
pkg-config --exists libva-wayland || fail "libva Wayland development files are required for AppImage builds!"

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
# Build both native Wayland and X11 support. linuxdeploy is told below not to bundle
# libwayland-client.so, because the build-environment copy can be older than the host
# copy required by the host's libEGL_mesa.so. Keeping that library host-provided
# preserves the original EGL compatibility fix without compiling Wayland out.
#
# We disable DRM support because linuxdeploy doesn't bundle the appropriate libraries for Qt EGLFS.
qmake6 $SOURCE_ROOT/moonlight-qt.pro CONFIG+=disable-libdrm PREFIX=$DEPLOY_FOLDER/usr DEFINES+=APP_IMAGE || fail "Qmake failed!"
popd

echo Compiling Moonlight in $BUILD_CONFIG configuration
pushd $BUILD_FOLDER
make -j$(nproc) $(echo "$BUILD_CONFIG" | tr '[:upper:]' '[:lower:]') || fail "Make failed!"
popd

echo Deploying to staging directory
pushd $BUILD_FOLDER
make install || fail "Make install failed!"
popd

echo Updating metadata
perl -pi -e 's/__GITHUB_REF_NAME__/$ENV{GITHUB_REF_NAME}/' $DEPLOY_FOLDER/usr/share/metainfo/com.moonlight_stream.Moonlight.appdata.xml
perl -pi -e 's/__GITHUB_SHA__/$ENV{GITHUB_SHA}/' $DEPLOY_FOLDER/usr/share/metainfo/com.moonlight_stream.Moonlight.appdata.xml

export QML_SOURCES_PATHS=$SOURCE_ROOT/app/gui
export QMAKE=qmake6

# Stage the complete build-environment libva in opt/libva-fallback (outside usr/, so
# linuxdeploy does not scan it, and outside every loader search path). Also stage a
# separate directory containing only the Wayland adapter. AppRun can expose that
# adapter without shadowing a compatible host libva core on X11 systems that do not
# install libva-wayland. opt/ is a standard linuxdeploy location for application data
# that must not be processed.
LIBVA_FALLBACK_DIR=$DEPLOY_FOLDER/opt/libva-fallback
LIBVA_WAYLAND_FALLBACK_DIR=$DEPLOY_FOLDER/opt/libva-wayland-fallback
SYSTEM_LIBVA=$(ldconfig -p 2>/dev/null | awk '/libva\.so\.2/{print $NF; exit}')
mkdir -p $LIBVA_FALLBACK_DIR $LIBVA_WAYLAND_FALLBACK_DIR
[ -n "$SYSTEM_LIBVA" ] || fail "Unable to locate build-environment libva for the fallback copy!"

cp -a "$(dirname "$SYSTEM_LIBVA")"/libva*.so* $LIBVA_FALLBACK_DIR/ || fail "Unable to stage libva fallback copy!"
[ -e "$LIBVA_FALLBACK_DIR/libva-wayland.so.2" ] || fail "The staged libva fallback has no Wayland adapter!"
cp -a "$LIBVA_FALLBACK_DIR"/libva-wayland.so* $LIBVA_WAYLAND_FALLBACK_DIR/ || \
  fail "Unable to stage the libva Wayland adapter-only fallback!"

echo Compiling libva probes
cc -O2 -L"$(dirname "$SYSTEM_LIBVA")" -Wl,--no-as-needed -o $LIBVA_FALLBACK_DIR/libva-probe \
  $SOURCE_ROOT/app/deploy/linux/libva-probe.c -lva -lva-x11 || fail "Unable to compile libva-probe!"
cc -O2 -DLIBVA_PROBE_WAYLAND -L"$(dirname "$SYSTEM_LIBVA")" -Wl,--no-as-needed \
  -o $LIBVA_FALLBACK_DIR/libva-wayland-probe $SOURCE_ROOT/app/deploy/linux/libva-probe.c \
  -lva -lva-wayland || fail "Unable to compile libva-wayland-probe!"

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

APP_RUN=$BUILD_ROOT/AppRun-libva
cat > $APP_RUN <<'APPRUN_EOF'
#!/bin/bash
# AppRun: prefer the host libva; use staged libraries only when the host cannot run us.
#
# VA-API driver modules on the host are loaded by libva and export an entrypoint
# named after the libva version they were built against (__vaDriverInit_1_XX).
# A libva can only load drivers that are not newer than itself, so bundling our
# own (older) libva silently breaks hardware decoding on up-to-date distros.
# Distros keep host libva and host drivers in step, so whenever the host libva
# can satisfy Moonlight's own ELF version requirements it is the right choice.
#
# "Can satisfy" is answered here by tiny probe programs linked against the same
# versioned libva symbols as Moonlight and the bundled FFmpeg. The core/X11 probe
# decides whether the complete fallback is necessary. The Wayland probe is kept
# separate so a missing host Wayland adapter can be supplied without shadowing an
# otherwise compatible host libva core and its matching GPU drivers.
APPDIR="${APPDIR:-$(dirname "$(readlink -f "$0")")}"
LIBVA_FALLBACK="$APPDIR/opt/libva-fallback"
LIBVA_WAYLAND_FALLBACK="$APPDIR/opt/libva-wayland-fallback"

use_full_libva_fallback()
{
    export LD_LIBRARY_PATH="$LIBVA_FALLBACK${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export VAAPI_USE_FALLBACK_PATHS=1
}

if [ -d "$LIBVA_FALLBACK" ] && ! "$LIBVA_FALLBACK/libva-probe" 2>/dev/null; then
    use_full_libva_fallback
elif [ -d "$LIBVA_FALLBACK" ] && ! "$LIBVA_FALLBACK/libva-wayland-probe" 2>/dev/null; then
    WAYLAND_LIBRARY_PATH="$LIBVA_WAYLAND_FALLBACK${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    if [ -d "$LIBVA_WAYLAND_FALLBACK" ] && \
       LD_LIBRARY_PATH="$WAYLAND_LIBRARY_PATH" "$LIBVA_FALLBACK/libva-wayland-probe" 2>/dev/null; then
        export LD_LIBRARY_PATH="$WAYLAND_LIBRARY_PATH"
    else
        use_full_libva_fallback
    fi
fi

exec "$APPDIR/usr/bin/moonlight" "$@"
APPRUN_EOF
chmod +x $APP_RUN

echo Creating AppImage
# Remove SQL driver plugins that depend on unavailable system libraries
# (e.g. libqsqlmimer.so -> libmimerapi.so) to prevent linuxdeploy/plugin failures
QT_PLUGIN_PATH=$(qmake6 -query QT_INSTALL_PLUGINS 2>/dev/null)
[ -d "$QT_PLUGIN_PATH/platforms" ] || fail "Unable to locate the Qt platform plugin directory!"

# Qt 6.11 uses a single libqwayland.so plugin, while older Qt 6 releases use
# libqwayland-egl.so and libqwayland-generic.so. Discover the installed names
# so the packaging policy remains explicit without silently omitting Wayland.
WAYLAND_PLATFORM_PLUGINS=
for plugin_path in "$QT_PLUGIN_PATH"/platforms/libqwayland*.so; do
  [ -e "$plugin_path" ] || continue
  plugin_name=$(basename "$plugin_path")
  WAYLAND_PLATFORM_PLUGINS="${WAYLAND_PLATFORM_PLUGINS:+$WAYLAND_PLATFORM_PLUGINS;}$plugin_name"
done
[ -n "$WAYLAND_PLATFORM_PLUGINS" ] || fail "Qt Wayland platform plugins are not installed!"

export EXTRA_QT_MODULES="${EXTRA_QT_MODULES:+$EXTRA_QT_MODULES;}waylandcompositor"
export EXTRA_PLATFORM_PLUGINS="${EXTRA_PLATFORM_PLUGINS:+$EXTRA_PLATFORM_PLUGINS;}$WAYLAND_PLATFORM_PLUGINS"

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
LDAI_UPDATE_INFORMATION="$APPIMAGE_UPDATE_INFORMATION" \
LDAI_OUTPUT="$APPIMAGE_PATH" \
VERSION="$VERSION" \
"$LINUXDEPLOY" --appdir "$DEPLOY_FOLDER" \
  --library=/usr/local/lib/libSDL3.so.0 \
  --exclude-library=libva.so* \
  --exclude-library=libva-drm.so* \
  --exclude-library=libva-wayland.so* \
  --exclude-library=libva-x11.so* \
  --exclude-library=libwayland-client.so* \
  --custom-apprun "$APP_RUN" \
  --plugin qt --output appimage || fail "linuxdeploy failed!"
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

echo Verifying native Wayland and X11 fallback payload
VERIFY_ROOT=$(mktemp -d "$BUILD_ROOT/appimage-verify.XXXXXX") || fail "Unable to create AppImage verification directory!"
(
  cd "$VERIFY_ROOT" || exit 1
  env -u APPIMAGE_EXTRACT_AND_RUN "$APPIMAGE_PATH" --appimage-extract >/dev/null
) || fail "Unable to extract AppImage for verification!"
EXTRACTED_APPDIR=$VERIFY_ROOT/squashfs-root

[ -e "$EXTRACTED_APPDIR/usr/plugins/platforms/libqxcb.so" ] || \
  fail "AppImage is missing the Qt XCB platform plugin!"
find "$EXTRACTED_APPDIR/usr/plugins/platforms" -maxdepth 1 -name 'libqwayland*.so' -print -quit | grep -q . || \
  fail "AppImage is missing the Qt Wayland platform plugin!"
find "$EXTRACTED_APPDIR/usr" -name 'libQt6WaylandClient.so*' -print -quit | grep -q . || \
  fail "AppImage is missing the Qt Wayland client library!"

for plugin_dir in wayland-decoration-client wayland-graphics-integration-client wayland-shell-integration; do
  find "$EXTRACTED_APPDIR/usr/plugins/$plugin_dir" -maxdepth 1 -type f -print -quit 2>/dev/null | grep -q . || \
    fail "AppImage is missing Qt's $plugin_dir plugins!"
done

readelf -d "$EXTRACTED_APPDIR/usr/bin/moonlight" | grep -q 'libwayland-client\.so' || \
  fail "Moonlight was built without native Wayland support!"
readelf -d "$EXTRACTED_APPDIR/usr/bin/moonlight" | grep -q 'libva-wayland\.so' || \
  fail "Moonlight was built without VA-API Wayland support!"
[ -e "$EXTRACTED_APPDIR/opt/libva-wayland-fallback/libva-wayland.so.2" ] || \
  fail "AppImage is missing the libva Wayland adapter-only fallback!"
[ ! -e "$EXTRACTED_APPDIR/opt/libva-wayland-fallback/libva.so.2" ] || \
  fail "The libva Wayland adapter-only fallback unexpectedly contains the libva core!"
[ ! -e "$EXTRACTED_APPDIR/opt/libva-wayland-fallback/libva-x11.so.2" ] || \
  fail "The libva Wayland adapter-only fallback unexpectedly contains the X11 adapter!"

if find "$EXTRACTED_APPDIR/usr" -name 'libwayland-client.so*' -print -quit | grep -q .; then
  fail "AppImage bundled libwayland-client and may conflict with the host's Mesa/EGL stack!"
fi

rm -rf "$VERIFY_ROOT"

echo Build successful
