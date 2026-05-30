#include "platform/Window.h"

#include <GLFW/glfw3.h>

// Native-access entry points must be requested per platform before including
// the native header. bgfx renders into the handles these expose.
#if defined(_WIN32)
  #define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
  #define GLFW_EXPOSE_NATIVE_COCOA
#elif defined(__linux__)
  #define GLFW_EXPOSE_NATIVE_X11
#endif
#include <GLFW/glfw3native.h>

#include <cstdint>
#include <cstdio>
#include <stdexcept>

namespace platform {

namespace {
void glfwErrorCallback(int code, const char* description) {
    std::fprintf(stderr, "[Window] GLFW error %d: %s\n", code, description);
}
}  // namespace

Window::Window(int width, int height, const std::string& title) {
    glfwSetErrorCallback(glfwErrorCallback);

#if defined(__linux__) && defined(GLFW_PLATFORM_X11)
    // Force X11 for now; native-handle wiring for Wayland is a planned
    // follow-up (see docs/ARCHITECTURE.md §9). On a Wayland session this runs
    // through XWayland.
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif

    if (!glfwInit())
        throw std::runtime_error("[Window] glfwInit failed");

    // bgfx owns the graphics device; GLFW must not create a GL/GLES context.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    window_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!window_) {
        glfwTerminate();
        throw std::runtime_error("[Window] glfwCreateWindow failed");
    }
}

Window::~Window() {
    if (window_) glfwDestroyWindow(window_);
    glfwTerminate();
}

NativeWindowHandles Window::nativeHandles() const {
    NativeWindowHandles handles;
#if defined(_WIN32)
    handles.window = static_cast<void*>(glfwGetWin32Window(window_));
#elif defined(__APPLE__)
    handles.window = glfwGetCocoaWindow(window_);  // 'id' is void* in non-ObjC builds
#elif defined(__linux__)
    handles.window =
        reinterpret_cast<void*>(static_cast<uintptr_t>(glfwGetX11Window(window_)));
    handles.display = glfwGetX11Display();
#endif
    return handles;
}

bool Window::shouldClose() const {
    return glfwWindowShouldClose(window_) != 0;
}

void Window::pollEvents() {
    glfwPollEvents();
}

void Window::framebufferSize(int& width, int& height) const {
    glfwGetFramebufferSize(window_, &width, &height);
}

}  // namespace platform
