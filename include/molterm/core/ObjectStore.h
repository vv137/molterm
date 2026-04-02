#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "molterm/core/MolObject.h"

namespace molterm {

class ObjectStore {
public:
    using ObjectPtr = std::shared_ptr<MolObject>;

    ObjectPtr add(std::unique_ptr<MolObject> obj);
    ObjectPtr get(const std::string& name) const;
    bool remove(const std::string& name);
    bool rename(const std::string& oldName, const std::string& newName);
    ObjectPtr clone(const std::string& name, const std::string& newName);

    std::vector<std::string> names() const;
    size_t size() const { return objects_.size(); }
    bool empty() const { return objects_.empty(); }

    // Iteration
    auto begin() { return objects_.begin(); }
    auto end() { return objects_.end(); }
    auto begin() const { return objects_.begin(); }
    auto end() const { return objects_.end(); }

private:
    std::unordered_map<std::string, ObjectPtr> objects_;
    std::string makeUniqueName(const std::string& base) const;
};

} // namespace molterm
