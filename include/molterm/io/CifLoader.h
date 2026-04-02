#pragma once

#include <memory>
#include <string>

#include "molterm/core/MolObject.h"

namespace molterm {

class CifLoader {
public:
    // Load mmCIF or PDB file, returns a MolObject
    static std::unique_ptr<MolObject> load(const std::string& filepath);

    // Detect format and load
    static std::unique_ptr<MolObject> loadAuto(const std::string& filepath);

private:
    static std::unique_ptr<MolObject> loadCif(const std::string& filepath);
    static std::unique_ptr<MolObject> loadPdb(const std::string& filepath);
    static std::string detectFormat(const std::string& filepath);
    static std::string baseNameFromPath(const std::string& filepath);
};

} // namespace molterm
