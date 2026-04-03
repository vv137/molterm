#pragma once

#include <memory>
#include <string>

#include "molterm/core/MolObject.h"

namespace molterm {

struct AssemblyInfo {
    std::string name;
    int oligomericCount = 0;
    std::string details;
};

class CifLoader {
public:
    // Load mmCIF or PDB file, returns a MolObject
    static std::unique_ptr<MolObject> load(const std::string& filepath);

    // Detect format and load
    static std::unique_ptr<MolObject> loadAuto(const std::string& filepath);

    // List biological assemblies in file
    static std::vector<AssemblyInfo> listAssemblies(const std::string& filepath);

    // Generate biological assembly by ID (returns new MolObject)
    static std::unique_ptr<MolObject> loadAssembly(const std::string& filepath,
                                                     const std::string& assemblyId);

    static std::string detectFormat(const std::string& filepath);
    static std::string baseNameFromPath(const std::string& filepath);

private:
    static std::unique_ptr<MolObject> loadCif(const std::string& filepath);
    static std::unique_ptr<MolObject> loadPdb(const std::string& filepath);
};

} // namespace molterm
