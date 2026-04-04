#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "molterm/core/MolObject.h"
#include "molterm/tui/Window.h"

namespace molterm {

class Selection;

class SeqBar {
public:
    // Extract sequence from a MolObject
    void update(const MolObject& mol, const std::string& activeChainId = "");

    // Render into the window. focusResi = residue to center on (-1 = no focus).
    // selectedAtoms = sele indices for highlight.
    void render(Window& win, int focusResi, const Selection* sele,
                ColorScheme scheme, bool wrap);

    // Mouse click → returns resSeq at terminal column, or -1. Sets chainId if found.
    int resSeqAtColumn(int col, bool wrap, int winWidth, std::string* outChain = nullptr) const;

    // How many rows needed for wrap mode
    int wrapRows(int winWidth) const;

    // Chain list
    const std::vector<std::string>& chains() const { return chainIds_; }
    void setActiveChain(const std::string& c) { activeChain_ = c; }
    const std::string& activeChain() const { return activeChain_; }
    void nextChain();
    void prevChain();
    void scrollToChain(const std::string& chainId);

    // 3-letter → 1-letter
    static char toOneLetter(const std::string& resName);

private:
public:
    struct Residue {
        int resSeq;        // kSeparator or kChainLabel for markers
        char letter;       // 1-letter code
        SSType ss;
        int reprAtomIdx;   // CA (protein) or C1' (nucleic) for picking
        int firstAtomIdx;  // first atom in residue (for range checking)
        int lastAtomIdx;   // last atom in residue (for selection check)
        std::string chainId;
    };
private:

    static constexpr int kSeparator = -1;
    static constexpr int kChainLabel = -2;

    std::vector<Residue> residues_;
    std::vector<std::string> chainIds_;
    std::string activeChain_;
    int scrollOffset_ = 0;
    int lastFocusResi_ = -1;   // track focus changes to avoid overriding manual scroll
    std::string lastObjName_;
    int cachedResCount_ = 0;
};

} // namespace molterm
