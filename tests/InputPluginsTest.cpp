// Tests for the reference input plugins (M17 C1): the keyboard-mouse mapper and
// the gamepad mapper. Both are driven through fake RawSource adapters, so no
// GLFW or window is needed — the boilerplate under test (action mapping,
// rebinding, two-key axes, edge detection, mouse-look deltas, and gamepad
// dead-zone handling) is exercised against scripted raw state.

#include "keyboard_mouse.h"
#include "gamepad.h"
#include "plugin_api.h"

#include <gtest/gtest.h>

#include <set>

// The plugin sources are compiled into this test binary (see CMakeLists.txt),
// so kbinput::api() / gpinput::api() resolve in one address space. Unique init
// names avoid clashing with ExamplePlugin's voxel_plugin_init.
extern "C" int keyboard_mouse_plugin_init(PluginContext* ctx);
extern "C" int gamepad_plugin_init(PluginContext* ctx);

namespace {

// ---------------------------------------------------------------------------
// Fake keyboard/mouse raw source
// ---------------------------------------------------------------------------

std::set<int> g_keysDown;
std::set<int> g_buttonsDown;
double        g_cursorX = 0.0;
double        g_cursorY = 0.0;

int  fakeKey(int code, void*)        { return g_keysDown.count(code) ? 1 : 0; }
int  fakeButton(int code, void*)     { return g_buttonsDown.count(code) ? 1 : 0; }
void fakeCursor(double* x, double* y, void*) { *x = g_cursorX; *y = g_cursorY; }

kbinput::RawSource makeKbSource() {
    kbinput::RawSource s;
    s.key          = fakeKey;
    s.mouse_button = fakeButton;
    s.cursor       = fakeCursor;
    s.user         = nullptr;
    return s;
}

void resetKbRaw() {
    g_keysDown.clear();
    g_buttonsDown.clear();
    g_cursorX = g_cursorY = 0.0;
}

// GLFW-equivalent codes used by the tests (values are arbitrary for the fake).
constexpr int KEY_W = 87, KEY_S = 83, KEY_A = 65, KEY_D = 68, KEY_SPACE = 32;
constexpr int MOUSE_LEFT = 0;

void setupKb() {
    keyboard_mouse_plugin_init(nullptr);
    resetKbRaw();
    static kbinput::RawSource src = makeKbSource();
    kbinput::api().set_source(&src);
    // Clear any bindings left over from a prior test (global mapper state).
    kbinput::api().clear_binding("jump");
    kbinput::api().clear_binding("forward");
    kbinput::api().clear_binding("strafe");
    kbinput::api().clear_binding("fire");
}

// ---------------------------------------------------------------------------
// Fake gamepad raw source
// ---------------------------------------------------------------------------

gpinput::GamepadState g_pad{};

void fakePoll(int /*jid*/, gpinput::GamepadState* out, void*) { *out = g_pad; }

gpinput::RawSource makeGpSource() {
    gpinput::RawSource s;
    s.poll = fakePoll;
    s.user = nullptr;
    return s;
}

void setupGp() {
    gamepad_plugin_init(nullptr);
    g_pad = {};
    g_pad.connected = 1;
    static gpinput::RawSource src = makeGpSource();
    gpinput::api().set_source(&src);
    gpinput::api().set_deadzone(0.15);
    gpinput::api().set_trigger_threshold(0.5);
    gpinput::api().clear_binding("jump");
    gpinput::api().clear_binding("fire");
    gpinput::api().clear_binding("turn");
}

}  // namespace

// ===========================================================================
// Keyboard-mouse plugin
// ===========================================================================

TEST(KeyboardMouse, ApiTableFilled) {
    keyboard_mouse_plugin_init(nullptr);
    EXPECT_NE(kbinput::api().bind_key, nullptr);
    EXPECT_NE(kbinput::api().update, nullptr);
    EXPECT_NE(kbinput::api().held, nullptr);
    EXPECT_NE(kbinput::api().mouse_delta, nullptr);
}

