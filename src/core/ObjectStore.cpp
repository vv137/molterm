#include "molterm/core/ObjectStore.h"
#include <algorithm>

namespace molterm {

ObjectStore::ObjectPtr ObjectStore::add(std::unique_ptr<MolObject> obj) {
    std::string name = makeUniqueName(obj->name());
    obj->setName(name);
    auto ptr = std::shared_ptr<MolObject>(std::move(obj));
    objects_[name] = ptr;
    return ptr;
}

ObjectStore::ObjectPtr ObjectStore::get(const std::string& name) const {
    auto it = objects_.find(name);
    return (it != objects_.end()) ? it->second : nullptr;
}

bool ObjectStore::remove(const std::string& name) {
    return objects_.erase(name) > 0;
}

bool ObjectStore::rename(const std::string& oldName, const std::string& newName) {
    auto it = objects_.find(oldName);
    if (it == objects_.end()) return false;
    if (objects_.count(newName)) return false;
    auto ptr = it->second;
    objects_.erase(it);
    ptr->setName(newName);
    objects_[newName] = ptr;
    return true;
}

ObjectStore::ObjectPtr ObjectStore::clone(const std::string& name, const std::string& newName) {
    auto src = get(name);
    if (!src) return nullptr;
    auto copy = std::make_unique<MolObject>(*src);
    copy->setName(makeUniqueName(newName));
    return add(std::move(copy));
}

std::vector<std::string> ObjectStore::names() const {
    std::vector<std::string> result;
    result.reserve(objects_.size());
    for (const auto& [k, v] : objects_) result.push_back(k);
    std::sort(result.begin(), result.end());
    return result;
}

std::string ObjectStore::makeUniqueName(const std::string& base) const {
    if (!objects_.count(base)) return base;
    for (int i = 1; ; ++i) {
        std::string candidate = base + "_" + std::to_string(i);
        if (!objects_.count(candidate)) return candidate;
    }
}

} // namespace molterm
