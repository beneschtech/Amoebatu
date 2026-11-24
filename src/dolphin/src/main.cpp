/**
 * @file main.cpp
 * @brief The `dolphin` command-line driver.
 *
 * @code
 *   dolphin [option] file.dol [args...]
 * @endcode
 *
 * | Option     | Effect                                                     |
 * |------------|------------------------------------------------------------|
 * | (none)     | Analyze, then run. Calls `main(*args)` if defined.         |
 * | `--tokens` | Print the token stream and exit.                           |
 * | `--ast`    | Print the parsed tree, with inferred needs, and exit.      |
 * | `--needs`  | Print each function's inferred needs and exit.             |
 * | `--check`  | Parse and analyze only; exit 0 if the program is well-formed. |
 *
 * Exit codes: 0 success (or main()'s int return value); 1 a syntax error or
 * an uncaught Dolphin exception; 2 bad usage or an unreadable file.
 *
 * The pipeline is the same in every mode: Lexer → Parser → analyzeNeeds() → Interp.
 * The options just stop it early and print what's been built.
 */
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#include "analyzer.hpp"
#include "interp.hpp"
#include "lexer.hpp"
#include "parser.hpp"

using namespace dolphin;

static int usage() {
    std::cerr << "usage: dolphin [--tokens|--ast|--needs|--check] file.dol [args...]\n";
    return 2;
}

int main(int argc, char** argv) {
    std::string mode = "run";
    int i = 1;
    if (i < argc && argv[i][0] == '-' && argv[i][1] == '-') {
        mode = argv[i] + 2;
        if (mode != "tokens" && mode != "ast" && mode != "needs" && mode != "check") return usage();
        ++i;
    }
    if (i >= argc) return usage();
    std::string path = argv[i++];
    std::vector<std::string> args(argv + i, argv + argc);

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "dolphin: cannot open " << path << ": " << std::strerror(errno) << "\n";
        return 2;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string src = ss.str();

    try {
        if (mode == "tokens") {
            for (auto& t : Lexer(src, path).run()) {
                std::cout << t.line << ":" << t.col << "\t" << tokName(t.kind);
                if (!t.text.empty() || t.kind == Tok::Str) std::cout << "\t" << t.text;
                std::cout << "\n";
            }
            return 0;
        }

        Module m = parseSource(src, path);
        NeedsReport report = analyzeNeeds(m);
        for (auto& w : report.warnings) std::cerr << path << ": warning: " << w << "\n";

        if (mode == "ast") {
            dump(m, std::cout);
            return 0;
        }
        if (mode == "needs") {
            for (auto& [name, n] : report.functions) std::cout << name << "\t" << needsToString(n) << "\n";
            std::cout << "<module>\t" << needsToString(report.topLevel) << "\n";
            return 0;
        }
        if (mode == "check") return 0;

        Interp interp(m);
        return interp.run(args);
    } catch (const SyntaxError& e) {
        std::cerr << e.file << ":" << e.line << ": SyntaxError: " << e.what() << "\n";
        return 1;
    }
}
