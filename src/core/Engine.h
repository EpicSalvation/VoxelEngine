#pragma once

#include <atomic>
#include <chrono>
#include <thread>

class Engine {
public:
    Engine();
    ~Engine();

    void start();
    void stop();
    void update(double deltaTime);

    bool   getIsRunning()      const { return isRunning; }
    double getDeltaTime()      const { return deltaTime; }
    void   setTargetFrameRate(int fps) { desiredFrameRate = fps; }
    int    getTargetFrameRate() const { return desiredFrameRate; }

private:
    void gameLoop();

    std::atomic<bool>  isRunning      = false;
    std::thread        gameLoopThread;
    int                desiredFrameRate = 60;
    double             fixedTimeStep    = 0.0;
    double             deltaTime        = 0.0;

    using Clock = std::chrono::high_resolution_clock;
    Clock::time_point previousTime;
};