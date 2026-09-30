/*
 * libva-probe: loadability oracle for the AppImage AppRun.
 *
 * Moonlight and the bundled FFmpeg import libva through symbols carrying
 * VA_API_* ELF version nodes. Whether the libva installed on the host can
 * satisfy those requirements cannot be inferred portably from file names or
 * loader caches, so this program embeds the same versioned symbol references
 * as those binaries without ever calling into them. It is compiled once for
 * the core/X11 libraries and again with LIBVA_PROBE_WAYLAND for the Wayland
 * adapter. This lets AppRun distinguish a missing Wayland adapter from an
 * incompatible host libva core instead of replacing the whole host stack.
 *
 * scripts/build-appimage.sh verifies at build time that this file references
 * every VA_API_* node required by the binaries shipped in the AppImage, so
 * it stays automatically in sync with the libva usage there.
 */

#include <stddef.h>

#include <va/va.h>
#ifdef LIBVA_PROBE_WAYLAND
#include <va/va_wayland.h>
#else
#include <va/va_x11.h>
#endif

/* Address references are enough: they produce relocations against the
 * versioned symbols, which is what makes the loader check the host's
 * VA_API_* version nodes. The functions must never actually be called.
 * volatile keeps the table (and its relocations) alive through -O2. */
static void *volatile requirements[] = {
    (void *)vaCreateSurfaces, /* libva.so.2, VA_API_0.33.0 */
    (void *)vaMapBuffer2,     /* libva.so.2, 2.21+ */
#ifdef LIBVA_PROBE_WAYLAND
    (void *)vaGetDisplayWl, /* libva-wayland.so.2 */
#else
    (void *)vaGetDisplay, /* libva-x11.so.2 */
#endif
};

int main(void)
{
    return requirements[0] == NULL;
}
