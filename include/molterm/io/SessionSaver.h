#pragma once

#include <string>

namespace molterm {

class Application;

class SessionSaver {
public:
    // Save current session to ~/.molterm/autosave.toml
    static bool saveSession(const Application& app);

    // Restore session from ~/.molterm/autosave.toml
    // Returns message describing what was loaded
    static std::string restoreSession(Application& app);

    // Path to autosave file
    static std::string sessionPath();
};

} // namespace molterm
