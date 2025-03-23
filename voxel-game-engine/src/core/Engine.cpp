#include "Engine.h"
#include "PluginManager.h"
#include <iostream>

Engine::Engine() : isRunning(false) {}

void Engine::start()
{
    isRunning = true;
    std::cout << "Engine started." << std::endl;
    gameLoop();
}

void Engine::stop()
{
    isRunning = false;
    std::cout << "Engine stopped." << std::endl;
}

void Engine::update()
{
    // Update game logic here
}

void Engine::gameLoop()
{
    while (isRunning)
    {
        update();
        // Render and other game loop tasks
    }
}