#include "molterm/tui/CommandLine.h"
#include "molterm/render/ColorMapper.h"

#include <algorithm>

namespace molterm {

void CommandLine::render(Window& win) {
    win.erase();
    if (active_) {
        std::string display = std::string(1, prefix_) + input_;
        win.print(0, 0, display);
        curs_set(1);
        wmove(win.raw(), 0, cursorPos_ + 1);
    } else if (!message_.empty()) {
        win.print(0, 0, message_);
    }
    win.refresh();
}

void CommandLine::renderHistoryHint(Window& win) {
    if (!active_ || !input_.empty()) return;
    if (history_.empty()) return;

    int maxLines = std::min(static_cast<int>(history_.size()), 5);
    int winH = win.height();
    int startY = winH - maxLines;
    if (startY < 0) startY = 0;

    // Draw from bottom up: most recent at the bottom
    for (int i = 0; i < maxLines; ++i) {
        int histIdx = static_cast<int>(history_.size()) - maxLines + i;
        if (histIdx < 0) continue;

        int y = startY + i;
        if (y >= winH) break;

        // Dim appearance: line number + command
        std::string line = " " + std::to_string(histIdx + 1) + "  :" + history_[histIdx];
        // Truncate
        int maxLen = win.width() - 1;
        if (static_cast<int>(line.size()) > maxLen)
            line = line.substr(0, maxLen);

        win.printColored(y, 0, line, kColorGray);
    }
}

void CommandLine::activate(char prefix) {
    active_ = true;
    prefix_ = prefix;
    input_.clear();
    cursorPos_ = 0;
    historyIdx_ = -1;
    savedInput_.clear();
    message_.clear();
}

void CommandLine::deactivate() {
    active_ = false;
    input_.clear();
    cursorPos_ = 0;
    curs_set(0);
}

void CommandLine::insertChar(int ch) {
    if (ch >= 32 && ch < 127) {
        input_.insert(cursorPos_, 1, static_cast<char>(ch));
        ++cursorPos_;
    }
}

void CommandLine::backspace() {
    if (cursorPos_ > 0) {
        input_.erase(cursorPos_ - 1, 1);
        --cursorPos_;
    }
}

void CommandLine::deleteForward() {
    if (cursorPos_ < static_cast<int>(input_.size())) {
        input_.erase(cursorPos_, 1);
    }
}

void CommandLine::cursorLeft() {
    if (cursorPos_ > 0) --cursorPos_;
}

void CommandLine::cursorRight() {
    if (cursorPos_ < static_cast<int>(input_.size())) ++cursorPos_;
}

void CommandLine::cursorHome() {
    cursorPos_ = 0;
}

void CommandLine::cursorEnd() {
    cursorPos_ = static_cast<int>(input_.size());
}

void CommandLine::deleteWord() {
    while (cursorPos_ > 0 && input_[cursorPos_ - 1] == ' ') {
        input_.erase(cursorPos_ - 1, 1);
        --cursorPos_;
    }
    while (cursorPos_ > 0 && input_[cursorPos_ - 1] != ' ') {
        input_.erase(cursorPos_ - 1, 1);
        --cursorPos_;
    }
}

void CommandLine::clearInput() {
    input_.clear();
    cursorPos_ = 0;
}

void CommandLine::pushHistory(const std::string& cmd) {
    if (cmd.empty()) return;
    // Don't duplicate consecutive identical commands
    if (!history_.empty() && history_.back() == cmd) return;
    history_.push_back(cmd);
    // Limit history size
    if (history_.size() > 200) {
        history_.erase(history_.begin());
    }
}

void CommandLine::historyPrev() {
    if (history_.empty()) return;
    if (historyIdx_ < 0) {
        savedInput_ = input_;
        historyIdx_ = static_cast<int>(history_.size()) - 1;
    } else if (historyIdx_ > 0) {
        --historyIdx_;
    }
    input_ = history_[historyIdx_];
    cursorPos_ = static_cast<int>(input_.size());
}

void CommandLine::historyNext() {
    if (historyIdx_ < 0) return;
    if (historyIdx_ < static_cast<int>(history_.size()) - 1) {
        ++historyIdx_;
        input_ = history_[historyIdx_];
    } else {
        historyIdx_ = -1;
        input_ = savedInput_;
    }
    cursorPos_ = static_cast<int>(input_.size());
}

void CommandLine::setMessage(const std::string& msg) {
    message_ = msg;
}

void CommandLine::clearMessage() {
    message_.clear();
}

} // namespace molterm