TEST(KeyboardMouse, ActionHeldFollowsKey) {
    setupKb();
    kbinput::api().bind_key("jump", KEY_SPACE);

    kbinput::api().update(0.016);
    EXPECT_FALSE(kbinput::api().held("jump"));

    g_keysDown.insert(KEY_SPACE);
    kbinput::api().update(0.016);
    EXPECT_TRUE(kbinput::api().held("jump"));
}

TEST(KeyboardMouse, PressedAndReleasedAreEdges) {
    setupKb();
    kbinput::api().bind_key("jump", KEY_SPACE);

    kbinput::api().update(0.016);  // baseline: up

    g_keysDown.insert(KEY_SPACE);
    kbinput::api().update(0.016);
    EXPECT_TRUE(kbinput::api().pressed("jump"));
    EXPECT_TRUE(kbinput::api().held("jump"));
    EXPECT_FALSE(kbinput::api().released("jump"));

    // Held across a second frame: no longer a press edge.
    kbinput::api().update(0.016);
    EXPECT_FALSE(kbinput::api().pressed("jump"));
    EXPECT_TRUE(kbinput::api().held("jump"));

    g_keysDown.erase(KEY_SPACE);
    kbinput::api().update(0.016);
    EXPECT_TRUE(kbinput::api().released("jump"));
    EXPECT_FALSE(kbinput::api().held("jump"));
}

TEST(KeyboardMouse, RebindMovesTheTrigger) {
    setupKb();
    kbinput::api().bind_key("jump", KEY_SPACE);
    g_keysDown.insert(KEY_W);

    // Rebind jump to W; pressing W now drives it.
    kbinput::api().bind_key("jump", KEY_W);
    kbinput::api().update(0.016);
    EXPECT_TRUE(kbinput::api().held("jump"));

    // The old key no longer affects the action.
    g_keysDown.clear();
    g_keysDown.insert(KEY_SPACE);
    kbinput::api().update(0.016);
    EXPECT_FALSE(kbinput::api().held("jump"));
}

TEST(KeyboardMouse, MouseButtonAction) {
    setupKb();
    kbinput::api().bind_mouse_button("fire", MOUSE_LEFT);

    kbinput::api().update(0.016);
    EXPECT_FALSE(kbinput::api().held("fire"));

    g_buttonsDown.insert(MOUSE_LEFT);
    kbinput::api().update(0.016);
    EXPECT_TRUE(kbinput::api().held("fire"));
    EXPECT_TRUE(kbinput::api().pressed("fire"));
}

TEST(KeyboardMouse, TwoKeyAxis) {
    setupKb();
    kbinput::api().bind_axis("forward", KEY_S, KEY_W);  // S = -1, W = +1

    kbinput::api().update(0.016);
    EXPECT_DOUBLE_EQ(kbinput::api().axis("forward"), 0.0);

    g_keysDown.insert(KEY_W);
    kbinput::api().update(0.016);
    EXPECT_DOUBLE_EQ(kbinput::api().axis("forward"), 1.0);

    g_keysDown.insert(KEY_S);  // both pressed cancel
    kbinput::api().update(0.016);
    EXPECT_DOUBLE_EQ(kbinput::api().axis("forward"), 0.0);

    g_keysDown.erase(KEY_W);
    kbinput::api().update(0.016);
    EXPECT_DOUBLE_EQ(kbinput::api().axis("forward"), -1.0);
}

TEST(KeyboardMouse, UnknownActionAndAxisAreInert) {
    setupKb();
    kbinput::api().update(0.016);
    EXPECT_FALSE(kbinput::api().held("nope"));
    EXPECT_FALSE(kbinput::api().pressed("nope"));
    EXPECT_DOUBLE_EQ(kbinput::api().axis("nope"), 0.0);
}

