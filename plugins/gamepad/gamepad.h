#pragma once

// Shared API for the gamepad reference input plugin (M17 C1).
//
// The controller counterpart to the keyboard-mouse plugin. It provides the
// boilerplate over raw GLFW gamepad polling: named **action mapping** to
// buttons, rebindable bindings, trigger-as-button bindings, and — the piece a
// controller specifically needs — **dead-zone handling** for the analog sticks
// (a radial dead-zone on each stick so diagonal input is not clipped per-axis).
//
// Same pattern as keyboard_mouse.h: the plugin fills a global API table at init;
// the host supplies a `RawSource` adapter wrapping glfwGetGamepadState. No GLFW
// type crosses this boundary — `GamepadState` is a flat POD mirroring GLFW's
// layout, and button/axis indices are plain ints (GLFW's GLFW_GAMEPAD_BUTTON_*
// / GLFW_GAMEPAD_AXIS_* constants), so the plugin stays GLFW-free and
// runtime-loadable.

#include <cstdint>

namespace gpinput {

// Number of buttons / axes in GLFW's standard gamepad mapping. Mirrors
// GLFW_GAMEPAD_BUTTON_LAST + 1 and GLFW_GAMEPAD_AXIS_LAST + 1.
static constexpr int kButtonCount = 15;
static constexpr int kAxisCount   = 6;

// Standard-mapping axis indices (== GLFW_GAMEPAD_AXIS_*), for stick() side and
// readability at call sites that don't include GLFW.
enum Axis : int {
    AxisLeftX        = 0,
    AxisLeftY        = 1,
    AxisRightX       = 2,
    AxisRightY       = 3,
    AxisLeftTrigger  = 4,
    AxisRightTrigger = 5,
};

// Which stick stick() reads.
enum Stick : int { StickLeft = 0, StickRight = 1 };

// Flat snapshot of one gamepad, filled by the host from glfwGetGamepadState.
// Field layout matches GLFWgamepadstate (buttons then axes) but the host copies
// values across, so no GLFW type is named here. axes are [-1,1]; sticks rest at
// 0, triggers rest at -1.
struct GamepadState {
    int           connected = 0;   // non-zero if a mapped gamepad is present
    float         axes[kAxisCount]      = {};
    unsigned char buttons[kButtonCount] = {};
};

// Raw poll adapter the host installs. The plugin calls poll() each update for
// the configured joystick id; the host wraps glfwJoystickIsGamepad +
// glfwGetGamepadState (leaving connected == 0 when absent).
struct RawSource {
    void  (*poll)(int jid, GamepadState* out, void* user) = nullptr;
    void* user = nullptr;
};

// Global function-pointer table filled by the plugin at init.
struct API {
    // Install the host's raw poll adapter. Must be set before update().
    void (*set_source)(const RawSource* source) = nullptr;

    // Which joystick id to read (default 0 == GLFW_JOYSTICK_1).
    void (*set_joystick)(int jid) = nullptr;

    // Radial dead-zone applied to stick() and to axis() for the stick axes —
    // input below this magnitude reads 0, above it is rescaled so the live
    // range starts at 0 (default 0.15).
    void (*set_deadzone)(double deadzone) = nullptr;

    // Normalized-trigger press threshold for bind_trigger actions (default 0.5).
    void (*set_trigger_threshold)(double threshold) = nullptr;

    // Bind (or rebind) a named action to a gamepad button (GLFW_GAMEPAD_BUTTON_*).
    void (*bind_button)(const char* action, int button) = nullptr;

    // Bind (or rebind) a named action to a trigger axis (AxisLeftTrigger /
    // AxisRightTrigger): "down" when the trigger's normalized [0,1] value
    // exceeds the trigger threshold.
    void (*bind_trigger)(const char* action, int trigger_axis) = nullptr;

    // Bind (or rebind) a named single-axis value to a gamepad axis, scaled by
    // `scale` (use -1 to invert). Stick axes are dead-zoned; trigger axes are
    // returned normalized to [0,1].
    void (*bind_axis)(const char* name, int axis, double scale) = nullptr;

    // Remove any action or axis binding for `name`. Idempotent.
    void (*clear_binding)(const char* name) = nullptr;

    // Poll the source and recompute connection, edges, and stick values. Call
    // once per frame before reading actions.
    void (*update)(double dt) = nullptr;

    // Queries — valid after update().
    int    (*connected)()                  = nullptr;  // a gamepad is present
    int    (*held)(const char* action)     = nullptr;
    int    (*pressed)(const char* action)  = nullptr;
    int    (*released)(const char* action) = nullptr;
    double (*axis)(const char* name)       = nullptr;

    // Dead-zoned 2D stick. side is StickLeft / StickRight. The radial dead-zone
    // is applied to the (x, y) magnitude so a diagonal push is not clipped on
    // either axis. Writes (0, 0) when no gamepad is connected.
    void   (*stick)(int side, double* x, double* y) = nullptr;
};

// Singleton accessor. The plugin fills this during init; the host reads it after
// loading the plugin.
inline API& api() {
    static API instance;
    return instance;
}

}  // namespace gpinput
