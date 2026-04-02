#pragma once

#include <string>
#include <vector>

#include "molterm/tui/Window.h"

namespace molterm {

class CommandLine {
public:
    void render(Window& win);

    // Render recent history overlay above command line (in viewport bottom)
    void renderHistoryHint(Window& win);

    // Input handling
    void activate(char prefix = ':');
    void deactivate();
    void insertChar(int ch);
    void backspace();
    void deleteWord();
    void clearInput();

    // History
    void historyPrev();
    void historyNext();

    // State
    bool active() const { return active_; }
    std::string input() const { return input_; }
    char prefix() const { return prefix_; }

    // Add to history after command execution
    void pushHistory(const std::string& cmd);

    // Messages (shown when command line is inactive)
    void setMessage(const std::string& msg);
    void clearMessage();

    const std::vector<std::string>& history() const { return history_; }
 
private:
    bool active_ = false;
    char prefix_ = ':';
    std::string input_;
    std::string message_;
    int cursorPos_ = 0;

    std::vector<std::string> history_;
    int historyIdx_ = -1;
    std::string savedInput_;  // saved input when browsing history
};

} // namespace molterm
