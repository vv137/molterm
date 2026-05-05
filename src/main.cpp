#include "molterm/app/Application.h"
#include "molterm/io/SessionSaver.h"
#include "molterm/tui/Screen.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <vector>

int main(int argc, char* argv[]) {
    // Handle flags before ncurses init
    bool resume = false;
    bool strict = false;
    std::string scriptPath;
    int forceTui = 0;  // 0 = auto, +1 = --tui, -1 = --no-tui
    std::vector<char*> filteredArgv;
    filteredArgv.push_back(argv[0]);

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::cout <<
                "Usage: molterm [OPTIONS] [FILE...]\n"
                "\n"
                "Terminal-based molecular viewer with VIM-like modal interface.\n"
                "\n"
                "Arguments:\n"
                "  FILE...                 Structure file(s) to load (.pdb, .cif, .cif.gz, ...)\n"
                "\n"
                "Options:\n"
                "  -h, --help              Show this help and exit\n"
                "  -v, --version           Print version and exit\n"
                "  -r, --resume            Restore the last auto-saved session\n"
                "  -s, --script FILE       Run a command script after load (use '-' for stdin)\n"
                "      --strict            With --script: abort on first error (exit 1)\n"
                "      --no-tui            Run without the terminal UI (auto-on for non-TTY scripts)\n"
                "      --tui               Force the terminal UI even when auto-detect would skip it\n"
                "\n"
                "In-app help:\n"
                "  ?                       Keybinding cheat sheet overlay (Normal mode)\n"
                "  :help                   Command index overlay, grouped by category\n"
                "  :help <cmd>             Per-command overlay (usage, description, examples)\n"
                "\n"
                "Examples:\n"
                "  molterm protein.pdb\n"
                "  molterm --resume\n"
                "  molterm --script demo.mt 1bna.cif        # load + run script\n"
                "  molterm --script render.mt --no-tui      # silent batch render\n";
            return 0;
        }
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
        if (std::strcmp(argv[i], "--no-tui") == 0) { forceTui = -1; continue; }
        if (std::strcmp(argv[i], "--tui") == 0)    { forceTui = +1; continue; }
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

    // Decide headless mode BEFORE constructing Application (Screen ctor reads
    // MOLTERM_HEADLESS to skip initscr). Auto-headless when running a script
    // without an interactive stdout, unless --tui forces the UI on.
    bool headless = false;
    if (forceTui == -1 || scriptPath == "-") {
        // --no-tui, or stdin is the script (TUI can't share stdin with ncurses).
        headless = true;
    } else if (forceTui == 0 && !scriptPath.empty() && !isatty(STDOUT_FILENO)) {
        headless = true;
    }
    if (headless) setenv("MOLTERM_HEADLESS", "1", 1);

    int newArgc = static_cast<int>(filteredArgv.size());
    char** newArgv = filteredArgv.data();

    auto leaveCurses = []() { if (!molterm::isHeadless()) endwin(); };

    try {
        molterm::Application app;
        app.init(newArgc, newArgv);

        if (resume) {
            std::string msg = molterm::SessionSaver::restoreSession(app);
            app.cmdLine().setMessage(msg);
            if (headless && !msg.empty()) std::cout << msg << "\n";
        }

        if (!scriptPath.empty()) {
            std::ifstream file;
            std::istream* in = &std::cin;
            if (scriptPath != "-") {
                file.open(scriptPath);
                if (!file) {
                    leaveCurses();
                    std::cerr << "molterm: cannot open script: " << scriptPath << std::endl;
                    return 1;
                }
                in = &file;
            }
            molterm::Application::ScriptRunResult r = app.runScriptStream(*in, strict);
            if (strict && r.stopped) {
                leaveCurses();
                std::cerr << "molterm: script error at line: " << r.failLine
                          << "\n  " << r.firstFail << std::endl;
                return 1;
            }
            std::string lastMsg = r.lastMsg;
            if (r.failures > 0) {
                lastMsg = "Script: " + std::to_string(r.failures) + " command(s) failed";
            }
            if (!lastMsg.empty()) {
                app.cmdLine().setMessage(lastMsg);
                if (headless) std::cout << lastMsg << "\n";
            }
        }

        return app.run();
    } catch (const std::exception& e) {
        leaveCurses();
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}
