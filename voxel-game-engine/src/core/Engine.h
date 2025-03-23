#ifndef ENGINE_H
#define ENGINE_H

class Engine {
public:
    void start();
    void stop();
    void update();
    void gameLoop();
    bool getIsRunning() { return isRunning; }
    
private:
    bool isRunning = false;

};

#endif // ENGINE_H