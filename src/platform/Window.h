#pragma once

#include "platform/NativeWindowHandles.h"

#include <string>

struct GLFWwindow;

namespace platform {

// A GLFW-backed application window.
//
// The window creates no graphics context of its own (GLFW_NO_API) — bgfx owns
// the device and renders into the native handle exposed by nativeHandles().
// One Window owns GLFW's global state for the process; construct exactly one.
//
// Linux note: X11 is requested explicitly for now. Wayland support is a planned
// follow-up (see docs/ARCHITECTURE.md §9).
class Window {
public:
    // Throws std::runtime_error if GLFW or window creation fails.
    Window(int width, int height, const std::string& title);
    ~Window();

    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;

    // Native handles for the renderer to populate bgfx::PlatformData.
    NativeWindowHandles nativeHandles() const;

    bool shouldClose() const;
    void pollEvents();

    // Framebuffer size in pixels (may differ from window size on HiDPI displays).
    void framebufferSize(int& width, int& height) const;

    GLFWwindow* glfwHandle() const { return window_; }

private:
    GLFWwindow* window_ = nullptr;
};

}  // namespace platform
