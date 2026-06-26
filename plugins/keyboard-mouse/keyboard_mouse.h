#pragma once

// Shared API for the keyboard-mouse reference input plugin (M17 C1).
//
// One of the two reference input plugins the engine ships. It provides the
// common boilerplate an indie dev (or AI agent) would otherwise hand-code on
// top of raw GLFW polling: named **action mapping**, **rebindable** keys/mouse
// buttons, two-key **axes** (e.g. W/S -> forward), edge detection
// (pressed / released this frame), and sensitivity-scaled **mouse-look** deltas.
//
// Like the kinematic-body plugin (M17 B1), the plugin fills a global API table
// at init and the host calls its function pointers. The host owns the window
// (and therefore GLFW), so it supplies a tiny `RawSource` poll adapter wrapping
// glfwGetKey / glfwGetMouseButton / glfwGetCursorPos; the plugin pulls raw state
// through it and does all the mapping. No GLFW type crosses this boundary —
// key/button codes are plain ints (GLFW's stable GLFW_KEY_* / GLFW_MOUSE_BUTTON_*
// constants), so the plugin stays GLFW-free and runtime-loadable. Developers can
// use it as-is, extend it, or replace it.

#include <cstdint>

namespace kbinput {

// Raw hardware poll adapter the host installs. The plugin pulls through these
// each frame; the host implements them as one-line wrappers around GLFW (the
// host owns the window). `user` is passed back to each callback unmodified
// (typically the GLFWwindow*).
struct RawSource {
    // Return non-zero if the key (a GLFW key code) is currently down.
    int  (*key)(int keycode, void* user)          = nullptr;
    // Return non-zero if the mouse button (a GLFW mouse-button code) is down.
    int  (*mouse_button)(int button, void* user)  = nullptr;
    // Write the current cursor position (pixels) to *x, *y.
    void (*cursor)(double* x, double* y, void* user) = nullptr;
    void* user = nullptr;
};

// Global function-pointer table filled by the plugin at init.
struct API {
    // Install the host's raw poll adapter. Must be set before update().
    // Passing nullptr detaches (update() then becomes a no-op).
    void (*set_source)(const RawSource* source) = nullptr;

    // Bind (or rebind) a named button action to a keyboard key (GLFW key code).
    // Rebinding an existing action replaces its previous binding.
    void (*bind_key)(const char* action, int keycode) = nullptr;

    // Bind (or rebind) a named button action to a mouse button (GLFW code).
    void (*bind_mouse_button)(const char* action, int button) = nullptr;

    // Bind (or rebind) a named axis to two keys: neg_key yields -1, pos_key
    // yields +1, both or neither yields 0.
    void (*bind_axis)(const char* axis, int neg_key, int pos_key) = nullptr;

    // Remove any action or axis binding for `name`. Idempotent.
    void (*clear_binding)(const char* name) = nullptr;

    // Mouse-look configuration. sensitivity scales the raw pixel delta reported
    // by mouse_delta (default 0.1); invert_y flips the Y delta sign.
    void (*set_mouse_sensitivity)(double sensitivity) = nullptr;
    void (*set_mouse_invert_y)(int invert) = nullptr;

    // Poll the raw source and recompute action edge state + mouse delta. Call
    // once per frame after the window pumps events and before reading actions.
    void (*update)(double dt) = nullptr;

    // Queries — valid after update().
    int    (*held)(const char* action)     = nullptr;  // currently down
    int    (*pressed)(const char* action)  = nullptr;  // went down this frame
    int    (*released)(const char* action) = nullptr;  // went up this frame
    double (*axis)(const char* name)       = nullptr;  // [-1, 1]

    // Mouse-look delta since the previous update(), scaled by sensitivity and
    // Y-inverted if configured. The first update() after a source is installed
    // reports (0, 0) so a large initial cursor jump is swallowed.
    void   (*mouse_delta)(double* dx, double* dy) = nullptr;
};

// Singleton accessor. The plugin fills this during init; the host reads it after
// loading the plugin. Single-writer (plugin init) / single-reader (host loop).
inline API& api() {
    static API instance;
    return instance;
}

}  // namespace kbinput
