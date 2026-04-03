#include "molterm/config/ConfigParser.h"

#include <toml++/toml.hpp>
#include <ncurses.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace molterm {

// ── Key notation parsing ────────────────────────────────────────────────────

int parseKeyNotation(const std::string& notation) {
    if (notation.empty()) return -1;

    // Special key names: <CR>, <Space>, <Tab>, <BS>, <Esc>, <Up>, <Down>, etc.
    if (notation.size() > 2 && notation.front() == '<' && notation.back() == '>') {
        std::string inner = notation.substr(1, notation.size() - 2);

        // Ctrl modifier: <C-x>
        if (inner.size() == 3 && inner[0] == 'C' && inner[1] == '-') {
            char c = inner[2];
            if (c >= 'a' && c <= 'z') return c - 'a' + 1;
            if (c >= 'A' && c <= 'Z') return c - 'A' + 1;
            return -1;
        }

        // Shift modifier: <S-Tab> etc.
        if (inner.size() > 2 && inner[0] == 'S' && inner[1] == '-') {
            std::string key = inner.substr(2);
            if (key == "Tab") return KEY_BTAB;
            // Shift + letter = uppercase
            if (key.size() == 1 && key[0] >= 'a' && key[0] <= 'z')
                return key[0] - 32;
            return -1;
        }

        // Named keys
        if (inner == "CR" || inner == "Enter") return '\n';
        if (inner == "Space") return ' ';
        if (inner == "Tab") return '\t';
        if (inner == "BS" || inner == "Backspace") return KEY_BACKSPACE;
        if (inner == "Esc" || inner == "Escape") return 27;
        if (inner == "Up") return KEY_UP;
        if (inner == "Down") return KEY_DOWN;
        if (inner == "Left") return KEY_LEFT;
        if (inner == "Right") return KEY_RIGHT;

        return -1;
    }

    // Single character
    if (notation.size() == 1) return notation[0];

    return -1;
}

std::vector<int> parseKeySequence(const std::string& seq) {
    std::vector<int> result;

    size_t i = 0;
    while (i < seq.size()) {
        if (seq[i] == '<') {
            // Find closing >
            size_t end = seq.find('>', i);
            if (end != std::string::npos) {
                std::string token = seq.substr(i, end - i + 1);
                int key = parseKeyNotation(token);
                if (key >= 0) result.push_back(key);
                i = end + 1;
                continue;
            }
        }
        // Plain character
        result.push_back(seq[i]);
        ++i;
    }

    return result;
}

// ── Config directory ────────────────────────────────────────────────────────

std::string ConfigParser::configDir() {
    const char* home = std::getenv("HOME");
    if (!home) home = ".";
    return std::string(home) + "/.molterm";
}

void ConfigParser::ensureConfigDir() {
    fs::create_directories(configDir());
}

// ── Config loading ──────────────────────────────────────────────────────────

Config ConfigParser::loadConfig() {
    Config cfg;
    std::string path = configDir() + "/config.toml";
    if (!fs::exists(path)) return cfg;

    try {
        auto tbl = toml::parse_file(path);

        if (auto v = tbl["renderer"].value<std::string>())
            cfg.defaultRenderer = *v;
        if (auto v = tbl["backbone_thickness"].value<double>())
            cfg.backboneThickness = static_cast<float>(*v);
        if (auto v = tbl["wireframe_thickness"].value<double>())
            cfg.wireframeThickness = static_cast<float>(*v);
        if (auto v = tbl["ball_radius"].value<int64_t>())
            cfg.ballRadius = static_cast<int>(*v);
        if (auto v = tbl["color_scheme"].value<std::string>())
            cfg.defaultColorScheme = *v;
        if (auto v = tbl["panel"].value<bool>())
            cfg.showPanel = *v;
    } catch (const toml::parse_error& e) {
        std::cerr << "molterm: error in " << path << ": " << e.what() << "\n";
    } catch (...) {}

    return cfg;
}

// ── Keymap loading ──────────────────────────────────────────────────────────

std::unordered_map<std::string, std::vector<ConfigParser::KeymapEntry>>
ConfigParser::loadKeymap() {
    std::unordered_map<std::string, std::vector<KeymapEntry>> result;
    std::string path = configDir() + "/keymap.toml";
    if (!fs::exists(path)) return result;

    try {
        auto tbl = toml::parse_file(path);

        for (const auto& modeName : {"normal", "command", "visual", "search"}) {
            if (auto modeTable = tbl[modeName].as_table()) {
                auto& entries = result[modeName];
                for (const auto& [key, val] : *modeTable) {
                    if (auto actionStr = val.value<std::string>()) {
                        auto keys = parseKeySequence(std::string(key.str()));
                        if (!keys.empty()) {
                            entries.push_back({std::move(keys), *actionStr});
                        }
                    }
                }
            }
        }
    } catch (const toml::parse_error& e) {
        std::cerr << "molterm: error in " << path << ": " << e.what() << "\n";
    } catch (...) {}

    return result;
}

// ── Colors loading ──────────────────────────────────────────────────────────

ConfigParser::ColorSchemeConfig ConfigParser::loadColors() {
    ColorSchemeConfig cfg;
    std::string path = configDir() + "/colors.toml";
    if (!fs::exists(path)) return cfg;

    try {
        auto tbl = toml::parse_file(path);

        // [schemes.element]
        if (auto elemTbl = tbl["schemes"]["element"].as_table()) {
            for (const auto& [key, val] : *elemTbl) {
                if (auto color = val.value<std::string>()) {
                    cfg.elementColors[std::string(key.str())] = *color;
                }
            }
        }

        // [schemes.chain]
        if (auto chainTbl = tbl["schemes"]["chain"].as_table()) {
            if (auto cycle = (*chainTbl)["_cycle"].as_array()) {
                for (const auto& v : *cycle) {
                    if (auto s = v.value<std::string>())
                        cfg.chainCycle.push_back(*s);
                }
            }
        }

        // [schemes.ss]
        if (auto ssTbl = tbl["schemes"]["ss"].as_table()) {
            for (const auto& [key, val] : *ssTbl) {
                if (auto color = val.value<std::string>()) {
                    cfg.ssColors[std::string(key.str())] = *color;
                }
            }
        }

        // [schemes.bfactor]
        if (auto bfTbl = tbl["schemes"]["bfactor"].as_table()) {
            if (auto grad = (*bfTbl)["gradient"].as_array()) {
                for (const auto& v : *grad) {
                    if (auto s = v.value<std::string>())
                        cfg.bfactorGradient.push_back(*s);
                }
            }
            if (auto v = (*bfTbl)["min"].value<double>())
                cfg.bfactorMin = static_cast<float>(*v);
            if (auto v = (*bfTbl)["max"].value<double>())
                cfg.bfactorMax = static_cast<float>(*v);
        }
    } catch (const toml::parse_error& e) {
        std::cerr << "molterm: error in " << path << ": " << e.what() << "\n";
    } catch (...) {}

    return cfg;
}

} // namespace molterm
