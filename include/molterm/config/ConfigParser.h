#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace molterm {

// Parsed key notation (e.g., "<C-t>", "gt", "<S-Tab>") to int key codes
int parseKeyNotation(const std::string& notation);
std::vector<int> parseKeySequence(const std::string& seq);

// General config from ~/.molterm/config.toml
struct Config {
    std::string defaultRenderer = "braille";
    float backboneThickness = 1.5f;
    float wireframeThickness = 1.0f;
    int ballRadius = 3;
    std::string defaultColorScheme = "element";
    bool showPanel = false;
};

class ConfigParser {
public:
    // Get the config directory path (~/.molterm/)
    static std::string configDir();

    // Ensure ~/.molterm/ exists
    static void ensureConfigDir();

    // Load general config from ~/.molterm/config.toml
    static Config loadConfig();

    // Load keymap overrides from ~/.molterm/keymap.toml
    // Returns map: mode_name -> [(key_sequence, action_name)]
    struct KeymapEntry {
        std::vector<int> keys;
        std::string actionName;
    };
    static std::unordered_map<std::string, std::vector<KeymapEntry>> loadKeymap();

    // Load custom color schemes from ~/.molterm/colors.toml
    struct ColorEntry {
        std::string element;
        std::string colorName;
    };
    struct ColorSchemeConfig {
        std::unordered_map<std::string, std::string> elementColors;
        std::vector<std::string> chainCycle;
        std::unordered_map<std::string, std::string> ssColors;
        std::vector<std::string> bfactorGradient;
        float bfactorMin = 0.0f;
        float bfactorMax = 100.0f;
    };
    static ColorSchemeConfig loadColors();
};

} // namespace molterm
