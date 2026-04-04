#include "molterm/tui/SeqBar.h"
#include "molterm/core/Selection.h"
#include "molterm/render/ColorMapper.h"

#include <algorithm>
#include <set>
#include <unordered_map>

namespace molterm {

// 3-letter → 1-letter amino acid / nucleotide code
char SeqBar::toOneLetter(const std::string& resName) {
    static const std::unordered_map<std::string, char> table = {
        {"ALA",'A'},{"VAL",'V'},{"LEU",'L'},{"ILE",'I'},{"PRO",'P'},
        {"PHE",'F'},{"TRP",'W'},{"MET",'M'},{"GLY",'G'},{"SER",'S'},
        {"THR",'T'},{"CYS",'C'},{"TYR",'Y'},{"ASN",'N'},{"GLN",'Q'},
        {"ASP",'D'},{"GLU",'E'},{"LYS",'K'},{"ARG",'R'},{"HIS",'H'},
        // DNA
        {"DA",'a'},{"DT",'t'},{"DG",'g'},{"DC",'c'},
        // RNA
        {"A",'a'},{"U",'u'},{"G",'g'},{"C",'c'},
        // Modified / common
        {"MSE",'M'},{"SEC",'U'},{"PYL",'O'},
    };
    auto it = table.find(resName);
    return (it != table.end()) ? it->second : '?';
}

void SeqBar::update(const MolObject& mol, const std::string& activeChainId) {
    residues_.clear();
    chainIds_.clear();

    const auto& atoms = mol.atoms();
    std::set<std::string> seenChains;
    int lastResSeq = -9999;
    std::string lastChain;

    // Collect unique residues for each chain
    for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
        const auto& a = atoms[i];
        if (seenChains.find(a.chainId) == seenChains.end()) {
            seenChains.insert(a.chainId);
            chainIds_.push_back(a.chainId);
        }
    }

    // Set active chain
    if (!activeChainId.empty()) {
        activeChain_ = activeChainId;
    } else if (activeChain_.empty() && !chainIds_.empty()) {
        activeChain_ = chainIds_[0];
    }

    // Extract residues for active chain
    lastResSeq = -9999;
    for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
        const auto& a = atoms[i];
        if (a.chainId != activeChain_) continue;
        if (a.resSeq == lastResSeq) continue;  // dedup
        lastResSeq = a.resSeq;

        Residue r;
        r.resSeq = a.resSeq;
        r.letter = toOneLetter(a.resName);
        r.ss = a.ssType;
        r.firstAtomIdx = i;
        residues_.push_back(r);
    }
}

