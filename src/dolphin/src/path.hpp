/**
 * @file path.hpp
 * @brief `Path`: filesystem paths and directory operations, after Python's pathlib.
 *
 *     d = Path("data") / "2026" / "11"       # join with /
 *     d.mkdir()                              # creates parents; fine if it exists
 *     for p in d.iterdir():                  # sorted
 *         if p.suffix == ".txt" and p.is_file():
 *             print(p.name, p.stat()["size"])
 *     for p in Path("data").rglob("*.grib2"): ...
 *
 * A Path value is immutable and pure: building one, joining, and reading the
 * fields (`name`, `stem`, `suffix`, `parent`, `parts`) touch no filesystem.
 * The methods that do (exists, iterdir, mkdir, stat, ...) need IO.
 *
 * Paths are kept normalized with forward slashes: `Path("a//b/./c/")` is
 * `a/b/c`. `..` is kept (resolving it needs the filesystem: resolve()).
 * Results that list several paths are sorted, so a program's output doesn't
 * depend on directory order.
 *
 * Built on std::filesystem, so it's the same on every platform; only stat()'s
 * uid/gid come from POSIX (0 on Windows).
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dolphin {

/// @brief A Path value: normalized text.
struct PathV {
    std::string p;
};

/// @brief Normalize: collapse "//" and "/./", drop a trailing "/"; "" becomes ".".
std::string normalizePath(const std::string& s);
/// @brief @p a / @p b. An absolute @p b replaces @p a, as in pathlib.
std::string pathJoin(const std::string& a, const std::string& b);
std::string pathName(const std::string& p);     ///< Last component: "obs.tar.gz".
std::string pathSuffix(const std::string& p);   ///< Last extension: ".gz" ("" for ".bashrc").
std::string pathStem(const std::string& p);     ///< Name without the suffix: "obs.tar".
std::string pathParent(const std::string& p);   ///< "a/b" -> "a", "a" -> ".", "/" -> "/".
std::vector<std::string> pathParts(const std::string& p);   ///< "/a/b" -> ["/", "a", "b"].
bool pathIsAbsolute(const std::string& p);

/// @brief Shell-style match of one name: `*`, `?`, `[abc]`, `[a-z]`, `[!x]`.
bool globMatch(const std::string& pattern, const std::string& name);

/// @brief Filesystem operations. Each returns false and sets @p err on failure.
namespace pathfs {

/// @brief What stat() reports.
struct StatInfo {
    int64_t size = 0;
    int mode = 0;            ///< Permission bits, including setuid/setgid/sticky (07777).
    std::string perms;       ///< ls-style: "-rw-r--r--", "drwxr-xr-x".
    double mtime = 0, atime = 0, ctime = 0;   ///< Seconds since the epoch.
    int64_t uid = 0, gid = 0;
    bool isDir = false, isFile = false, isLink = false;
};

bool exists(const std::string& p);
bool isDir(const std::string& p);
bool isFile(const std::string& p);
/// Entries of directory @p p, as joined paths, sorted.
bool iterdir(const std::string& p, std::vector<std::string>& out, std::string& err);
/// Paths under @p p matching @p pattern ("*.txt", "*/obs/*.csv", "**/*.grib2"), sorted.
bool glob(const std::string& p, const std::string& pattern, std::vector<std::string>& out, std::string& err);
/// Create @p p and any missing parents. @p created says whether it didn't exist; @p mode < 0: default.
bool mkdir(const std::string& p, int mode, bool& created, std::string& err);
bool rmdir(const std::string& p, std::string& err);    ///< Empty directories only.
bool unlink(const std::string& p, std::string& err);   ///< Files (and symlinks), not directories.
bool rename(const std::string& p, const std::string& to, std::string& err);
bool chmod(const std::string& p, int mode, std::string& err);
bool stat(const std::string& p, StatInfo& out, std::string& err);
bool resolve(const std::string& p, std::string& out, std::string& err);
bool touch(const std::string& p, std::string& err);
bool readAll(const std::string& p, std::string& out, std::string& err);
bool writeAll(const std::string& p, const std::string& data, std::string& err);

} // namespace pathfs

} // namespace dolphin
