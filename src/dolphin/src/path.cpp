/**
 * @file path.cpp
 * @brief Path text handling and filesystem operations. See path.hpp.
 */
#include "path.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sys/stat.h>

namespace fs = std::filesystem;

namespace dolphin {

// ------------------------------------------------------------- path text

std::string normalizePath(const std::string& in) {
    std::string s = in;
#ifdef _WIN32
    std::replace(s.begin(), s.end(), '\\', '/');
#endif
    bool abs = !s.empty() && s[0] == '/';
    std::string out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find('/', i);
        if (j == std::string::npos) j = s.size();
        std::string part = s.substr(i, j - i);
        if (!part.empty() && part != ".") {
            if (!out.empty()) out += '/';
            out += part;
        }
        i = j + 1;
    }
    if (abs) return "/" + out;
    return out.empty() ? "." : out;
}

bool pathIsAbsolute(const std::string& p) {
    return (!p.empty() && p[0] == '/') || fs::path(p).is_absolute();
}

std::string pathJoin(const std::string& a, const std::string& b) {
    if (pathIsAbsolute(b)) return normalizePath(b);
    return normalizePath(a + "/" + b);
}

std::string pathName(const std::string& p) {
    if (p == "/" || p == ".") return "";
    size_t i = p.rfind('/');
    return i == std::string::npos ? p : p.substr(i + 1);
}

std::string pathSuffix(const std::string& p) {
    std::string n = pathName(p);
    size_t i = n.rfind('.');
    if (i == std::string::npos || i == 0 || i + 1 == n.size()) return "";
    return n.substr(i);
}

std::string pathStem(const std::string& p) {
    std::string n = pathName(p);
    return n.substr(0, n.size() - pathSuffix(p).size());
}

std::string pathParent(const std::string& p) {
    if (p == "/" || p == ".") return p;
    size_t i = p.rfind('/');
    if (i == std::string::npos) return ".";
    if (i == 0) return "/";
    return p.substr(0, i);
}

