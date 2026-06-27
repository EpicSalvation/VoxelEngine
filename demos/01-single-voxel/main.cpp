#include "core/Engine.h"
#include "core/LayerConfig.h"
#include "core/Logger.h"
#include "core/PluginManager.h"
#include "platform/Window.h"
#include "plugins/ExamplePlugin.h"
#include "renderer/BgfxRenderer.h"
#include "world/Voxel.h"
#include "world/World.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <string>

namespace { constexpr char kLogCat[] = "demo01"; }

int main() {
    // M1: validate layer configuration at startup — bad config is a hard error.
    try {
        LayerConfig layerConfig = LayerConfig::loadFromString(R"(
layers:
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
)");
        Log::info(kLogCat, (std::string("Layer config OK. ")
                            + std::to_string(layerConfig.layers().size())
                            + " layer(s) defined.").c_str());
    } catch (const std::exception& e) {
        Log::error(kLogCat, (std::string("Fatal: layer config error: ") + e.what()).c_str());
        return 1;
    }

    // M4: load plugins (scan directory, then wire in the example plugin directly).
    PluginManager pluginManager;
    pluginManager.loadPluginsFromDirectory("plugins");
    pluginManager.wireInPlugin(voxel_plugin_init);
    Log::info(kLogCat, (std::string("Registered materials: ")
                        + std::to_string(pluginManager.materials().size())).c_str());
    Log::info(kLogCat, (std::string("Registered layer generators: ")
                        + std::to_string(pluginManager.layerGenerators().size())).c_str());

    Engine engine;
    engine.start();

    // M2: open window and initialise bgfx renderer.
    platform::Window window(800, 600, "VoxelEngine — M2 Demo");

    BgfxRenderer renderer;
    int fbW, fbH;
    window.framebufferSize(fbW, fbH);
    renderer.initialize(window.nativeHandles(),
                        static_cast<uint32_t>(fbW),
                        static_cast<uint32_t>(fbH));

    // M2 demo: one voxel at world origin with a recognisable palette colour.
    World world(1, 1, 1);
    {
        Voxel v;
        v.material.palette_index = 2;  // grass green
        v.material.density       = 1.0f;
        world.setVoxel(0, 0, 0, v);
    }

    // Camera state.
    float      pitch = 0.0f, yaw = 0.0f;
    WorldCoord camPos(0.0, 2.0, 4.0);
    bool       freeCam    = false;
    bool       prevKeyF   = false;
    double     lastMouseX = 0.0, lastMouseY = 0.0;
    bool       firstMouse = true;

    // Auto-orbit parameters.
    float orbitAngle  = 0.0f;
    const float kOrbitRadius = 4.0f;
    const float kOrbitHeight = 2.0f;
    const float kOrbitSpeed  = 0.5f;  // radians per second

    // Free-camera parameters.
    const float kMoveSpeed   = 3.0f;
    const float kMouseSens   = 0.002f;

    GLFWwindow* glfwWin = window.glfwHandle();

    auto prevTime = std::chrono::high_resolution_clock::now();

    Log::info(kLogCat, "Rendering. Press F to toggle free-camera / auto-orbit.");
    Log::info(kLogCat, "Free-cam controls: WASD to move, Space/Shift for up/down, mouse to look.");

    while (!window.shouldClose()) {
        window.pollEvents();

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - prevTime).count();
        prevTime = now;
        if (dt > 0.1f) dt = 0.1f;  // guard against hitches on first frame / focus regain

        // Toggle free-camera with F (edge-triggered).
        bool curKeyF = (glfwGetKey(glfwWin, GLFW_KEY_F) == GLFW_PRESS);
        if (curKeyF && !prevKeyF) {
            freeCam = !freeCam;
            glfwSetInputMode(glfwWin, GLFW_CURSOR,
                             freeCam ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
            firstMouse = true;
            Log::info(kLogCat, freeCam ? "Free-camera mode." : "Auto-orbit mode.");
        }
        prevKeyF = curKeyF;

        // ESC quits.
        if (glfwGetKey(glfwWin, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            break;

        if (freeCam) {
            // Mouse look.
            double mx, my;
            glfwGetCursorPos(glfwWin, &mx, &my);
            if (!firstMouse) {
                yaw   += static_cast<float>(mx - lastMouseX) * kMouseSens;
                pitch -= static_cast<float>(my - lastMouseY) * kMouseSens;
                // Clamp pitch to just under ±90° to avoid gimbal flip.
                if (pitch >  1.55f) pitch =  1.55f;
                if (pitch < -1.55f) pitch = -1.55f;
            }
            lastMouseX = mx;
            lastMouseY = my;
            firstMouse = false;

            // WASD + Space/Shift movement — camera-relative horizontal, world-up vertical.
            float sp = std::sin(pitch), cp = std::cos(pitch);
            float sy = std::sin(yaw),   cy = std::cos(yaw);

            // Forward and right vectors in world space.
            glm::dvec3 fwd  {static_cast<double>(cp * sy),
                             static_cast<double>(sp),
                             static_cast<double>(cp * cy)};
            glm::dvec3 right{static_cast<double>(cy), 0.0, static_cast<double>(-sy)};

            glm::dvec3 delta{0.0, 0.0, 0.0};
            double step = static_cast<double>(kMoveSpeed * dt);
            if (glfwGetKey(glfwWin, GLFW_KEY_W)           == GLFW_PRESS) delta += fwd   * step;
            if (glfwGetKey(glfwWin, GLFW_KEY_S)           == GLFW_PRESS) delta -= fwd   * step;
            if (glfwGetKey(glfwWin, GLFW_KEY_A)           == GLFW_PRESS) delta -= right * step;
            if (glfwGetKey(glfwWin, GLFW_KEY_D)           == GLFW_PRESS) delta += right * step;
            if (glfwGetKey(glfwWin, GLFW_KEY_SPACE)       == GLFW_PRESS) delta.y += step;
            if (glfwGetKey(glfwWin, GLFW_KEY_LEFT_SHIFT)  == GLFW_PRESS) delta.y -= step;

            camPos = WorldCoord(camPos.value + delta);
        } else {
            // Auto-orbit: camera circles the origin at fixed radius and height.
            orbitAngle += kOrbitSpeed * dt;

            float cx = kOrbitRadius * std::sin(orbitAngle);
            float cz = kOrbitRadius * std::cos(orbitAngle);
            float cy = kOrbitHeight;
            camPos = WorldCoord(static_cast<double>(cx),
                                static_cast<double>(cy),
                                static_cast<double>(cz));

            // Derive pitch and yaw so the camera always faces the origin.
            float dx        = -cx, dy = -cy, dz = -cz;
            float horizDist = std::sqrt(dx * dx + dz * dz);
            pitch = std::atan2(dy, horizDist);
            yaw   = std::atan2(dx, dz);
        }

        // Handle framebuffer resize.
        int w, h;
        window.framebufferSize(w, h);
        if (w != fbW || h != fbH) {
            fbW = w; fbH = h;
            renderer.setViewport(w, h);
        }

        // Submit world and render.
        renderer.setCameraPosition(camPos);
        renderer.setCameraRotation(pitch, yaw, 0.0f);
        renderer.renderWorld(world);
        renderer.render();
    }

    renderer.shutdown();
    engine.stop();
    return 0;
}
