/**
 * @file dcl.hpp
 * @brief The `.dcl` compiled-module file: writer, loader and verifier.
 *
 * A `.dcl` holds one module's p-code, so a program can run without its
 * source (standalone delivery, decision 21). Layout and the loader's checks
 * are described in doc/PCODE.md §6–7. In short:
 *
 * - A 64-byte header (magic, format version, source SHA-256, compiler id),
 *   a section directory, then 8-aligned sections: META, STRS (string table),
 *   CODE (all instruction words, so code can be mapped in place), FUNC
 *   (functions: parameters, constants, call sites, exception and line
 *   tables, analysis claims), IMPT/EXPT (imports and exports; imports must be
 *   empty until the language has modules) and optional DEBG (register names).
 * - A loaded file is untrusted. Every index, register, jump target and table
 *   range is checked before anything runs, and the analysis results that
 *   decide what may run on other threads are recomputed from the code; a
 *   file's own claims can only make a function *less* parallel.
 */
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.hpp"
#include "pcode.hpp"

namespace dolphin {

/// @brief A file that can't be written, or can't be loaded (malformed, wrong version, fails verification).
struct DclError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// @brief Format version written by this build. A loader refuses another major,
///        and a newer minor. Major 0 means "development format": it may change.
constexpr uint16_t kDclMajor = 0;
constexpr uint16_t kDclMinor = 1;

/**
 * @brief A compiled module, ready to run, with no source behind it.
 *
 * Built by loadDcl(). Owns stand-in Defs (name, parameters, defaults,
 * analysis flags; no body) so the interpreter's call and dispatch paths work
 * unchanged, and owns the strings that string constants borrow.
 */
struct Program {
    std::string file;                              ///< Source file name, for error messages.
    std::array<uint8_t, 32> sourceHash{};          ///< SHA-256 of the source it was compiled from.
    std::vector<std::unique_ptr<Def>> defs;        ///< Function table (index = function number).
    std::vector<std::unique_ptr<PCode>> code;      ///< Parallel to @c defs.
    std::unique_ptr<Def> moduleDef;                ///< Stand-in naming module code `<module>`.
    std::unique_ptr<PCode> init;                   ///< Module top-level code.
    std::vector<std::shared_ptr<const Str>> strs;  ///< Keeps borrowed string constants alive.
};

/// @brief What the loader needs from the interpreter.
struct LoadHooks {
    /// The value of builtin @p name (function or exception class), or null if there's none.
    std::function<const Value*(const std::string& name)> builtin;
};

/**
 * @brief Serialize a compiled module.
 * @param m        The module (function order, parameter defaults, exports).
 * @param fns      Every function's p-code (Interp::pcodes()).
 * @param init     Module top-level code (Interp::modulePcode()).
 * @param source   Source text, hashed into the header.
 * @param debug    Include register names (the DEBG section).
 * @throws DclError if a function or the module code isn't compiled, or a
 *         default isn't a constant.
 */
std::string writeDcl(const Module& m, const std::unordered_map<const Def*, std::unique_ptr<PCode>>& fns,
                     const PCode* init, const std::string& source, bool debug);

/**
 * @brief Parse and verify a `.dcl` image.
 * @throws DclError with a one-line reason if anything is malformed or fails verification.
 */
std::unique_ptr<Program> loadDcl(const std::string& image, const LoadHooks& hooks);

/// @brief Does @p data start with the `.dcl` magic number?
bool isDcl(const std::string& data);

/// @brief SHA-256 of @p data.
std::array<uint8_t, 32> sha256(const std::string& data);

/**
 * @brief Verify a loaded program's code (doc/PCODE.md §7); called by loadDcl().
 *
 * Checks every operand against its table, control flow (no jump out of the
 * code, no falling off the end, multi-word instructions complete), exception
 * table ranges, SETG only in module code, that APPEND/DICTSET only target a
 * container the function just created and hasn't let escape, and that
 * SPAWN/PMAP name functions. Then recomputes needs and argument mutation
 * from the code and keeps the more conservative of that and the file's claim.
 * @throws DclError on the first problem found.
 */
void verifyProgram(Program& p);

} // namespace dolphin