std::vector<std::string> pathParts(const std::string& p) {
    std::vector<std::string> out;
    if (p == ".") return out;
    size_t i = 0;
    if (!p.empty() && p[0] == '/') {
        out.push_back("/");
        i = 1;
    }
    while (i < p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        out.push_back(p.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

// Iterative wildcard match with single-star backtracking (the classic
// algorithm): on a mismatch, retry from just after the last '*' with one more
// character absorbed by it. Linear in practice, no recursion.
bool globMatch(const std::string& pat, const std::string& s) {
    size_t p = 0, i = 0, starP = std::string::npos, starI = 0;
    // Match one character class at pat[p] ('[' ... ']') against c; advances p past it.
    auto matchClass = [&](size_t& q, char c, bool& ok) {
        size_t k = q + 1;
        bool neg = k < pat.size() && (pat[k] == '!' || pat[k] == '^');
        if (neg) ++k;
        bool hit = false, first = true;
        while (k < pat.size() && (first || pat[k] != ']')) {
            first = false;
            if (k + 2 < pat.size() && pat[k + 1] == '-' && pat[k + 2] != ']') {
                if (c >= pat[k] && c <= pat[k + 2]) hit = true;
                k += 3;
            } else {
                if (c == pat[k]) hit = true;
                ++k;
            }
        }
        if (k >= pat.size()) return false;   // no closing ']': treat '[' literally
        ok = hit != neg;
        q = k + 1;
        return true;
    };
    while (i < s.size()) {
        if (p < pat.size() && pat[p] == '*') {
            starP = p++;
            starI = i;
            continue;
        }
        if (p < pat.size()) {
            if (pat[p] == '?') { ++p; ++i; continue; }
            if (pat[p] == '[') {
                size_t q = p;
                bool ok = false;
                if (matchClass(q, s[i], ok)) {
                    if (ok) { p = q; ++i; continue; }
                } else if (s[i] == '[') { ++p; ++i; continue; }
            } else if (pat[p] == s[i]) { ++p; ++i; continue; }
        }
        if (starP == std::string::npos) return false;
        p = starP + 1;
        i = ++starI;
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

// ------------------------------------------------------------ filesystem

namespace pathfs {

namespace {
bool setErr(std::string& err, const std::string& p, const std::error_code& ec) {
    err = p + ": " + ec.message();
    return false;
}
bool hasWildcard(const std::string& c) { return c.find_first_of("*?[") != std::string::npos; }
double secs(const struct timespec& t) { return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_nsec) / 1e9; }

/// Sorted names in directory @p dir (empty on error).
std::vector<std::string> names(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        out.push_back(it->path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

bool isRealDir(const std::string& p) {   // a directory, not a symlink to one (** doesn't follow links)
    std::error_code ec;
    return fs::is_directory(fs::symlink_status(p, ec));
}

void globFrom(const std::string& dir, const std::vector<std::string>& comps, size_t k, std::set<std::string>& out) {
    if (k == comps.size()) {
        out.insert(dir);
        return;
    }
    const std::string& c = comps[k];
    bool last = k + 1 == comps.size();
    if (c == "**") {
        globFrom(dir, comps, k + 1, out);   // zero directories
        for (auto& n : names(dir)) {
            std::string sub = pathJoin(dir, n);
            if (isRealDir(sub)) globFrom(sub, comps, k, out);
        }
        return;
    }
    if (!hasWildcard(c)) {
        std::string next = pathJoin(dir, c);
        if (last ? exists(next) : isDir(next)) globFrom(next, comps, k + 1, out);
        return;
    }
    for (auto& n : names(dir)) {
        if (!globMatch(c, n)) continue;
        std::string next = pathJoin(dir, n);
        if (last || isDir(next)) globFrom(next, comps, k + 1, out);
    }
}

std::string filemode(mode_t m) {
    char t = '-';
#ifdef S_ISDIR
    if (S_ISDIR(m)) t = 'd';
#endif
#ifdef S_ISLNK
    if (S_ISLNK(m)) t = 'l';
#endif
#ifdef S_ISFIFO
    if (S_ISFIFO(m)) t = 'p';
#endif
#ifdef S_ISSOCK
    if (S_ISSOCK(m)) t = 's';
#endif
#ifdef S_ISCHR
    if (S_ISCHR(m)) t = 'c';
#endif
#ifdef S_ISBLK
    if (S_ISBLK(m)) t = 'b';
#endif
    std::string s(1, t);
    const char* rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i) s += (m & (0400 >> i)) ? rwx[i] : '-';
    if (m & 04000) s[3] = (m & 0100) ? 's' : 'S';
    if (m & 02000) s[6] = (m & 0010) ? 's' : 'S';
    if (m & 01000) s[9] = (m & 0001) ? 't' : 'T';
    return s;
}
} // namespace

bool exists(const std::string& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}
bool isDir(const std::string& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}
bool isFile(const std::string& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

bool iterdir(const std::string& p, std::vector<std::string>& out, std::string& err) {
    std::error_code ec;
    if (!fs::is_directory(p, ec)) {
        err = p + ": " + (fs::exists(p, ec) ? "Not a directory" : "No such file or directory");
        return false;
    }
    for (auto it = fs::directory_iterator(p, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        out.push_back(pathJoin(p, it->path().filename().string()));
    if (ec) return setErr(err, p, ec);
    std::sort(out.begin(), out.end());
    return true;
}

bool glob(const std::string& p, const std::string& pattern, std::vector<std::string>& out, std::string& err) {
    if (pattern.empty()) { err = "glob(): empty pattern"; return false; }
    if (pathIsAbsolute(pattern)) { err = "glob(): the pattern must be relative, like \"*.txt\" or \"**/*.csv\""; return false; }
    std::vector<std::string> comps;
    for (auto& c : pathParts(normalizePath(pattern))) comps.push_back(c);
    std::set<std::string> found;
    globFrom(p, comps, 0, found);
    out.assign(found.begin(), found.end());
    return true;
}

bool mkdir(const std::string& p, int mode, bool& created, std::string& err) {
    std::error_code ec;
    created = false;
    auto st = fs::status(p, ec);
    if (fs::exists(st)) {
        if (fs::is_directory(st)) return true;
        err = p + ": exists and is not a directory";
        return false;
    }
    fs::create_directories(p, ec);
    if (ec) return setErr(err, p, ec);
    if (mode >= 0) {
        fs::permissions(p, static_cast<fs::perms>(mode & 07777), fs::perm_options::replace, ec);
        if (ec) return setErr(err, p, ec);
    }
    created = true;
    return true;
}

bool rmdir(const std::string& p, std::string& err) {
    std::error_code ec;
    auto st = fs::symlink_status(p, ec);
    if (!fs::exists(st)) { err = p + ": No such file or directory"; return false; }
    if (!fs::is_directory(st)) { err = p + ": Not a directory (use unlink())"; return false; }
    fs::remove(p, ec);
    return ec ? setErr(err, p, ec) : true;
}

bool unlink(const std::string& p, std::string& err) {
    std::error_code ec;
    auto st = fs::symlink_status(p, ec);
    if (!fs::exists(st) && !fs::is_symlink(st)) { err = p + ": No such file or directory"; return false; }
    if (fs::is_directory(st)) { err = p + ": Is a directory (use rmdir())"; return false; }
    fs::remove(p, ec);
    return ec ? setErr(err, p, ec) : true;
}

bool rename(const std::string& p, const std::string& to, std::string& err) {
    std::error_code ec;
    fs::rename(p, to, ec);
    if (ec) { err = p + " -> " + to + ": " + ec.message(); return false; }
    return true;
}

bool chmod(const std::string& p, int mode, std::string& err) {
    std::error_code ec;
    fs::permissions(p, static_cast<fs::perms>(mode & 07777), fs::perm_options::replace, ec);
    return ec ? setErr(err, p, ec) : true;
}

bool stat(const std::string& p, StatInfo& out, std::string& err) {
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) { err = p + ": " + std::strerror(errno); return false; }
    out.size = static_cast<int64_t>(st.st_size);
    out.mode = static_cast<int>(st.st_mode & 07777);
    out.perms = filemode(st.st_mode);
#if defined(__APPLE__)
    out.mtime = secs(st.st_mtimespec);
    out.atime = secs(st.st_atimespec);
    out.ctime = secs(st.st_ctimespec);
#elif defined(_WIN32)
    out.mtime = static_cast<double>(st.st_mtime);
    out.atime = static_cast<double>(st.st_atime);
    out.ctime = static_cast<double>(st.st_ctime);
#else
    out.mtime = secs(st.st_mtim);
    out.atime = secs(st.st_atim);
    out.ctime = secs(st.st_ctim);
#endif
#ifndef _WIN32
    out.uid = static_cast<int64_t>(st.st_uid);
    out.gid = static_cast<int64_t>(st.st_gid);
    struct stat lst{};
    out.isLink = ::lstat(p.c_str(), &lst) == 0 && S_ISLNK(lst.st_mode);
#endif
    std::error_code ec;
    out.isDir = fs::is_directory(p, ec);
    out.isFile = fs::is_regular_file(p, ec);
    return true;
}

bool resolve(const std::string& p, std::string& out, std::string& err) {
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    if (ec) return setErr(err, p, ec);
    fs::path c = fs::weakly_canonical(a, ec);
    if (ec) return setErr(err, p, ec);
    out = normalizePath(c.generic_string());
    return true;
}

bool touch(const std::string& p, std::string& err) {
    std::error_code ec;
    if (fs::exists(p, ec)) {
        fs::last_write_time(p, fs::file_time_type::clock::now(), ec);
        return ec ? setErr(err, p, ec) : true;
    }
    std::ofstream f(p, std::ios::app);
    if (!f) { err = p + ": " + std::strerror(errno); return false; }
    return true;
}

bool readAll(const std::string& p, std::string& out, std::string& err) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { err = p + ": " + std::strerror(errno); return false; }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (f.bad()) { err = p + ": read error"; return false; }
    return true;
}

bool writeAll(const std::string& p, const std::string& data, std::string& err) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) { err = p + ": " + std::strerror(errno); return false; }
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.flush();
    if (!f) { err = p + ": write error"; return false; }
    return true;
}

} // namespace pathfs

} // namespace dolphin
