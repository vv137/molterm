#include "molterm/app/Application.h"
#include "molterm/io/SessionSaver.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

int main(int argc, char* argv[]) {
    // Handle flags before ncurses init
    bool resume = false;
    bool strict = false;
    std::string scriptPath;
    std::vector<char*> filteredArgv;
    filteredArgv.push_back(argv[0]);

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--version") == 0) {
            std::cout << "molterm " << MOLTERM_VERSION << std::endl;
            return 0;
        }
        if (std::strcmp(argv[i], "-r") == 0 || std::strcmp(argv[i], "--resume") == 0) {
            resume = true;
            filteredArgv.push_back(argv[i]);
            continue;
        }
        if (std::strcmp(argv[i], "--strict") == 0) {
            strict = true;
            continue;
        }
        if (std::strcmp(argv[i], "-s") == 0 || std::strcmp(argv[i], "--script") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "molterm: " << argv[i] << " requires a file argument" << std::endl;
                return 2;
            }
            scriptPath = argv[++i];
            continue;
        }
        filteredArgv.push_back(argv[i]);
    }

    int newArgc = static_cast<int>(filteredArgv.size());
    char** newArgv = filteredArgv.data();

    try {
        molterm::Application app;
        app.init(newArgc, newArgv);

        if (resume) {
            std::string msg = molterm::SessionSaver::restoreSession(app);
            app.cmdLine().setMessage(msg);
        }

        if (!scriptPath.empty()) {
            std::ifstream file(scriptPath);
            if (!file) {
                endwin();
                std::cerr << "molterm: cannot open script: " << scriptPath << std::endl;
                return 1;
            }
            molterm::Application::ScriptRunResult r = app.runScriptStream(file, strict);
            if (strict && r.stopped) {
                endwin();
                std::cerr << "molterm: script error at line: " << r.failLine
                          << "\n  " << r.firstFail << std::endl;
                return 1;
            }
            std::string lastMsg = r.lastMsg;
            if (r.failures > 0) {
                lastMsg = "Script: " + std::to_string(r.failures) + " command(s) failed";
            }
            if (!lastMsg.empty()) app.cmdLine().setMessage(lastMsg);
        }

        return app.run();
    } catch (const std::exception& e) {
        endwin();
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}
