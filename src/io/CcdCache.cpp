#include "molterm/io/CcdCache.h"
#include "molterm/config/ConfigParser.h"

#include <gemmi/chemcomp.hpp>
#include <gemmi/cif.hpp>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace molterm::ccd {

namespace {

namespace fs = std::filesystem;

// Uppercase and strip whitespace — CCD codes are case-insensitive and the
// component file name uses the bare code (e.g. "ATP.cif").
std::string normalizeName(const std::string& resName) {
    std::string s;
    for (char c : resName) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        s += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

// ~/.molterm/ccd — reuses ConfigParser's ~/.molterm resolution. Empty if HOME
// is unset (then the on-disk cache is unavailable).
fs::path cacheDir() {
    std::string base = ConfigParser::configDir();
    if (base.empty()) return {};
    return fs::path(base) / "ccd";
}

bool fetchEnabled() {
    const char* v = std::getenv("MOLTERM_CCD_FETCH");
    return !(v && std::string(v) == "0");
}

// Download <name>.cif from RCSB into destPath using the popen+curl pattern that
// :fetch already relies on (no libcurl dependency). Returns true on success.
bool download(const std::string& name, const fs::path& destPath) {
    std::error_code ec;
    fs::create_directories(destPath.parent_path(), ec);
    fs::path tmp = destPath.string() + ".tmp";
    std::string url = "https://files.rcsb.org/ligands/download/" + name + ".cif";
    std::string cmd = "curl -sfL --max-time 15 -o '" + tmp.string() +
                      "' '" + url + "'";
    int rc = std::system(cmd.c_str());
    if (rc != 0 || !fs::exists(tmp, ec) || fs::file_size(tmp, ec) == 0) {
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, destPath, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

// Parse a component CIF into a ChemComp. Returns nullopt on any failure
// (missing file, malformed CIF).
std::optional<gemmi::ChemComp> parse(const fs::path& path) {
    std::error_code ec;
    if (path.empty() || !fs::exists(path, ec)) return std::nullopt;
    try {
        gemmi::cif::Document doc = gemmi::cif::read_file(path.string());
        if (doc.blocks.empty()) return std::nullopt;
        return gemmi::make_chemcomp_from_block(doc.blocks[0]);
    } catch (...) {
        return std::nullopt;
    }
}

struct Cache {
    std::mutex mtx;
    std::unordered_map<std::string, gemmi::ChemComp> hits;
    std::unordered_set<std::string> misses;  // resolution already failed
};

Cache& cache() {
    static Cache c;
    return c;
}

} // namespace

const gemmi::ChemComp* lookup(const std::string& resName) {
    std::string name = normalizeName(resName);
    if (name.empty()) return nullptr;

    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mtx);

    if (auto it = c.hits.find(name); it != c.hits.end()) return &it->second;
    if (c.misses.count(name)) return nullptr;

    // Cache a successful parse and hand back a stable pointer into the map.
    auto store = [&](std::optional<gemmi::ChemComp> comp) -> const gemmi::ChemComp* {
        if (!comp) return nullptr;
        return &c.hits.emplace(name, std::move(*comp)).first->second;
    };

    fs::path cached = cacheDir();
    if (!cached.empty()) cached /= name + ".cif";

    if (auto* hit = store(parse(cached)))                       // 2. on-disk cache
        return hit;
    if (const char* dir = std::getenv("MOLTERM_CCD_DIR"))      // 3. local CCD dir
        if (auto* hit = store(parse(fs::path(dir) / (name + ".cif"))))
            return hit;
    if (!cached.empty() && fetchEnabled() && download(name, cached))  // 4. fetch
        if (auto* hit = store(parse(cached)))
            return hit;

    c.misses.insert(name);
    return nullptr;
}

} // namespace molterm::ccd
