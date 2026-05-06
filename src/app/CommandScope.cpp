#include "molterm/app/CommandScope.h"

#include "molterm/app/Application.h"
#include "molterm/core/MolObject.h"

namespace molterm {

namespace {

// Build a name-resolver closure that looks up `$name` selections in the
// app-level table. Mirrors what Application::parseSelection does, but
// without the `sele` auto-save side effect — the auto-save is harmful
// when we re-parse the same expression once per object (we'd overwrite
// `$sele` with whichever object happened to be visited last).
Selection::NameResolver makeResolver(Application& app) {
    return [&app](const std::string& name) -> const Selection* {
        auto& table = app.namedSelections();
        auto it = table.find(name);
        return (it != table.end()) ? &it->second : nullptr;
    };
}

}  // namespace

int forEachInScope(Application& app, const std::string& expr,
                   const std::function<bool(ScopedTarget&)>& fn) {
    return forEachInScope(app, app.effectiveCommandScope(), expr, fn);
}

int forEachInScope(Application& app, ScopeMode mode, const std::string& expr,
                   const std::function<bool(ScopedTarget&)>& fn) {
    auto& tab = app.tabs().currentTab();
    auto resolver = makeResolver(app);
    int count = 0;

    auto visit = [&](std::shared_ptr<MolObject> obj) -> bool {
        if (!obj) return true;
        ScopedTarget t;
        t.obj = obj;
        if (expr.empty()) {
            t.sel = Selection::all(static_cast<int>(obj->atoms().size()));
            t.wholeObject = true;
        } else {
            t.sel = Selection::parse(expr, *obj, resolver);
            if (t.sel.empty()) return true;  // skip but keep iterating
            t.wholeObject = false;
        }
        ++count;
        return fn(t);
    };

    if (mode == ScopeMode::Current) {
        visit(tab.currentObject());
    } else {
        for (const auto& obj : tab.objects()) {
            if (!visit(obj)) break;
        }
    }
    // Mirror Application::parseSelection's transient behavior so the
    // ":select <expr>" → "$sele" workflow still works after a multi-
    // object command runs. We pick the first matched object (most
    // intuitive when N=1, harmless when N>1 since `$sele` is documented
    // as transient last-result).
    if (!expr.empty() && count > 0) {
        const auto& objs = tab.objects();
        for (const auto& obj : objs) {
            if (!obj) continue;
            auto sel = Selection::parse(expr, *obj, resolver);
            if (!sel.empty()) {
                app.namedSelections()[kSele] = std::move(sel);
                break;
            }
        }
    }
    return count;
}

ScopedAtoms collectInScope(Application& app, const std::string& expr) {
    return collectInScope(app, app.effectiveCommandScope(), expr);
}

ScopedAtoms collectInScope(Application& app, ScopeMode mode,
                           const std::string& expr) {
    ScopedAtoms out;
    forEachInScope(app, mode, expr, [&](ScopedTarget& t) {
        std::vector<int> idxs(t.sel.indices().begin(), t.sel.indices().end());
        out.totalAtoms += static_cast<int>(idxs.size());
        out.perObject.emplace_back(t.obj, std::move(idxs));
        return true;
    });
    return out;
}

}  // namespace molterm
