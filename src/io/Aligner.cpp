#include "molterm/io/Aligner.h"

#include "molterm/io/PdbWriter.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <string>

namespace molterm {

namespace {

// Unpredictable token (16 hex chars) for temp file names, so a local attacker
// can't pre-create or symlink them ahead of us. Reads /dev/urandom and mixes
// in the pid as a fallback. Portable (no mkstemp, which strict -std=c++17 can
// hide on glibc).
std::string randomToken() {
    unsigned char b[8] = {};
    if (FILE* f = std::fopen("/dev/urandom", "rb")) {
        (void)std::fread(b, 1, sizeof(b), f);
        std::fclose(f);
    }
    unsigned long pid = static_cast<unsigned long>(getpid());
    for (size_t i = 0; i < sizeof(b); ++i)
        b[i] ^= static_cast<unsigned char>(pid >> (8 * (i % sizeof(pid))));
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (unsigned char c : b) { s += hex[c >> 4]; s += hex[c & 0xF]; }
    return s;
}

// Securely create + write a temp file: O_EXCL fails if the path already
// exists (no hijack of a pre-created file), O_NOFOLLOW fails on a symlink
// (no redirect to an attacker target), mode 0600. Returns false on any error.
bool writeSecureFile(const std::string& path, const std::string& content) {
    int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    const char* p = content.data();
    size_t left = content.size();
    bool ok = true;
    while (left > 0) {
        ssize_t w = ::write(fd, p, left);
        if (w <= 0) { ok = false; break; }
        p += w;
        left -= static_cast<size_t>(w);
    }
    ::close(fd);
    if (!ok) std::remove(path.c_str());
    return ok;
}

}  // namespace

std::string Aligner::usalignPath_ = "USalign";

void Aligner::setUSalignPath(const std::string& path) {
    usalignPath_ = path;
}

std::string Aligner::usAlignPath() {
    return usalignPath_;
}

AlignResult Aligner::align(const MolObject& mobile, const MolObject& target,
                            const std::vector<int>& mobileAtoms,
                            const std::vector<int>& targetAtoms) {
    return runUSalign(mobile, target, false, mobileAtoms, targetAtoms);
}

AlignResult Aligner::alignComplex(const MolObject& mobile, const MolObject& target,
                                   const std::vector<int>& mobileAtoms,
                                   const std::vector<int>& targetAtoms) {
    return runUSalign(mobile, target, true, mobileAtoms, targetAtoms);
}

void Aligner::applyTransform(MolObject& obj, const AlignResult& result) {
    if (!result.success) return;

    const auto& t = result.translation;
    const auto& u = result.rotation;

    for (auto& atom : obj.atoms()) {
        double x = atom.x, y = atom.y, z = atom.z;
        atom.x = static_cast<float>(t[0] + u[0][0]*x + u[0][1]*y + u[0][2]*z);
        atom.y = static_cast<float>(t[1] + u[1][0]*x + u[1][1]*y + u[1][2]*z);
        atom.z = static_cast<float>(t[2] + u[2][0]*x + u[2][1]*y + u[2][2]*z);
    }
}

AlignResult Aligner::runUSalign(const MolObject& mobile, const MolObject& target,
                                 bool complex,
                                 const std::vector<int>& mobileAtoms,
                                 const std::vector<int>& targetAtoms) {
    AlignResult result;

    // Write temp PDB files under unpredictable, exclusively-created paths
    // (writeSecureFile). A shared token keeps the trio easy to correlate.
    std::string token = randomToken();
    std::string tmpDir = "/tmp";
    std::string mobilePdb = tmpDir + "/molterm_mobile_" + token + ".pdb";
    std::string targetPdb = tmpDir + "/molterm_target_" + token + ".pdb";
    std::string matrixFile = tmpDir + "/molterm_matrix_" + token + ".txt";

    if (!writeSecureFile(mobilePdb, buildTempPDB(mobile, mobileAtoms)) ||
        !writeSecureFile(targetPdb, buildTempPDB(target, targetAtoms))) {
        // Remove whichever of the two we managed to create before failing.
        std::remove(mobilePdb.c_str());
        std::remove(targetPdb.c_str());
        result.message = "Failed to write temp PDB for alignment";
        return result;
    }

    // Build USalign command
    std::string cmd = usalignPath_ + " " + mobilePdb + " " + targetPdb +
                      " -m " + matrixFile +
                      " -ter 0";   // don't split by TER records
    if (complex) {
        cmd += " -mm 1";  // multi-chain (complex) alignment
    }
    cmd += " 2>&1";

    // Run USalign
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        result.message = "Failed to run USalign (is it installed?)";
        std::remove(mobilePdb.c_str());
        std::remove(targetPdb.c_str());
        return result;
    }

