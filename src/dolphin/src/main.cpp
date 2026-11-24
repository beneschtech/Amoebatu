/**
 * @file main.cpp
 * @brief The `dolphin` command-line driver.
 *
 * @code
 *   dolphin [options] file.dol [args...]
 * @endcode
 *
 * | Option          | Effect                                                     |
 * |-----------------|------------------------------------------------------------|
 * | (none)          | Analyze, then run. Calls `main(*args)` if defined.         |
 * | `--tokens`      | Print the token stream and exit.                           |
 * | `--ast`         | Print the parsed tree, with bindings and needs, and exit.  |
 * | `--needs`       | Print each function's needs and whether its calls can run in parallel. |
 * | `--check`       | Parse and analyze only; exit 0 if the program is well-formed. |
 * | `--threads N`   | Run on exactly N threads, including the main one (1 = sequential). Default: automatic, adapting to load (see ThreadPool). |
 * | `--stats`       | After running, print wall time, CPU time (user + system, all threads), and dispatch/output statistics to stderr. |
 * | `--sync-output` | Write output directly from print() instead of via the printer thread. |
 * | `--trace`       | Log every dispatch decision (and why), failure, and parallel map to stderr. |
 * | `--fixed-thresholds` | Use the old constant thresholds (calls ≥ 50 µs, batches ~250 µs) instead of the measured ones. For A/B comparison. |
 *
 * Environment: `DOLPHIN_SHM=0` turns off the shared machine profile (SharedProfile).
 *
 * Exit codes: 0 success (or main()'s int return value); 1 a syntax error or
 * an uncaught Dolphin exception; 2 bad usage or an unreadable file.
 *
 * The pipeline is the same in every mode: Lexer → Parser → resolveNames() →
 * analyzeNeeds() → Interp.
 * The options just stop it early and print what's been built.
 */
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#include <sys/resource.h>

#include "analyzer.hpp"
#include "interp.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "resolver.hpp"

using namespace dolphin;

static int usage() {
    std::cerr << "usage: dolphin [--tokens|--ast|--needs|--check] [--threads N] [--stats] [--sync-output] [--trace] [--fixed-thresholds] file.dol [args...]\n";
    return 2;
}

int main(int argc, char** argv) {
    std::string mode = "run";
    unsigned threads = 0;
    bool stats = false;
    bool asyncOutput = true;
    bool traceOn = false;
    bool fixedThresholds = false;
    int i = 1;
    for (; i < argc && std::strncmp(argv[i], "--", 2) == 0; ++i) {
        std::string opt = argv[i] + 2;
        if (opt == "tokens" || opt == "ast" || opt == "needs" || opt == "check") {
            mode = opt;
        } else if (opt == "threads" && i + 1 < argc) {
            char* end = nullptr;
            long n = std::strtol(argv[++i], &end, 10);
            if (!end || *end || n < 1 || n > 1024) return usage();
            threads = static_cast<unsigned>(n);
        } else if (opt == "stats") {
            stats = true;
        } else if (opt == "sync-output") {
            asyncOutput = false;
        } else if (opt == "trace") {
            traceOn = true;
        } else if (opt == "fixed-thresholds") {
            fixedThresholds = true;
        } else {
            return usage();
        }
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
        resolveNames(m);
        NeedsReport report = analyzeNeeds(m);
        for (auto& w : report.warnings) std::cerr << path << ": warning: " << w << "\n";

        if (mode == "ast") {
            dump(m, std::cout);
            return 0;
        }
        if (mode == "needs") {
            // name, needs, and how calls to it will run.
            for (auto& [name, n] : report.functions) {
                const char* how = n != NeedNone             ? "inline (has needs)"
                                  : report.mutatesArgs[name] ? "inline (mutates arguments)"
                                                             : "parallel";
                std::cout << name << "\t" << needsToString(n) << "\t" << how << "\n";
            }
            std::cout << "<module>\t" << needsToString(report.topLevel) << "\n";
            return 0;
        }
        if (mode == "check") return 0;

        Interp interp(m, threads, asyncOutput);
        interp.setTrace(traceOn);
        interp.setFixedThresholds(fixedThresholds);
        // CPU time (all threads, user + system) next to wall time: under load
        // from other programs, wall time rewards a program for grabbing more
        // threads; CPU time shows what its run actually cost the machine.
        auto cpuMs = [] {
            rusage ru{};
            getrusage(RUSAGE_SELF, &ru);
            auto ms = [](const timeval& tv) { return static_cast<double>(tv.tv_sec) * 1e3 + static_cast<double>(tv.tv_usec) / 1e3; };
            return std::make_pair(ms(ru.ru_utime), ms(ru.ru_stime));
        };
        auto cpu0 = cpuMs();
        auto t0 = std::chrono::steady_clock::now();
        int rc = interp.run(args);
        if (stats) {
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            auto cpu1 = cpuMs();
            double user = cpu1.first - cpu0.first, sys = cpu1.second - cpu0.second;
            char cbuf[120];
            std::snprintf(cbuf, sizeof cbuf, ", cpu %.0f ms (user %.0f, sys %.0f)", user + sys, user, sys);
            std::cerr << "dolphin: " << interp.threads() << " thread(s), " << ms << " ms" << cbuf
                      << ", allocator " << DOLPHIN_ALLOCATOR_NAME;
            if (const ThreadPool* p = interp.pool(); p && p->adaptive()) {
                const PoolStats& s = p->stats();
                std::cerr << "; adaptive: up to " << interp.threads() << ", started " << interp.topology().start << " ("
                          << interp.topology().basis << "), now " << interp.activeThreads() << " (range "
                          << s.activeMin + 1 << "-" << s.activeMax + 1 << "; +" << s.raises << " -" << s.cuts << ", "
                          << s.reverted << " reverted), contention " << s.contentionPermille / 10.0 << "%";
                if (p->shared())
                    std::cerr << ", shared with " << s.othersProcesses << " other program(s) using " << s.othersThreads
                              << " threads, learned ceiling " << p->shared()->learnedCeiling();
            }
            if (PoolStats* ps = interp.stats())
                std::cerr << "; tasks dispatched " << ps->submitted << " (run by workers " << ps->byWorkers
                          << ", by waiting threads " << ps->byHelpers << "), ran inline because pool was busy "
                          << interp.inlineCalls();
            const CostModel& cm = interp.costModel();
            if (interp.threads() > 1 && cm.fixed()) {
                std::cerr << "; fixed thresholds: dispatch calls >= 50 us, batches ~250 us";
            } else if (interp.threads() > 1) {
                char buf[200];
                std::snprintf(buf, sizeof buf, "; rate %.1f Mops/s, submit %.1f us, wake %.1f us -> dispatch calls >= %.0f us, batches >= %.0f us",
                              cm.rate() * 1e3, cm.submitNs() / 1e3, cm.wakeNs() / 1e3, cm.callThresholdNs() / 1e3, cm.minBatchNs() / 1e3);
                std::cerr << buf;
            }
            if (const OutputChannel* oc = interp.output())
                std::cerr << "; output " << oc->messages() << " lines in " << oc->batches() << " writes, "
                          << oc->stalls() << " backpressure waits";
            else
                std::cerr << "; output synchronous";
            std::cerr << "\n";
        }
        return rc;
    } catch (const SyntaxError& e) {
        std::cerr << e.file << ":" << e.line << ": " << e.kind << ": " << e.what() << "\n";
        return 1;
    }
}
