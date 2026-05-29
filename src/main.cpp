#include "core/Engine.h"

int main() {
    Engine engine;
    engine.start();
    // Simulate some work in the main thread
    std::this_thread::sleep_for(std::chrono::seconds(5));

    engine.stop();
    return 0;
}