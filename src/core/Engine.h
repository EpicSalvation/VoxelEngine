#ifndef ENGINE_H
#define ENGINE_H

#include <thread>
#include <atomic>
#include <chrono>

class Engine
{
public:
    Engine();
    ~Engine();
    void start();
    void stop();
    void update(double deltaTime); // Pass delta time to the update method
    void gameLoop();
    bool getIsRunning() { return isRunning; }
    int getCount() const { return count; }
    double getDeltaTime() const { return deltaTime; }            // For logic that needs to work even if the framerate fluctuates
    void setTargetFrameRate(int fps) { desiredFrameRate = fps; } // Set desired frame rate
    int getTargetFrameRate() const { return desiredFrameRate; }  // Get desired frame rate

private:
    void runFixedTimeStepUpdates(double &accumulatedTime, const double fixedTimeStep);
    void runDeltaTimeUpdates(double deltaTime);
    std::atomic<bool> isRunning = false;
    std::thread gameLoopThread;
    int desiredFrameRate = 60;
    double fixedTimeStep = 0.0; // Fixed time step (based on desired frame rate)
    double lastFrameTime = 0.0; // Store the last frame time
    int count = 0;

    double deltaTime = 0.0; // Store the most recent delta time
    using Clock = std::chrono::high_resolution_clock;
    Clock::time_point previousTime; // Track the previous frame's time
};

#endif // ENGINE_H