void SeqBar::render(Window& win, int focusResi, const Selection* sele,
                    ColorScheme scheme, bool wrap) {
    win.erase();
    int w = win.width();
    int h = win.height();
    if (residues_.empty() || w < 5) return;

    // Prefix: "A:" (chain label)
    std::string prefix = activeChain_ + ":";
    int prefixLen = static_cast<int>(prefix.size());

    // Build full sequence string and colors
    int seqLen = static_cast<int>(residues_.size());

    if (!wrap) {
        // 1-row mode: center on focusResi
        int focusIdx = -1;
        if (focusResi >= 0) {
            for (int i = 0; i < seqLen; ++i) {
                if (residues_[i].resSeq == focusResi) { focusIdx = i; break; }
            }
        }

        int visibleW = w - prefixLen - 8;  // room for [pos/total]
        if (visibleW < 10) visibleW = w - prefixLen;

        // Compute scroll offset to center focusIdx
        if (focusIdx >= 0) {
            scrollOffset_ = focusIdx - visibleW / 2;
        }
        scrollOffset_ = std::max(0, std::min(scrollOffset_, seqLen - visibleW));
        if (scrollOffset_ < 0) scrollOffset_ = 0;

        // Draw prefix
        win.printColored(0, 0, prefix, kColorPanelHeader);

        // Draw sequence
        for (int i = 0; i < visibleW && scrollOffset_ + i < seqLen; ++i) {
            int ri = scrollOffset_ + i;
            const auto& r = residues_[ri];
            int color = kColorOther;

            // Color by scheme
            switch (scheme) {
                case ColorScheme::SecondaryStructure:
                    color = ColorMapper::colorForSS(r.ss);
                    break;
                case ColorScheme::Chain:
                    color = ColorMapper::colorForChain(activeChain_);
                    break;
                case ColorScheme::ResType:
                    color = ColorMapper::colorForResType(
                        std::string(1, r.letter));  // won't match 3-letter, fallback
                    break;
                default:
                    color = kColorOther;
                    break;
            }

            // Highlight if selected
            bool selected = false;
            if (sele) {
                selected = sele->has(r.firstAtomIdx);
            }

            int x = prefixLen + i;
            if (selected) {
                win.setAttr(A_REVERSE);
                win.addCharColored(0, x, r.letter, color);
                win.unsetAttr(A_REVERSE);
            } else {
                win.addCharColored(0, x, r.letter, color);
            }

            // Cursor marker for focused residue
            if (ri == focusIdx && h > 1) {
                win.addCharColored(1, x, '^', kColorYellow);
            }
        }

        // Position indicator
        std::string pos = " [" + std::to_string(scrollOffset_ + 1) + "/" +
                         std::to_string(seqLen) + "]";
        int posX = w - static_cast<int>(pos.size());
        if (posX > prefixLen + visibleW)
            win.printColored(0, posX, pos, kColorStatusBar);

    } else {
        // Wrap mode: fill rows left-to-right
        int cols = w - prefixLen;
        if (cols < 5) cols = w;
        int row = 0;
        for (int i = 0; i < seqLen && row < h; ++i) {
            int col = i % cols;
            if (col == 0 && i > 0) ++row;
            if (row >= h) break;

            // Draw prefix on each row start
            if (col == 0) {
                std::string rowPrefix = (i == 0) ? prefix :
                    std::string(prefixLen, ' ');
                win.printColored(row, 0, rowPrefix, kColorPanelHeader);
            }

            const auto& r = residues_[i];
            int color = kColorOther;
            switch (scheme) {
                case ColorScheme::SecondaryStructure:
                    color = ColorMapper::colorForSS(r.ss); break;
                case ColorScheme::Chain:
                    color = ColorMapper::colorForChain(activeChain_); break;
                default: color = kColorOther; break;
            }

            bool selected = sele && sele->has(r.firstAtomIdx);
            int x = prefixLen + col;
            if (selected) {
                win.setAttr(A_REVERSE);
                win.addCharColored(row, x, r.letter, color);
                win.unsetAttr(A_REVERSE);
            } else {
                win.addCharColored(row, x, r.letter, color);
            }
        }
    }
}

int SeqBar::resSeqAtColumn(int col, bool wrap, int winWidth) const {
    std::string prefix = activeChain_ + ":";
    int prefixLen = static_cast<int>(prefix.size());
    int seqCol = col - prefixLen;
    if (seqCol < 0) return -1;

    int idx;
    if (!wrap) {
        idx = scrollOffset_ + seqCol;
    } else {
        // TODO: handle row for multi-line wrap
        idx = seqCol;
    }

    if (idx >= 0 && idx < static_cast<int>(residues_.size()))
        return residues_[idx].resSeq;
    return -1;
}

int SeqBar::wrapRows(int winWidth) const {
    std::string prefix = activeChain_ + ":";
    int prefixLen = static_cast<int>(prefix.size());
    int cols = winWidth - prefixLen;
    if (cols < 5) cols = winWidth;
    int seqLen = static_cast<int>(residues_.size());
    return (seqLen + cols - 1) / cols;
}

void SeqBar::nextChain() {
    if (chainIds_.empty()) return;
    for (size_t i = 0; i < chainIds_.size(); ++i) {
        if (chainIds_[i] == activeChain_) {
            activeChain_ = chainIds_[(i + 1) % chainIds_.size()];
            return;
        }
    }
    activeChain_ = chainIds_[0];
}

void SeqBar::prevChain() {
    if (chainIds_.empty()) return;
    for (size_t i = 0; i < chainIds_.size(); ++i) {
        if (chainIds_[i] == activeChain_) {
            activeChain_ = chainIds_[(i + chainIds_.size() - 1) % chainIds_.size()];
            return;
        }
    }
    activeChain_ = chainIds_[0];
}

} // namespace molterm
