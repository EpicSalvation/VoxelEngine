#pragma once

// Platform-neutral native window handles.
//
// This is the seam between the windowing layer (src/platform/Window, backed by
// GLFW) and the renderer (src/renderer/BgfxRenderer, backed by bgfx). The window
// layer produces these handles; the renderer consumes them to populate
// bgfx::PlatformData. Neither side depends on the other's third-party library.

namespace platform {

struct NativeWindowHandles {
    // Native window: HWND (Windows), NSWindow* (macOS), X11 Window (Linux/X11),
    // or wl_surface* (Linux/Wayland).
    void* window = nullptr;

    // Native display: X11 Display* or wl_display* on Linux; null on Windows/macOS.
    void* display = nullptr;

    // True when the handles refer to a Wayland surface/display, so the renderer
    // can set bgfx::PlatformData::type accordingly. Always false off Linux.
    bool wayland = false;
};

}  // namespace platform
