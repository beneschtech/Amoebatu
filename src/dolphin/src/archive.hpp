/**
 * @file archive.hpp
 * @brief `Archive(path[, mode])`: zip, tar (plain or compressed), 7z, cpio, via libarchive.
 *
 * The method names follow Python's zipfile so they feel familiar:
 *
 *     a = Archive("obs.tar.gz")            # read: any format libarchive recognizes
 *     a.namelist()                         # member names, in archive order
 *     a.infolist()                         # [{"name", "size", "mode", "mtime", "is_dir"}, ...]
 *     a.read("2026/11/obs.txt")            # bytes; KeyError if there's no such member
 *     a.extractall("out")                  # names extracted; never writes outside "out"
 *
 *     w = Archive("bundle.zip", "w")       # write: format from the extension
 *     w.writestr("notes.txt", "text")      # a member from str or bytes
 *     w.write("data/obs.txt")              # a file from disk (a directory: recursively)
 *     w.close()                            # finishes the archive
 *
 * Write formats by extension (or `"w:<ext>"` as the mode): .zip, .7z, .tar,
 * .tar.gz/.tgz, .tar.bz2/.tbz2, .tar.xz/.txz, .tar.zst/.tzst, .tar.lz4, .cpio.
 *
 * Like Stream, constructing one never raises: check `valid` and `error`.
 * Operations on an invalid archive, or failing ones, raise IOError. Every
 * method is an effect (Need IO). Without libarchive at build time, every
 * Archive is invalid with an error saying so.
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dolphin {

class Archive {
public:
    /// @brief One member, as infolist() reports it.
    struct Entry {
        std::string name;
        int64_t size = 0;
        int mode = 0;        ///< Permission bits (e.g. 0644).
        double mtime = 0;    ///< Seconds since the epoch.
        bool isDir = false;
    };

    /// @brief Open @p path for reading (@p mode "r") or create it for writing ("w" or "w:<ext>").
    Archive(std::string path, const std::string& mode);
    ~Archive();
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;

    bool valid() const { return valid_; }
    const std::string& error() const { return error_; }
    const std::string& path() const { return path_; }
    bool writing() const { return writing_; }

    /// @name Reading (each scans the archive from the start)
    /// @{
    bool list(std::vector<Entry>& out);
    /// @p found is false (and the result true) if there's no member called @p name.
    bool read(const std::string& name, std::string& out, bool& found);
    /// Extract into @p dest (created if needed); only @p names if not empty. @p done gets each name written.
    bool extractAll(const std::string& dest, const std::vector<std::string>& names, std::vector<std::string>& done);
    /// @}

    /// @name Writing
    /// @{
    bool addBytes(const std::string& name, const std::string& data);
    /// Add the file at @p path as @p arcname (empty: @p path itself, without a leading '/'). Directories recurse.
    bool addPath(const std::string& path, const std::string& arcname);
    /// @}

    /// Finish writing (a no-op when reading). Safe to call twice.
    bool close();

    /// @brief Whether this build has libarchive.
    static bool supported();

private:
    bool fail(std::string msg);
    bool addOne(const std::string& path, const std::string& arcname);

    std::string path_;
    bool writing_ = false;
    bool valid_ = false;
    bool closed_ = false;
    std::string error_;
    void* w_ = nullptr;   ///< struct archive* while writing.
};

} // namespace dolphin