    std::string output;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe) != nullptr) {
        output += buf;
    }
    int exitCode = pclose(pipe);

    if (exitCode != 0) {
        result.message = "USalign failed (exit " + std::to_string(exitCode) + "): " + output;
        std::remove(mobilePdb.c_str());
        std::remove(targetPdb.c_str());
        std::remove(matrixFile.c_str());
        return result;
    }

    // Parse output
    result = parseOutput(output, matrixFile);

    // Cleanup
    std::remove(mobilePdb.c_str());
    std::remove(targetPdb.c_str());
    std::remove(matrixFile.c_str());

    return result;
}

std::string Aligner::buildTempPDB(const MolObject& obj,
                                  const std::vector<int>& atomIndices) {
    std::string out;
    const auto& atoms = obj.atoms();

    auto writeAtom = [&](int i, int serial) {
        char line[82];
        // USalign only needs tolerant ATOM records (raw names, no HETATM/TER),
        // so alignName=false; serial is 1..N over the written atoms.
        formatPdbAtomRecord(line, sizeof(line), "ATOM", serial, atoms[i],
                            /*alignName=*/false);
        out += line;
        out += '\n';
    };

    if (atomIndices.empty()) {
        for (int i = 0; i < static_cast<int>(atoms.size()); ++i)
            writeAtom(i, i + 1);
    } else {
        int serial = 1;
        for (int idx : atomIndices)
            writeAtom(idx, serial++);
    }
    out += "END\n";
    return out;
}

AlignResult Aligner::parseOutput(const std::string& output, const std::string& matrixFile) {
    AlignResult result;

    // Parse TM-score, RMSD, aligned length from stdout
    // Typical output lines:
    //   Aligned length=  123, RMSD=   1.23, Seq_ID=n_identical/n_aligned= 0.456
    //   TM-score= 0.9876 (normalized by length of Chain_1...)
    //   TM-score= 0.8765 (normalized by length of Chain_2...)

    std::istringstream iss(output);
    std::string line;
    int tmCount = 0;

    while (std::getline(iss, line)) {
        // Parse aligned length and RMSD
        if (line.find("Aligned length=") != std::string::npos) {
            std::sscanf(line.c_str(), "Aligned length= %d, RMSD= %lf",
                        &result.alignedLength, &result.rmsd);
        }
        // Parse TM-scores (first is normalized by chain 1, second by chain 2)
        if (line.find("TM-score=") != std::string::npos && line.find("normalized") != std::string::npos) {
            double tm = 0.0;
            std::sscanf(line.c_str(), "TM-score= %lf", &tm);
            if (tmCount == 0) result.tmScore1 = tm;
            else result.tmScore2 = tm;
            ++tmCount;
        }
    }

    // Parse rotation matrix from matrix file
    // Format:
    //  ------ The rotation matrix ...
    //  m               t[m]        u[m][0]        u[m][1]        u[m][2]
    //  0       1.2345678      0.9876543     -0.1234567      0.0987654
    //  1       2.3456789      0.1234567      0.9876543      0.0123456
    //  2       3.4567890     -0.0987654     -0.0123456      0.9876543
    std::ifstream matFile(matrixFile);
    if (matFile) {
        std::string mline;
        bool seen[3] = {false, false, false};
        while (std::getline(matFile, mline)) {
            int m;
            double t, u0, u1, u2;
            if (std::sscanf(mline.c_str(), " %d %lf %lf %lf %lf", &m, &t, &u0, &u1, &u2) == 5) {
                if (m >= 0 && m < 3) {
                    result.translation[m] = t;
                    result.rotation[m][0] = u0;
                    result.rotation[m][1] = u1;
                    result.rotation[m][2] = u2;
                    seen[m] = true;
                }
            }
        }
        // Only a *complete* 3-row matrix is a success. A file that opened but
        // held no/partial rotation rows previously set success=true with an
        // all-zero matrix, so applyTransform silently collapsed every mobile
        // atom onto the origin — corruption reported as success.
        if (seen[0] && seen[1] && seen[2]) {
            result.success = true;
        } else {
            result.message = "USalign produced no usable rotation matrix";
        }
    } else {
        result.message = "Failed to read rotation matrix";
    }

    if (result.success) {
        char msg[200];
        snprintf(msg, sizeof(msg),
            "TM1=%.4f TM2=%.4f RMSD=%.2f Aligned=%d",
            result.tmScore1, result.tmScore2, result.rmsd, result.alignedLength);
        result.message = msg;
    }

    return result;
}

} // namespace molterm
