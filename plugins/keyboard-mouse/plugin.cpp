// Reference keyboard-mouse input plugin (M17 C1).
//
// Turns raw GLFW key / mouse-button / cursor polling into named, rebindable
// game actions, two-key axes, and a sensitivity-scaled mouse-look delta — the
// boilerplate every game otherwise re-derives. All POLICY (the binding table,
// the edge model, sensitivity) lives here; the engine adds NOTHING for this
// plugin. The host owns the window and supplies a `RawSource` poll adapter
// (a few one-line GLFW wrappers); the plugin pulls through it each frame.
//
// Mirrors the kinematic-body plugin (B1): a shared header (keyboard_mouse.h)
// exposes a function-pointer table the host calls; init fills the table. Unlike
// kinbody it needs no engine seam — input is read host-side before the game
// step, so the host drives update() directly rather than through register_on_tick.

#include "keyboard_mouse.h"
#include "plugin_api.h"

#include <string>
#include <unordered_map>

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif
VOXEL_PLUGIN_ABI_STAMP();  // no-op in compiled-in host builds (VOXEL_PLUGIN_NO_ABI_STAMP)

namespace {

// A button action is bound to one physical input — a keyboard key or a mouse
// button. The same code namespace would collide between the two, so the source
// is tracked explicitly.
enum class Source { Key, MouseButton };

struct ActionBinding {
    Source source = Source::Key;
    int    code   = 0;
};

struct AxisBinding {
    int neg_key = 0;
    int pos_key = 0;
};

// Per-frame snapshot of an action's held state, so pressed()/released() can
// report edges against the previous frame deterministically.
struct ActionState {
    bool held = false;
    bool prev = false;
};

const kbinput::RawSource*                         g_source = nullptr;
std::unordered_map<std::string, ActionBinding>    g_actions;
std::unordered_map<std::string, AxisBinding>      g_axes;
std::unordered_map<std::string, ActionState>      g_state;

double g_sensitivity = 0.1;
bool   g_invertY     = false;

bool   g_haveCursor  = false;   // false until the first update establishes a baseline
double g_lastCursorX = 0.0;
double g_lastCursorY = 0.0;
double g_deltaX      = 0.0;
double g_deltaY      = 0.0;

// True if the physical input behind a binding is currently down.
bool rawDown(const ActionBinding& b) {
    if (!g_source) return false;
    if (b.source == Source::Key)
        return g_source->key && g_source->key(b.code, g_source->user) != 0;
    return g_source->mouse_button && g_source->mouse_button(b.code, g_source->user) != 0;
}

bool keyDown(int keycode) {
    return g_source && g_source->key && g_source->key(keycode, g_source->user) != 0;
}

// ---------------------------------------------------------------------------
// API implementations
// ---------------------------------------------------------------------------

void set_source_impl(const kbinput::RawSource* source) {
    g_source     = source;
    g_haveCursor = false;   // re-baseline mouse-look on (re)attach
    g_deltaX = g_deltaY = 0.0;
}

void bind_key_impl(const char* action, int keycode) {
    if (!action) return;
    g_actions[action] = ActionBinding{Source::Key, keycode};
    g_state[action];   // ensure an entry exists so queries are well-defined
}

void bind_mouse_button_impl(const char* action, int button) {
    if (!action) return;
    g_actions[action] = ActionBinding{Source::MouseButton, button};
    g_state[action];
}

void bind_axis_impl(const char* axis, int neg_key, int pos_key) {
    if (!axis) return;
    g_axes[axis] = AxisBinding{neg_key, pos_key};
}

void clear_binding_impl(const char* name) {
    if (!name) return;
    g_actions.erase(name);
    g_axes.erase(name);
    g_state.erase(name);
}

void set_mouse_sensitivity_impl(double sensitivity) { g_sensitivity = sensitivity; }
void set_mouse_invert_y_impl(int invert)            { g_invertY = (invert != 0); }

void update_impl(double /*dt*/) {
    // Action edges: roll current held into prev, then re-sample.
    for (auto& [name, binding] : g_actions) {
        ActionState& s = g_state[name];
        s.prev = s.held;
        s.held = rawDown(binding);
    }

    // Mouse-look delta. The first poll only establishes the baseline so a large
    // initial cursor offset never lurches the camera.
    g_deltaX = g_deltaY = 0.0;
    if (g_source && g_source->cursor) {
        double x = 0.0, y = 0.0;
        g_source->cursor(&x, &y, g_source->user);
        if (g_haveCursor) {
            g_deltaX = (x - g_lastCursorX) * g_sensitivity;
            g_deltaY = (y - g_lastCursorY) * g_sensitivity * (g_invertY ? -1.0 : 1.0);
        }
        g_lastCursorX = x;
        g_lastCursorY = y;
        g_haveCursor  = true;
    }
}

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
    if (it == g_axes.end()) return 0.0;
    double v = 0.0;
    if (keyDown(it->second.pos_key)) v += 1.0;
    if (keyDown(it->second.neg_key)) v -= 1.0;
    return v;
}

void mouse_delta_impl(double* dx, double* dy) {
    if (dx) *dx = g_deltaX;
    if (dy) *dy = g_deltaY;
}

void fillApi() {
    kbinput::api().set_source             = set_source_impl;
    kbinput::api().bind_key               = bind_key_impl;
    kbinput::api().bind_mouse_button      = bind_mouse_button_impl;
    kbinput::api().bind_axis              = bind_axis_impl;
    kbinput::api().clear_binding          = clear_binding_impl;
    kbinput::api().set_mouse_sensitivity  = set_mouse_sensitivity_impl;
    kbinput::api().set_mouse_invert_y     = set_mouse_invert_y_impl;
    kbinput::api().update                 = update_impl;
    kbinput::api().held                   = held_impl;
    kbinput::api().pressed                = pressed_impl;
    kbinput::api().released               = released_impl;
    kbinput::api().axis                   = axis_impl;
    kbinput::api().mouse_delta            = mouse_delta_impl;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Plugin entry point
// ---------------------------------------------------------------------------

// Unique entry point used when the plugin is compiled directly into a binary
// that already contains another voxel_plugin_init (e.g. the test binary).
VOXEL_PLUGIN_EXPORT int keyboard_mouse_plugin_init(PluginContext* /*ctx*/) {
    fillApi();
    return 0;
}

#ifndef KBINPUT_COMPILED_IN
VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    return keyboard_mouse_plugin_init(ctx);
}
#endif
