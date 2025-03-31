#include "Engine.h"
#include "PluginManager.h"
#include <iostream>

Engine::Engine() : isRunning(false) {}

Engine::~Engine()
{
    if (gameLoopThread.joinable())
    {
        stop();
        gameLoopThread.join();
    }
}

void Engine::start()
{
    isRunning = true;
    gameLoopThread = std::thread(&Engine::gameLoop, this);
    std::cout << "Engine started." << std::endl;
}

void Engine::stop()
{
    isRunning = false;

    // Wait for the game loop thread to finish
    if (gameLoopThread.joinable())
    {
        gameLoopThread.join();
    }

    std::cout << "Engine stopped." << std::endl;
}

void Engine::update(double deltaTime)
{
    // Update game logic here
    count++;
    std::cout << "Updating game logic..." << std::endl;
}

void Engine::gameLoop() {
    fixedTimeStep = 1.0 / desiredFrameRate; // Fixed time step (e.g., 60 updates per second)
    double accumulatedTime = 0.0;

    while (isRunning) {
        // Calculate delta time
        auto currentTime = Clock::now();
        deltaTime = std::chrono::duration<double>(currentTime - previousTime).count();
        previousTime = currentTime;

        // Accumulate time for fixed time step updates
        accumulatedTime += deltaTime;

        // Process updates in fixed time steps
        if (accumulatedTime >= fixedTimeStep) {
            update(deltaTime); // Pass delta time to the update method
            accumulatedTime -= fixedTimeStep;
        }

        // Optional: Use delta time for rendering or non-critical systems
        // render(deltaTime);

        // Simulate rendering and other tasks (optional sleep to limit frame rate)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}