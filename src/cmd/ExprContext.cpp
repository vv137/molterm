#include "molterm/cmd/ExprContext.h"

#include <cctype>
#include <memory>
#include <string>
#include <vector>

#include "molterm/app/Application.h"
#include "molterm/core/BondTable.h"
#include "molterm/core/MolObject.h"

namespace molterm {

// Shared selection→coordinate resolver for the expression context. When
// `masses` is non-null it is also filled with per-atom atomic weights (for
// com()); centroid()/pca() pass null. Commas are treated as list separators
// (the selection grammar is space-separated and never uses commas), so
// `pca(resi 1,2,3)` is accepted as a convenience alongside `pca(resi 1 2 3)`.
static int collectSelXYZ(Application& app, const std::string& expr,
                         std::vector<float>* xs, std::vector<float>* ys,
                         std::vector<float>* zs, std::vector<float>* masses) {
    auto obj = app.tabs().currentTab().currentObject();
    if (!obj) return 0;
    std::string normalized = expr;
    for (char& c : normalized) if (c == ',') c = ' ';
    auto sel = app.parseSelection(normalized, *obj);
    const auto& atoms = obj->atoms();
    for (int i : sel.indices()) {
        if (i < 0 || i >= (int)atoms.size()) continue;
        xs->push_back(atoms[i].x); ys->push_back(atoms[i].y); zs->push_back(atoms[i].z);
        if (masses) masses->push_back(atomicMass(atoms[i].element));
    }
    return static_cast<int>(xs->size());
}

RegisterExpr::Context makeExprContext(Application& app) {
    Application* self = &app;
    RegisterExpr::Context ctx;
    ctx.regs = &app.registers();
    ctx.resolveAtomPos = [self](const std::string& spec,
                                double* x, double* y, double* z) -> bool {
        // Atom-spec resolver — accepts `chain:resi:name` or `obj/chain:resi:name`
        // (the first '/' splits an optional object qualifier; issue #66).
        std::string objName, remainder = spec;
        auto slash = spec.find('/');
        if (slash != std::string::npos) {
            objName = spec.substr(0, slash);
            remainder = spec.substr(slash + 1);
        }
        std::shared_ptr<MolObject> obj = objName.empty()
            ? self->tabs().currentTab().currentObject()
            : self->store().get(objName);
        if (!obj) return false;
        std::string chain, resi, name;
        int part = 0;
        for (char c : remainder) {
            if (c == ':') { ++part; continue; }
            if (std::isspace(static_cast<unsigned char>(c))) continue;
            if      (part == 0) chain += c;
            else if (part == 1) resi += c;
            else                name += c;
        }
        int rs = -1;
        try { rs = std::stoi(resi); } catch (...) { return false; }
        for (const auto& a : obj->atoms()) {
            if (!chain.empty() && a.chainId != chain) continue;
            if (a.resSeq != rs) continue;
            if (!name.empty() && a.name != name) continue;
            *x = a.x; *y = a.y; *z = a.z;
            return true;
        }
        return false;
    };
    ctx.collectSelectionXYZ = [self](const std::string& expr,
                                     std::vector<float>* xs, std::vector<float>* ys,
                                     std::vector<float>* zs) -> int {
        return collectSelXYZ(*self, expr, xs, ys, zs, nullptr);
    };
    ctx.collectSelectionXYZMass = [self](const std::string& expr,
                                         std::vector<float>* xs, std::vector<float>* ys,
                                         std::vector<float>* zs, std::vector<float>* masses) -> int {
        return collectSelXYZ(*self, expr, xs, ys, zs, masses);
    };
    return ctx;
}

}  // namespace molterm
