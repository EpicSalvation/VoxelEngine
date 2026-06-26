// Reference gamepad input plugin (M17 C1).
//
// The controller counterpart to the keyboard-mouse plugin: named, rebindable
// button/trigger actions and dead-zoned analog sticks over raw GLFW gamepad
// polling. All POLICY (bindings, dead-zone model, trigger threshold) lives here;
// the engine adds nothing. The host supplies a `RawSource` wrapping
// glfwGetGamepadState; the plugin pulls a flat snapshot through it each frame.
//
// Same shape as the keyboard-mouse plugin: a shared header (gamepad.h) exposes a
// function-pointer table the host calls, init fills the table, and the host
// drives update() directly (input is read before the game step, not in a tick).

#include "gamepad.h"
#include "plugin_api.h"

#include <cmath>
#include <string>
#include <unordered_map>

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif
// Suppressed when compiled in (e.g. the test binary compiles several plugins
// together); the ABI stamp is a single exported symbol. Runtime-loaded builds
// keep it. See the keyboard-mouse plugin for the same guard.
#ifndef GPINPUT_COMPILED_IN
VOXEL_PLUGIN_ABI_STAMP();
#endif

namespace {

enum class Kind { Button, Trigger };

struct ActionBinding {
    Kind kind = Kind::Button;
    int  index = 0;   // button index (Button) or trigger axis index (Trigger)
};

struct AxisBinding {
    int    axis  = 0;
    double scale = 1.0;
};

struct ActionState {
    bool held = false;
    bool prev = false;
};

const gpinput::RawSource*                       g_source = nullptr;
int                                             g_jid    = 0;
double                                          g_deadzone        = 0.15;
double                                          g_triggerThreshold = 0.5;

gpinput::GamepadState                           g_pad{};   // latest snapshot
std::unordered_map<std::string, ActionBinding>  g_actions;
std::unordered_map<std::string, AxisBinding>    g_axes;
std::unordered_map<std::string, ActionState>    g_state;

bool isTriggerAxis(int axis) {
    return axis == gpinput::AxisLeftTrigger || axis == gpinput::AxisRightTrigger;
}

// GLFW triggers rest at -1 and travel to +1; normalize to [0,1].
double normalizedTrigger(int axis) {
    if (axis < 0 || axis >= gpinput::kAxisCount) return 0.0;
    return (static_cast<double>(g_pad.axes[axis]) + 1.0) * 0.5;
}

// 1D dead-zone: zero inside the band, rescaled so the live range starts at 0.
double deadzone1D(double v) {
    double a = std::fabs(v);
    if (a < g_deadzone) return 0.0;
    double scaled = (a - g_deadzone) / (1.0 - g_deadzone);
    return std::copysign(scaled, v);
}

bool buttonDown(int index) {
    if (!g_pad.connected) return false;
    if (index < 0 || index >= gpinput::kButtonCount) return false;
    return g_pad.buttons[index] != 0;
}

bool actionDown(const ActionBinding& b) {
    if (b.kind == Kind::Button) return buttonDown(b.index);
    return g_pad.connected && normalizedTrigger(b.index) > g_triggerThreshold;
}

// ---------------------------------------------------------------------------
// API implementations
// ---------------------------------------------------------------------------

void set_source_impl(const gpinput::RawSource* source) {
    g_source = source;
    g_pad = {};
}

void set_joystick_impl(int jid)            { g_jid = jid; }
void set_deadzone_impl(double deadzone)    { g_deadzone = deadzone; }
void set_trigger_threshold_impl(double t)  { g_triggerThreshold = t; }

void bind_button_impl(const char* action, int button) {
    if (!action) return;
    g_actions[action] = ActionBinding{Kind::Button, button};
    g_state[action];
}

void bind_trigger_impl(const char* action, int trigger_axis) {
    if (!action) return;
    g_actions[action] = ActionBinding{Kind::Trigger, trigger_axis};
    g_state[action];
}

void bind_axis_impl(const char* name, int axis, double scale) {
    if (!name) return;
    g_axes[name] = AxisBinding{axis, scale};
}

void clear_binding_impl(const char* name) {
    if (!name) return;
    g_actions.erase(name);
    g_axes.erase(name);
    g_state.erase(name);
}

void update_impl(double /*dt*/) {
    g_pad = {};
    if (g_source && g_source->poll)
        g_source->poll(g_jid, &g_pad, g_source->user);

    for (auto& [name, binding] : g_actions) {
        ActionState& s = g_state[name];
        s.prev = s.held;
        s.held = actionDown(binding);
    }
}

int connected_impl() { return g_pad.connected ? 1 : 0; }

int held_impl(const char* action) {
    if (!action) return 0;
    auto it = g_state.find(action);
    return (it != g_state.end() && it->second.held) ? 1 : 0;
}

int pressed_impl(const char* action) {
    if (!action) return 0;
    auto it = g_state.find(action);
    return (it != g_state.end() && it->second.held && !it->second.prev) ? 1 : 0;
}

int released_impl(const char* action) {
    if (!action) return 0;
    auto it = g_state.find(action);
    return (it != g_state.end() && !it->second.held && it->second.prev) ? 1 : 0;
}

double axis_impl(const char* name) {
    if (!name) return 0.0;
    auto it = g_axes.find(name);
    if (it == g_axes.end() || !g_pad.connected) return 0.0;
    const AxisBinding& b = it->second;
    if (b.axis < 0 || b.axis >= gpinput::kAxisCount) return 0.0;
    double v = isTriggerAxis(b.axis)
                   ? normalizedTrigger(b.axis)               // triggers: [0,1]
                   : deadzone1D(g_pad.axes[b.axis]);          // sticks: dead-zoned
    return v * b.scale;
}

void stick_impl(int side, double* x, double* y) {
    double ox = 0.0, oy = 0.0;
    if (g_pad.connected) {
        int ax = (side == gpinput::StickRight) ? gpinput::AxisRightX : gpinput::AxisLeftX;
        int ay = (side == gpinput::StickRight) ? gpinput::AxisRightY : gpinput::AxisLeftY;
        double rx = g_pad.axes[ax];
        double ry = g_pad.axes[ay];
        // Radial dead-zone: gate and rescale on the 2D magnitude so a diagonal
        // push is not clipped on either axis.
        double mag = std::sqrt(rx * rx + ry * ry);
        if (mag > g_deadzone) {
            double scaled = (mag - g_deadzone) / (1.0 - g_deadzone);
            if (scaled > 1.0) scaled = 1.0;
            double k = scaled / mag;
            ox = rx * k;
            oy = ry * k;
        }
    }
    if (x) *x = ox;
    if (y) *y = oy;
}

void fillApi() {
    gpinput::api().set_source            = set_source_impl;
    gpinput::api().set_joystick          = set_joystick_impl;
    gpinput::api().set_deadzone          = set_deadzone_impl;
    gpinput::api().set_trigger_threshold = set_trigger_threshold_impl;
    gpinput::api().bind_button           = bind_button_impl;
    gpinput::api().bind_trigger          = bind_trigger_impl;
    gpinput::api().bind_axis             = bind_axis_impl;
    gpinput::api().clear_binding         = clear_binding_impl;
    gpinput::api().update                = update_impl;
    gpinput::api().connected             = connected_impl;
    gpinput::api().held                  = held_impl;
    gpinput::api().pressed               = pressed_impl;
    gpinput::api().released              = released_impl;
    gpinput::api().axis                  = axis_impl;
    gpinput::api().stick                 = stick_impl;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Plugin entry point
// ---------------------------------------------------------------------------

VOXEL_PLUGIN_EXPORT int gamepad_plugin_init(PluginContext* /*ctx*/) {
    fillApi();
    return 0;
}

#ifndef GPINPUT_COMPILED_IN
VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    return gamepad_plugin_init(ctx);
}
#endif
