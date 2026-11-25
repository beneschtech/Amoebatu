/**
 * @file dcl_fuzz.cpp
 * @brief Mutation fuzzer for the `.dcl` loader and verifier.
 *
 * Usage: dcl_fuzz SEED ITERATIONS OUTDIR KEEP file.dcl...
 *
 * Each iteration takes one of the given (valid) files, applies a few random
 * mutations (bit flips, byte overwrites, truncation, inserted or deleted
 * bytes, and instruction-level edits inside CODE), and loads the result.
 * The only acceptable outcomes are DclError (rejected) or a loaded program;
 * any other exception is a bug, and so is a crash (run this under ASan).
 * Up to KEEP accepted mutants are written to OUTDIR, so a script can run them
 * (they're programs, and a mutated program may loop forever, so running
 * belongs in a separate process with a timeout).
 */
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <typeinfo>
#include <sstream>

#include "dcl.hpp"
#include "interp.hpp"

using namespace dolphin;

namespace {

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Byte range of the CODE section in a valid image (from the section directory), or {0, 0}.
std::pair<size_t, size_t> codeSection(const std::string& img) {
    if (img.size() < 64) return {0, 0};
    auto u32 = [&](size_t o) {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<unsigned char>(img[o + static_cast<size_t>(i)])) << (8 * i);
        return v;
    };
    auto u64 = [&](size_t o) { return static_cast<uint64_t>(u32(o)) | static_cast<uint64_t>(u32(o + 4)) << 32; };
    uint32_t n = u32(56);
    for (uint32_t i = 0; i < n && 64 + 24 * (i + 1) <= img.size(); ++i) {
        size_t e = 64 + 24 * i;
        if (std::memcmp(img.data() + e, "CODE", 4) == 0) return {u64(e + 8), u64(e + 16)};
    }
    return {0, 0};
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 6) {
        std::cerr << "usage: dcl_fuzz SEED ITERATIONS OUTDIR KEEP file.dcl...\n";
        return 2;
    }
    std::mt19937_64 rng(std::strtoull(argv[1], nullptr, 10));
    long iterations = std::strtol(argv[2], nullptr, 10);
    std::string outDir = argv[3];
    long keep = std::strtol(argv[4], nullptr, 10);
    std::vector<std::string> images;
    for (int i = 5; i < argc; ++i) images.push_back(readFile(argv[i]));

    Module m;
    m.file = "fuzz";
    Interp interp(m, 1, false);
    LoadHooks hooks;
    hooks.builtin = [&](const std::string& n) { return interp.builtin(n); };

    // Every original must load: otherwise the fuzzer would only test rejection paths.
    for (size_t i = 0; i < images.size(); ++i) {
        try {
            loadDcl(images[i], hooks);
        } catch (const DclError& e) {
            std::cerr << "dcl_fuzz: original " << argv[5 + i] << " doesn't load: " << e.what() << "\n";
            return 1;
        }
    }

    auto pick = [&](size_t n) { return n ? static_cast<size_t>(rng() % n) : 0; };
    long accepted = 0, rejected = 0, kept = 0;
    std::map<std::string, long> reasons;
    for (long it = 0; it < iterations; ++it) {
        std::string img = images[pick(images.size())];
        auto [codeOff, codeLen] = codeSection(img);
        int edits = 1 + static_cast<int>(pick(4));
        for (int e = 0; e < edits && !img.empty(); ++e) {
            switch (pick(9)) {
            case 0: img[pick(img.size())] ^= static_cast<char>(1 << pick(8)); break;   // bit flip
            case 1: img[pick(img.size())] = static_cast<char>(rng()); break;           // random byte
            case 2: img[pick(img.size())] = pick(2) ? '\0' : '\xff'; break;              // extreme byte
            case 3: img.resize(pick(img.size())); break;                                 // truncate
            case 4: img.insert(pick(img.size()), 1 + pick(8), static_cast<char>(rng())); break;
            case 5: {
                size_t at = pick(img.size());
                img.erase(at, 1 + pick(8));
                break;
            }
            case 6:   // header/directory field
                if (img.size() >= 64 + 24) img[pick(64 + 24)] = static_cast<char>(rng());
                break;
            default:   // inside CODE: a random word, or one operand byte of an instruction
                if (codeLen >= 4 && codeOff + codeLen <= img.size()) {
                    size_t w = codeOff + 4 * pick(codeLen / 4);
                    if (pick(2)) {
                        for (int k = 0; k < 4; ++k) img[w + static_cast<size_t>(k)] = static_cast<char>(rng());
                    } else {
                        img[w + pick(4)] = static_cast<char>(pick(2) ? pick(256) : pick(8));
                    }
                }
            }
        }
        try {
            loadDcl(img, hooks);
            ++accepted;
            if (kept < keep) {
                std::string path = outDir + "/mutant" + std::to_string(kept++) + ".dcl";
                std::ofstream(path, std::ios::binary).write(img.data(), static_cast<std::streamsize>(img.size()));
            }
        } catch (const DclError& e) {
            ++rejected;
            std::string why = e.what();
            if (auto c = why.find(':'); c != std::string::npos && why.compare(0, 7, "verify:") != 0) why = why.substr(0, c);
            else if (why.compare(0, 7, "verify:") == 0) {
                auto c2 = why.rfind(": ");
                why = "verify: " + why.substr(c2 + 2);
            }
            ++reasons[why];
        } catch (const std::exception& e) {
            std::cerr << "dcl_fuzz: BUG: iteration " << it << " threw " << typeid(e).name() << ": " << e.what() << "\n";
            std::ofstream(outDir + "/bug.dcl", std::ios::binary).write(img.data(), static_cast<std::streamsize>(img.size()));
            return 1;
        }
    }
    std::cout << "dcl_fuzz: " << iterations << " mutants: " << rejected << " rejected, " << accepted << " loaded\n";
    std::vector<std::pair<long, std::string>> top;
    for (auto& [w, n] : reasons) top.emplace_back(n, w);
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 12; ++i) std::cout << "  " << top[i].first << "  " << top[i].second << "\n";
    return 0;
}
