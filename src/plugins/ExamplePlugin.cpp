#include "ExamplePlugin.h"
#include <iostream>

void ExamplePlugin::initialize() {
    std::cout << "ExamplePlugin initialized." << std::endl;
}

void ExamplePlugin::update() {
    std::cout << "ExamplePlugin update called." << std::endl;
}