TEST(KeyboardMouse, MouseDeltaBaselineThenScaled) {
    setupKb();
    kbinput::api().set_mouse_sensitivity(0.5);

    g_cursorX = 100.0; g_cursorY = 100.0;
    kbinput::api().update(0.016);  // first poll only establishes baseline
    double dx = 9.0, dy = 9.0;
    kbinput::api().mouse_delta(&dx, &dy);
    EXPECT_DOUBLE_EQ(dx, 0.0);
    EXPECT_DOUBLE_EQ(dy, 0.0);

    g_cursorX = 110.0; g_cursorY = 90.0;
    kbinput::api().update(0.016);
    kbinput::api().mouse_delta(&dx, &dy);
    EXPECT_DOUBLE_EQ(dx, 10.0 * 0.5);    // +10 px * sensitivity
    EXPECT_DOUBLE_EQ(dy, -10.0 * 0.5);   // -10 px * sensitivity
}

TEST(KeyboardMouse, MouseInvertY) {
    setupKb();
    kbinput::api().set_mouse_sensitivity(1.0);
    kbinput::api().set_mouse_invert_y(1);

    g_cursorX = 0.0; g_cursorY = 0.0;
    kbinput::api().update(0.016);  // baseline
    g_cursorY = 5.0;
    kbinput::api().update(0.016);
    double dx = 0.0, dy = 0.0;
    kbinput::api().mouse_delta(&dx, &dy);
    EXPECT_DOUBLE_EQ(dy, -5.0);  // inverted
}

// ===========================================================================
// Gamepad plugin
// ===========================================================================

TEST(Gamepad, ApiTableFilled) {
    gamepad_plugin_init(nullptr);
    EXPECT_NE(gpinput::api().bind_button, nullptr);
    EXPECT_NE(gpinput::api().stick, nullptr);
    EXPECT_NE(gpinput::api().update, nullptr);
}

TEST(Gamepad, DisconnectedReadsInert) {
    setupGp();
    g_pad.connected = 0;
    gpinput::api().bind_button("jump", 0);
    gpinput::api().update(0.016);
    EXPECT_FALSE(gpinput::api().connected());
    EXPECT_FALSE(gpinput::api().held("jump"));
    double x = 9.0, y = 9.0;
    gpinput::api().stick(gpinput::StickLeft, &x, &y);
    EXPECT_DOUBLE_EQ(x, 0.0);
    EXPECT_DOUBLE_EQ(y, 0.0);
}

TEST(Gamepad, ButtonActionAndEdges) {
    setupGp();
    constexpr int BUTTON_A = 0;
    gpinput::api().bind_button("jump", BUTTON_A);

    gpinput::api().update(0.016);
    EXPECT_FALSE(gpinput::api().held("jump"));

    g_pad.buttons[BUTTON_A] = 1;
    gpinput::api().update(0.016);
    EXPECT_TRUE(gpinput::api().held("jump"));
    EXPECT_TRUE(gpinput::api().pressed("jump"));

    gpinput::api().update(0.016);
    EXPECT_FALSE(gpinput::api().pressed("jump"));

    g_pad.buttons[BUTTON_A] = 0;
    gpinput::api().update(0.016);
    EXPECT_TRUE(gpinput::api().released("jump"));
}

TEST(Gamepad, TriggerAsButtonCrossesThreshold) {
    setupGp();
    gpinput::api().set_trigger_threshold(0.5);
    gpinput::api().bind_trigger("fire", gpinput::AxisRightTrigger);

    // Resting trigger is -1 (normalized 0) -> not pressed.
    g_pad.axes[gpinput::AxisRightTrigger] = -1.0f;
    gpinput::api().update(0.016);
    EXPECT_FALSE(gpinput::api().held("fire"));

    // Half-pressed: normalized 0.5, not strictly above threshold.
    g_pad.axes[gpinput::AxisRightTrigger] = 0.0f;
    gpinput::api().update(0.016);
    EXPECT_FALSE(gpinput::api().held("fire"));

    // Fully pressed: normalized 1.0 -> pressed.
    g_pad.axes[gpinput::AxisRightTrigger] = 1.0f;
    gpinput::api().update(0.016);
    EXPECT_TRUE(gpinput::api().held("fire"));
}

