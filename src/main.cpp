#include "molterm/app/Application.h"
#include "molterm/io/SessionSaver.h"

#include <cstring>
#include <iostream>

int main(int argc, char* argv[]) {
    // Handle flags before ncurses init
    bool resume = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--version") == 0) {
            std::cout << "molterm " << MOLTERM_VERSION << std::endl;
            return 0;
        }
        if (std::strcmp(argv[i], "-r") == 0 || std::strcmp(argv[i], "--resume") == 0) {
            resume = true;
        }
    }

    try {
        molterm::Application app;
        app.init(argc, argv);

        if (resume) {
            std::string msg = molterm::SessionSaver::restoreSession(app);
            app.cmdLine().setMessage(msg);
        }

        return app.run();
    } catch (const std::exception& e) {
        endwin();
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}
