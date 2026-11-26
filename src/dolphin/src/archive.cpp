/**
 * @file archive.cpp
 * @brief Archive implementation on libarchive. See archive.hpp.
 */
#include "archive.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>

#ifdef DOLPHIN_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

namespace fs = std::filesystem;

namespace dolphin {

bool Archive::supported() {
#ifdef DOLPHIN_HAVE_LIBARCHIVE
    return true;
#else
    return false;
#endif
}

bool Archive::fail(std::string msg) {
    error_ = std::move(msg);
    return false;
}

#ifndef DOLPHIN_HAVE_LIBARCHIVE

Archive::Archive(std::string path, const std::string& mode) : path_(std::move(path)), writing_(mode != "r") {
    error_ = "archive support was not compiled into this dolphin (libarchive not found at build time)";
}
Archive::~Archive() = default;
bool Archive::list(std::vector<Entry>&) { return false; }
bool Archive::read(const std::string&, std::string&, bool&) { return false; }
bool Archive::extractAll(const std::string&, const std::vector<std::string>&, std::vector<std::string>&) { return false; }
bool Archive::addBytes(const std::string&, const std::string&) { return false; }
bool Archive::addPath(const std::string&, const std::string&) { return false; }
bool Archive::addOne(const std::string&, const std::string&) { return false; }
bool Archive::close() { return true; }

#else

namespace {

struct archive* W(void* p) { return static_cast<struct archive*>(p); }

bool endsWith(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

/// Set format and filter on writer @p a for a name ending in a known extension.
/// Returns an error message, or empty on success.
std::string configureWriter(struct archive* a, std::string ext) {
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto tar = [&](int (*filter)(struct archive*)) -> std::string {
        if (archive_write_set_format_pax_restricted(a) != ARCHIVE_OK) return archive_error_string(a);
        if (filter && filter(a) != ARCHIVE_OK) {
            const char* e = archive_error_string(a);
            return std::string("compression filter not available in this libarchive") + (e ? std::string(": ") + e : "");
        }
        return "";
    };
    if (endsWith(ext, ".tar.gz") || endsWith(ext, ".tgz")) return tar(archive_write_add_filter_gzip);
    if (endsWith(ext, ".tar.bz2") || endsWith(ext, ".tbz2") || endsWith(ext, ".tbz")) return tar(archive_write_add_filter_bzip2);
    if (endsWith(ext, ".tar.xz") || endsWith(ext, ".txz")) return tar(archive_write_add_filter_xz);
    if (endsWith(ext, ".tar.zst") || endsWith(ext, ".tzst")) return tar(archive_write_add_filter_zstd);
    if (endsWith(ext, ".tar.lz4")) return tar(archive_write_add_filter_lz4);
    if (endsWith(ext, ".tar")) return tar(nullptr);
    if (endsWith(ext, ".zip")) return archive_write_set_format_zip(a) == ARCHIVE_OK ? "" : archive_error_string(a);
    if (endsWith(ext, ".7z")) return archive_write_set_format_7zip(a) == ARCHIVE_OK ? "" : archive_error_string(a);
    if (endsWith(ext, ".cpio")) return archive_write_set_format_cpio_newc(a) == ARCHIVE_OK ? "" : archive_error_string(a);
    return "can't tell the archive format from the name; use .zip, .7z, .tar, .tar.gz, .tar.bz2, .tar.xz, "
           ".tar.zst, .tar.lz4 or .cpio, or a mode like \"w:tar.gz\"";
}

/// A reader positioned before the first header, or null with @p err set.
struct archive* openReader(const std::string& path, std::string& err) {
    std::error_code ec;   // libarchive only says "Failed to open"; say why
    auto st = fs::status(path, ec);
    if (!fs::exists(st)) { err = path + ": No such file or directory"; return nullptr; }
    if (fs::is_directory(st)) { err = path + ": Is a directory"; return nullptr; }
    struct archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, path.c_str(), 64 * 1024) != ARCHIVE_OK) {
        err = path + ": " + (archive_error_string(a) ? archive_error_string(a) : "cannot open");
        archive_read_free(a);
        return nullptr;
    }
    return a;
}

std::string entryName(struct archive_entry* e) {
    const char* n = archive_entry_pathname_utf8(e);
    if (!n) n = archive_entry_pathname(e);
    return n ? n : "";
}

double fileMtime(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 ? static_cast<double>(st.st_mtime) : static_cast<double>(std::time(nullptr));
}

} // namespace

