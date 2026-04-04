#include "molterm/tui/SeqBar.h"
#include "molterm/core/Selection.h"
#include "molterm/render/ColorMapper.h"

#include <algorithm>
#include <set>
#include <unordered_map>

namespace molterm {

char SeqBar::toOneLetter(const std::string& resName) {
    static const std::unordered_map<std::string, char> table = {
        {"ALA",'A'},{"VAL",'V'},{"LEU",'L'},{"ILE",'I'},{"PRO",'P'},
        {"PHE",'F'},{"TRP",'W'},{"MET",'M'},{"GLY",'G'},{"SER",'S'},
        {"THR",'T'},{"CYS",'C'},{"TYR",'Y'},{"ASN",'N'},{"GLN",'Q'},
        {"ASP",'D'},{"GLU",'E'},{"LYS",'K'},{"ARG",'R'},{"HIS",'H'},
        {"DA",'a'},{"DT",'t'},{"DG",'g'},{"DC",'c'},
        {"A",'a'},{"U",'u'},{"G",'g'},{"C",'c'},
        {"MSE",'M'},{"SEC",'U'},{"PYL",'O'},
    };
    auto it = table.find(resName);
    return (it != table.end()) ? it->second : '?';
}

void SeqBar::update(const MolObject& mol, const std::string& /*activeChainId*/) {
    residues_.clear();
    chainIds_.clear();

    const auto& atoms = mol.atoms();
    std::set<std::string> seenChains;
    int lastResSeq = -9999;
    std::string lastChain;

    // Extract all residues from all chains, with chain separator markers
    for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
        const auto& a = atoms[i];

        // New chain?
        if (a.chainId != lastChain) {
            if (seenChains.find(a.chainId) == seenChains.end()) {
                seenChains.insert(a.chainId);
                chainIds_.push_back(a.chainId);

                // Insert separator marker (resSeq=-1 signals chain break)
                if (!residues_.empty()) {
                    residues_.push_back({-1, '|', SSType::Loop, -1, ""});
                }
                // Insert chain label marker
                residues_.push_back({-2, ' ', SSType::Loop, -1, a.chainId});
            }
            lastChain = a.chainId;
            lastResSeq = -9999;
        }

        if (a.resSeq == lastResSeq) continue;
        lastResSeq = a.resSeq;

        Residue r;
        r.resSeq = a.resSeq;
        r.letter = toOneLetter(a.resName);
        r.ss = a.ssType;
        r.firstAtomIdx = i;
        r.chainId = a.chainId;
        residues_.push_back(r);
    }
}

static int colorForResidue(const SeqBar::Residue& r, ColorScheme scheme) {
    switch (scheme) {
        case ColorScheme::SecondaryStructure:
            return ColorMapper::colorForSS(r.ss);
        case ColorScheme::Chain:
            return ColorMapper::colorForChain(r.chainId);
        default:
            return kColorOther;
    }
}

void SeqBar::render(Window& win, int focusResi, const Selection* sele,
                    ColorScheme scheme, bool wrap) {
    win.erase();
    int w = win.width();
    int h = win.height();
    if (residues_.empty() || w < 5) return;

    int seqLen = static_cast<int>(residues_.size());

    if (!wrap) {
        // 1-row mode: auto-scroll to center on focusResi
        int focusIdx = -1;
        if (focusResi >= 0) {
            for (int i = 0; i < seqLen; ++i) {
                if (residues_[i].resSeq == focusResi) { focusIdx = i; break; }
            }
        }

        int visibleW = w - 8;  // room for position indicator
        if (visibleW < 10) visibleW = w;

        if (focusIdx >= 0) {
            scrollOffset_ = focusIdx - visibleW / 2;
        }
        scrollOffset_ = std::max(0, std::min(scrollOffset_, seqLen - visibleW));
        if (scrollOffset_ < 0) scrollOffset_ = 0;

        for (int i = 0; i < visibleW && scrollOffset_ + i < seqLen; ++i) {
            int ri = scrollOffset_ + i;
            const auto& r = residues_[ri];

            // Separator / chain label
            if (r.resSeq == -1) {
                win.addCharColored(0, i, '|', kColorSlate);
                continue;
            }
            if (r.resSeq == -2) {
                // Chain label: "A:"
                std::string lbl = r.chainId + ":";
                win.printColored(0, i, lbl, kColorPanelHeader);
                continue;
            }

            int color = colorForResidue(r, scheme);
            bool selected = sele && r.firstAtomIdx >= 0 && sele->has(r.firstAtomIdx);

            if (selected) {
                win.setAttr(A_REVERSE);
                win.addCharColored(0, i, r.letter, color);
                win.unsetAttr(A_REVERSE);
            } else {
                win.addCharColored(0, i, r.letter, color);
            }
        }

        // Position indicator at right edge
        int totalRes = 0;
        for (const auto& r : residues_) if (r.resSeq >= 0) ++totalRes;
        std::string pos = "[" + std::to_string(totalRes) + "]";
        int posX = w - static_cast<int>(pos.size());
        if (posX > visibleW)
            win.printColored(0, posX, pos, kColorStatusBar);

    } else {
        // Wrap mode: fill rows left-to-right
        int cols = w;
        int row = 0, col = 0;
        for (int i = 0; i < seqLen && row < h; ++i) {
            const auto& r = residues_[i];

            // Chain break: start new line
            if (r.resSeq == -1) {
                if (col > 0) { ++row; col = 0; }
                continue;
            }
            if (r.resSeq == -2) {
                // Chain label at start of new line
                std::string lbl = r.chainId + ":";
                if (row < h) win.printColored(row, 0, lbl, kColorPanelHeader);
                col = static_cast<int>(lbl.size());
                continue;
            }

            if (col >= cols) { ++row; col = 0; }
            if (row >= h) break;

            int color = colorForResidue(r, scheme);
            bool selected = sele && r.firstAtomIdx >= 0 && sele->has(r.firstAtomIdx);

            if (selected) {
                win.setAttr(A_REVERSE);
                win.addCharColored(row, col, r.letter, color);
                win.unsetAttr(A_REVERSE);
            } else {
                win.addCharColored(row, col, r.letter, color);
            }
            ++col;
        }
    }
}

int SeqBar::resSeqAtColumn(int col, bool wrap, int winWidth) const {
    if (!wrap) {
        int idx = scrollOffset_ + col;
        if (idx >= 0 && idx < static_cast<int>(residues_.size()) && residues_[idx].resSeq >= 0)
            return residues_[idx].resSeq;
    }
    (void)winWidth;
    return -1;
}

int SeqBar::wrapRows(int winWidth) const {
    if (winWidth < 5) return 1;
    int rows = 1, col = 0;
    for (const auto& r : residues_) {
        if (r.resSeq == -1) { if (col > 0) { ++rows; col = 0; } continue; }
        if (r.resSeq == -2) { col = static_cast<int>(r.chainId.size()) + 1; continue; }
        if (col >= winWidth) { ++rows; col = 0; }
        ++col;
    }
    return rows;
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
