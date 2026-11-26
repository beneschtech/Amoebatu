/**
 * @file compress.hpp
 * @brief In-memory compression codecs: `compress()`, `decompress()`, `compressors()`.
 *
 * | Format    | Library | Notes                                              |
 * |-----------|---------|----------------------------------------------------|
 * | `gzip`    | zlib    | `.gz` files; concatenated members are all read     |
 * | `zlib`    | zlib    | zlib wrapper (RFC 1950), as Python's zlib.compress |
 * | `deflate` | zlib    | raw deflate (RFC 1951): no header, can't be detected |
 * | `bz2`     | bzip2   | `.bz2`; concatenated streams (pbzip2) are all read  |
 * | `xz`      | liblzma | `.xz`; concatenated streams are all read            |
 * | `zstd`    | libzstd | `.zst`; every frame is read                         |
 *
 * Each library is optional (CMakeLists.txt finds what's installed). A format
 * whose library wasn't found is still *known*, so asking for it gives "not
 * compiled in" rather than "unknown format".
 *
 * Everything here is pure: bytes in, bytes out, no I/O. So decompressing many
 * blobs in a comprehension runs as a parallel map.
 */
#pragma once
#include <string>
#include <vector>

namespace dolphin::codec {

/// @brief Outcome of a codec call. On failure @c kind is the Dolphin exception class.
struct Result {
    bool ok = false;
    std::string data;   ///< Output bytes when ok.
    std::string kind;   ///< "ValueError" (bad format or level) or "DecodeError" (corrupt input).
    std::string error;  ///< Message when !ok.
};

/// @brief The formats compiled into this build, in a fixed order.
std::vector<std::string> available();

/**
 * @brief Compress @p in as @p format.
 * @param format  One of the names in the table above.
 * @param in      Input bytes.
 * @param level   Compression level, or -1 for the library's default. Ranges:
 *                gzip/zlib/deflate 0..9, bz2 1..9, xz 0..9, zstd 1..22.
 */
Result compress(const std::string& format, const std::string& in, int level);

/**
 * @brief Decompress @p in.
 * @param format  A format name, or empty to detect it from the leading bytes
 *                (works for everything except raw `deflate`).
 * @param in      Compressed bytes.
 */
Result decompress(const std::string& format, const std::string& in);

/// @brief The format @p in looks like, from its magic bytes; empty if unrecognized.
std::string detect(const std::string& in);

} // namespace dolphin::codec