Archive::Archive(std::string path, const std::string& mode) : path_(std::move(path)) {
    if (mode == "r") {
        std::string err;
        struct archive* a = openReader(path_, err);
        if (!a) { fail(err); return; }
        struct archive_entry* e;
        int rc = archive_read_next_header(a, &e);   // proves it's an archive we can read
        if (rc != ARCHIVE_OK && rc != ARCHIVE_EOF && rc != ARCHIVE_WARN)
            fail(path_ + ": " + (archive_error_string(a) ? archive_error_string(a) : "not a readable archive"));
        else
            valid_ = true;
        archive_read_free(a);
        return;
    }
    if (mode != "w" && mode.rfind("w:", 0) != 0) {
        fail("mode must be \"r\", \"w\" or \"w:<format>\", not \"" + mode + "\"");
        return;
    }
    writing_ = true;
    struct archive* a = archive_write_new();
    std::string err = configureWriter(a, mode == "w" ? path_ : "." + mode.substr(2));
    if (!err.empty()) {
        archive_write_free(a);
        fail(path_ + ": " + err);
        return;
    }
    if (archive_write_open_filename(a, path_.c_str()) != ARCHIVE_OK) {
        fail(path_ + ": " + (archive_error_string(a) ? archive_error_string(a) : "cannot create"));
        archive_write_free(a);
        return;
    }
    w_ = a;
    valid_ = true;
}

Archive::~Archive() { close(); }

bool Archive::close() {
    if (!w_) return true;
    struct archive* a = W(w_);
    w_ = nullptr;
    closed_ = true;
    bool ok = archive_write_close(a) == ARCHIVE_OK;
    if (!ok) fail(path_ + ": " + (archive_error_string(a) ? archive_error_string(a) : "cannot finish archive"));
    archive_write_free(a);
    valid_ = false;   // nothing more can be added
    return ok;
}

bool Archive::list(std::vector<Entry>& out) {
    if (writing_) return fail(path_ + ": opened for writing; namelist/infolist/read need mode \"r\"");
    std::string err;
    struct archive* a = openReader(path_, err);
    if (!a) return fail(err);
    struct archive_entry* e;
    int rc;
    while ((rc = archive_read_next_header(a, &e)) == ARCHIVE_OK || rc == ARCHIVE_WARN) {
        Entry x;
        x.name = entryName(e);
        x.size = archive_entry_size(e);
        x.mode = static_cast<int>(archive_entry_perm(e));
        x.mtime = static_cast<double>(archive_entry_mtime(e)) + archive_entry_mtime_nsec(e) / 1e9;
        x.isDir = archive_entry_filetype(e) == AE_IFDIR;
        out.push_back(std::move(x));
        archive_read_data_skip(a);
    }
    bool ok = rc == ARCHIVE_EOF;
    if (!ok) fail(path_ + ": " + (archive_error_string(a) ? archive_error_string(a) : "read error"));
    archive_read_free(a);
    return ok;
}

