#include "molterm/app/Application.h"

#include <iostream>

int main(int argc, char* argv[]) {
    try {
        molterm::Application app;
        app.init(argc, argv);
        return app.run();
    } catch (const std::exception& e) {
        // Make sure ncurses is cleaned up before printing
        endwin();
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}