TEST(Gamepad, StickRadialDeadzone) {
    setupGp();
    gpinput::api().set_deadzone(0.2);

    // Inside the dead-zone -> zero.
    g_pad.axes[gpinput::AxisLeftX] = 0.1f;
    g_pad.axes[gpinput::AxisLeftY] = 0.1f;
    gpinput::api().update(0.016);
    double x = 9.0, y = 9.0;
    gpinput::api().stick(gpinput::StickLeft, &x, &y);
    EXPECT_DOUBLE_EQ(x, 0.0);
    EXPECT_DOUBLE_EQ(y, 0.0);

    // Full deflection on one axis -> magnitude 1 maps to 1 (k = 1/mag).
    g_pad.axes[gpinput::AxisLeftX] = 1.0f;
    g_pad.axes[gpinput::AxisLeftY] = 0.0f;
    gpinput::api().update(0.016);
    gpinput::api().stick(gpinput::StickLeft, &x, &y);
    EXPECT_NEAR(x, 1.0, 1e-6);
    EXPECT_NEAR(y, 0.0, 1e-6);
}

TEST(Gamepad, StickDiagonalNotClipped) {
    setupGp();
    gpinput::api().set_deadzone(0.0);

    // A full diagonal push: both axes at the corner. With a radial zone (and no
    // dead-zone) the output direction is preserved and each component is the raw
    // value — a per-axis clip would not change this case, but the magnitude
    // rescale must not exceed 1 along the diagonal.
    g_pad.axes[gpinput::AxisRightX] = 0.6f;
    g_pad.axes[gpinput::AxisRightY] = 0.8f;  // magnitude exactly 1.0
    gpinput::api().update(0.016);
    double x = 0.0, y = 0.0;
    gpinput::api().stick(gpinput::StickRight, &x, &y);
    EXPECT_NEAR(x, 0.6, 1e-6);
    EXPECT_NEAR(y, 0.8, 1e-6);
    EXPECT_NEAR(std::sqrt(x * x + y * y), 1.0, 1e-6);
}

TEST(Gamepad, AxisBindingDeadzoneAndScale) {
    setupGp();
    gpinput::api().set_deadzone(0.15);
    gpinput::api().bind_axis("turn", gpinput::AxisRightX, 2.0);

    // Inside dead-zone.
    g_pad.axes[gpinput::AxisRightX] = 0.1f;
    gpinput::api().update(0.016);
    EXPECT_DOUBLE_EQ(gpinput::api().axis("turn"), 0.0);

    // Outside: rescaled then scaled by 2. (0.5-0.15)/(1-0.15) = 0.4117647...
    g_pad.axes[gpinput::AxisRightX] = 0.5f;
    gpinput::api().update(0.016);
    double expected = ((0.5 - 0.15) / (1.0 - 0.15)) * 2.0;
    EXPECT_NEAR(gpinput::api().axis("turn"), expected, 1e-5);
}

TEST(Gamepad, TriggerAxisReturnsNormalized) {
    setupGp();
    gpinput::api().bind_axis("brake", gpinput::AxisLeftTrigger, 1.0);

    g_pad.axes[gpinput::AxisLeftTrigger] = -1.0f;  // resting
    gpinput::api().update(0.016);
    EXPECT_NEAR(gpinput::api().axis("brake"), 0.0, 1e-6);

    g_pad.axes[gpinput::AxisLeftTrigger] = 1.0f;   // full
    gpinput::api().update(0.016);
    EXPECT_NEAR(gpinput::api().axis("brake"), 1.0, 1e-6);
}

TEST(Gamepad, RebindButton) {
    setupGp();
    gpinput::api().bind_button("jump", 0);
    gpinput::api().bind_button("jump", 1);  // rebind to button 1

    g_pad.buttons[0] = 1;
    gpinput::api().update(0.016);
    EXPECT_FALSE(gpinput::api().held("jump"));

    g_pad.buttons[0] = 0;
    g_pad.buttons[1] = 1;
    gpinput::api().update(0.016);
    EXPECT_TRUE(gpinput::api().held("jump"));
}