bool Archive::read(const std::string& name, std::string& out, bool& found) {
    found = false;
    if (writing_) return fail(path_ + ": opened for writing; namelist/infolist/read need mode \"r\"");
    std::string err;
    struct archive* a = openReader(path_, err);
    if (!a) return fail(err);
    struct archive_entry* e;
    int rc;
    bool ok = true;
    while ((rc = archive_read_next_header(a, &e)) == ARCHIVE_OK || rc == ARCHIVE_WARN) {
        if (entryName(e) != name) {
            archive_read_data_skip(a);
            continue;
        }
        found = true;
        char buf[64 * 1024];
        la_ssize_t n;
        while ((n = archive_read_data(a, buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
        if (n < 0) ok = fail(path_ + ": " + name + ": " + (archive_error_string(a) ? archive_error_string(a) : "read error"));
        break;
    }
    if (!found && rc != ARCHIVE_EOF) ok = fail(path_ + ": " + (archive_error_string(a) ? archive_error_string(a) : "read error"));
    archive_read_free(a);
    return ok;
}

bool Archive::extractAll(const std::string& dest, const std::vector<std::string>& names, std::vector<std::string>& done) {
    if (writing_) return fail(path_ + ": opened for writing; extractall needs mode \"r\"");
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) return fail(dest + ": " + ec.message());
    std::string err;
    struct archive* a = openReader(path_, err);
    if (!a) return fail(err);
    struct archive* disk = archive_write_disk_new();
    // Refuse members that would land outside dest: absolute paths, "..", and paths through symlinks.
    archive_write_disk_set_options(disk, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_SECURE_NODOTDOT |
                                             ARCHIVE_EXTRACT_SECURE_SYMLINKS | ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS);
    archive_write_disk_set_standard_lookup(disk);
    struct archive_entry* e;
    int rc;
    bool ok = true;
    std::string prefix = dest;
    if (!prefix.empty() && prefix.back() != '/') prefix += '/';
    while (ok && ((rc = archive_read_next_header(a, &e)) == ARCHIVE_OK || rc == ARCHIVE_WARN)) {
        std::string name = entryName(e);
        if (!names.empty() && std::find(names.begin(), names.end(), name) == names.end()) {
            archive_read_data_skip(a);
            continue;
        }
        if (name.empty() || name[0] == '/' || name == ".." || name.rfind("../", 0) == 0 ||
            name.find("/../") != std::string::npos || endsWith(name, "/..")) {
            ok = fail(path_ + ": member '" + name + "' would be written outside " + dest);
            break;
        }
        archive_entry_copy_pathname(e, (prefix + name).c_str());
        if (const char* hl = archive_entry_hardlink(e)) archive_entry_copy_hardlink(e, (prefix + hl).c_str());
        if (archive_write_header(disk, e) != ARCHIVE_OK) {
            ok = fail(name + ": " + (archive_error_string(disk) ? archive_error_string(disk) : "cannot extract"));
            break;
        }
        const void* block;
        size_t size;
        la_int64_t off;
        int r;
        while ((r = archive_read_data_block(a, &block, &size, &off)) == ARCHIVE_OK)
            if (archive_write_data_block(disk, block, size, off) != ARCHIVE_OK) { r = ARCHIVE_FATAL; break; }
        if (r != ARCHIVE_EOF) {
            ok = fail(name + ": " + (archive_error_string(a) ? archive_error_string(a) : "cannot extract"));
            break;
        }
        archive_write_finish_entry(disk);
        done.push_back(name);
    }
    if (ok && rc != ARCHIVE_EOF) ok = fail(path_ + ": " + (archive_error_string(a) ? archive_error_string(a) : "read error"));
    archive_write_close(disk);
    archive_write_free(disk);
    archive_read_free(a);
    return ok;
}

bool Archive::addBytes(const std::string& name, const std::string& data) {
    if (!writing_) return fail(path_ + ": opened for reading; writestr/write need mode \"w\"");
    if (!w_) return fail(path_ + ": already closed");
    struct archive_entry* e = archive_entry_new();
    archive_entry_set_pathname_utf8(e, name.c_str());
    archive_entry_set_size(e, static_cast<la_int64_t>(data.size()));
    archive_entry_set_filetype(e, AE_IFREG);
    archive_entry_set_perm(e, 0644);
    archive_entry_set_mtime(e, std::time(nullptr), 0);
    bool ok = archive_write_header(W(w_), e) == ARCHIVE_OK &&
              (data.empty() || archive_write_data(W(w_), data.data(), data.size()) == static_cast<la_ssize_t>(data.size()));
    if (!ok) fail(path_ + ": " + name + ": " + (archive_error_string(W(w_)) ? archive_error_string(W(w_)) : "write error"));
    archive_entry_free(e);
    return ok;
}

bool Archive::addOne(const std::string& path, const std::string& arcname) {
    std::error_code ec;
    fs::file_status st = fs::status(path, ec);
    if (ec) return fail(path + ": " + ec.message());
    struct archive_entry* e = archive_entry_new();
    archive_entry_set_pathname_utf8(e, arcname.c_str());
    archive_entry_set_perm(e, static_cast<mode_t>(st.permissions() & fs::perms::mask));
    archive_entry_set_mtime(e, static_cast<time_t>(fileMtime(path)), 0);
    bool ok = true;
    if (fs::is_directory(st)) {
        archive_entry_set_filetype(e, AE_IFDIR);
        archive_entry_set_size(e, 0);
        ok = archive_write_header(W(w_), e) == ARCHIVE_OK;
    } else if (fs::is_regular_file(st)) {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            archive_entry_free(e);
            return fail(path + ": " + std::strerror(errno));
        }
        auto size = fs::file_size(path, ec);
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_size(e, static_cast<la_int64_t>(size));
        ok = archive_write_header(W(w_), e) == ARCHIVE_OK;
        char buf[64 * 1024];
        while (ok && f) {
            f.read(buf, sizeof buf);
            std::streamsize n = f.gcount();
            if (n > 0 && archive_write_data(W(w_), buf, static_cast<size_t>(n)) != n) ok = false;
        }
    } else {
        archive_entry_free(e);
        return fail(path + ": only regular files and directories can be added");
    }
    if (!ok) fail(path_ + ": " + arcname + ": " + (archive_error_string(W(w_)) ? archive_error_string(W(w_)) : "write error"));
    archive_entry_free(e);
    return ok;
}

bool Archive::addPath(const std::string& path, const std::string& arcname) {
    if (!writing_) return fail(path_ + ": opened for reading; writestr/write need mode \"w\"");
    if (!w_) return fail(path_ + ": already closed");
    std::string base = arcname;
    if (base.empty()) {
        base = fs::path(path).lexically_normal().generic_string();
        while (!base.empty() && base[0] == '/') base.erase(0, 1);   // never store absolute names
        if (base.empty()) base = ".";
    }
    std::error_code ec;
    if (!fs::is_directory(path, ec)) return addOne(path, base);
    if (!addOne(path, base)) return false;
    // A directory: everything under it, in sorted order so the archive is reproducible.
    std::vector<fs::path> all;
    for (auto it = fs::recursive_directory_iterator(path, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        all.push_back(it->path());
    if (ec) return fail(path + ": " + ec.message());
    std::sort(all.begin(), all.end());
    for (auto& p : all) {
        std::string rel = p.lexically_relative(path).generic_string();
        if (!addOne(p.string(), base + "/" + rel)) return false;
    }
    return true;
}

#endif

} // namespace dolphin
