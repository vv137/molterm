#include "molterm/app/Application.h"

#include <cstring>
#include <iostream>

int main(int argc, char* argv[]) {
    // Handle -v / --version before ncurses init
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--version") == 0) {
            std::cout << "molterm " << MOLTERM_VERSION << std::endl;
            return 0;
        }
    }

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
