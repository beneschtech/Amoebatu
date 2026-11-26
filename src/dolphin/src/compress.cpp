/**
 * @file compress.cpp
 * @brief Codec implementations. See compress.hpp.
 *
 * Decompression always streams into a growing buffer rather than trusting a
 * size in the header: gzip's trailer size is mod 2^32, zstd frames may omit
 * it, and a corrupt header must not make us allocate gigabytes up front.
 */
#include "compress.hpp"

#include <cstring>
#include <new>

#ifdef DOLPHIN_HAVE_ZLIB
#include <zlib.h>
#endif
#ifdef DOLPHIN_HAVE_BZIP2
#include <bzlib.h>
#endif
#ifdef DOLPHIN_HAVE_LZMA
#include <lzma.h>
#endif
#ifdef DOLPHIN_HAVE_ZSTD
#include <zstd.h>
#endif

namespace dolphin::codec {

namespace {

/// Every format this file knows, compiled in or not, in the order listed.
const char* const kKnown[] = {"gzip", "zlib", "deflate", "bz2", "xz", "zstd"};

Result fail(const char* kind, std::string msg) {
    Result r;
    r.kind = kind;
    r.error = std::move(msg);
    return r;
}
Result done(std::string data) {
    Result r;
    r.ok = true;
    r.data = std::move(data);
    return r;
}

bool isKnown(const std::string& f) {
    for (const char* k : kKnown)
        if (f == k) return true;
    return false;
}

Result unavailable(const std::string& f) {
    std::string have;
    for (auto& a : available()) have += (have.empty() ? "" : ", ") + a;
    if (!isKnown(f))
        return fail("ValueError", "unknown compression format '" + f + "' (known: gzip, zlib, deflate, bz2, xz, zstd)");
    return fail("ValueError", f + " support was not compiled into this dolphin (available: " +
                                  (have.empty() ? "none" : have) + ")");
}

Result badLevel(const std::string& f, int lo, int hi) {
    return fail("ValueError", f + " level must be " + std::to_string(lo) + ".." + std::to_string(hi));
}

/// Output grows in chunks of at least this much (and at least the input size).
constexpr size_t kChunk = 64 * 1024;

// ------------------------------------------------------------------ zlib

#ifdef DOLPHIN_HAVE_ZLIB
int windowBits(const std::string& f) { return f == "gzip" ? 15 + 16 : f == "zlib" ? 15 : -15; }

Result zlibCompress(const std::string& f, const std::string& in, int level) {
    if (level < -1 || level > 9) return badLevel(f, 0, 9);
    z_stream z{};
    if (deflateInit2(&z, level < 0 ? Z_DEFAULT_COMPRESSION : level, Z_DEFLATED, windowBits(f), 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return fail("ValueError", f + ": cannot initialize compressor");
    std::string out(deflateBound(&z, static_cast<uLong>(in.size())), '\0');
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    z.avail_in = static_cast<uInt>(in.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = static_cast<uInt>(out.size());
    int rc = deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    if (rc != Z_STREAM_END) return fail("ValueError", f + ": compression failed");
    return done(std::move(out));
}

Result zlibDecompress(const std::string& f, const std::string& in) {
    z_stream z{};
    if (inflateInit2(&z, windowBits(f)) != Z_OK) return fail("DecodeError", f + ": cannot initialize decompressor");
    std::string out;
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    z.avail_in = static_cast<uInt>(in.size());
    int rc = Z_OK;
    for (;;) {
        if (out.size() - z.total_out < kChunk) out.resize(out.size() + std::max(kChunk, in.size()));
        size_t have = z.total_out;
        z.next_out = reinterpret_cast<Bytef*>(out.data() + have);
        z.avail_out = static_cast<uInt>(out.size() - have);
        rc = inflate(&z, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) {
            // gzip files may be several members back to back (`cat a.gz b.gz`): keep going.
            if (f == "gzip" && z.avail_in > 0) {
                uLong total = z.total_out;
                inflateReset(&z);
                z.total_out = total;
                continue;
            }
            break;
        }
        if (rc != Z_OK) break;
        if (z.avail_in == 0 && z.avail_out != 0) { rc = Z_BUF_ERROR; break; }   // input ended mid-stream
    }
    size_t n = z.total_out;
    std::string msg = z.msg ? z.msg : "";
    inflateEnd(&z);
    if (rc != Z_STREAM_END)
        return fail("DecodeError", f + ": " + (rc == Z_BUF_ERROR ? std::string("truncated data") : "corrupt data" + (msg.empty() ? "" : " (" + msg + ")")));
    out.resize(n);
    return done(std::move(out));
}
#endif

// ----------------------------------------------------------------- bzip2

#ifdef DOLPHIN_HAVE_BZIP2
Result bz2Compress(const std::string& in, int level) {
    if (level == -1) level = 9;
    if (level < 1 || level > 9) return badLevel("bz2", 1, 9);
    // bzip2's documented worst case: input + 1% + 600 bytes.
    std::string out(in.size() + in.size() / 100 + 601, '\0');
    auto n = static_cast<unsigned>(out.size());
    int rc = BZ2_bzBuffToBuffCompress(out.data(), &n, const_cast<char*>(in.data()), static_cast<unsigned>(in.size()), level, 0, 0);
    if (rc != BZ_OK) return fail("ValueError", "bz2: compression failed");
    out.resize(n);
    return done(std::move(out));
}

Result bz2Decompress(const std::string& in) {
    std::string out;
    size_t pos = 0;
    do {   // one iteration per concatenated stream
        bz_stream s{};
        if (BZ2_bzDecompressInit(&s, 0, 0) != BZ_OK) return fail("DecodeError", "bz2: cannot initialize decompressor");
        s.next_in = const_cast<char*>(in.data() + pos);
        s.avail_in = static_cast<unsigned>(in.size() - pos);
        int rc;
        for (;;) {
            size_t have = out.size();
            out.resize(have + std::max(kChunk, in.size()));
            s.next_out = out.data() + have;
            s.avail_out = static_cast<unsigned>(out.size() - have);
            rc = BZ2_bzDecompress(&s);
            out.resize(out.size() - s.avail_out);
            if (rc != BZ_OK || (s.avail_in == 0 && s.avail_out != 0)) break;
        }
        pos = in.size() - s.avail_in;
        BZ2_bzDecompressEnd(&s);
        if (rc != BZ_STREAM_END) return fail("DecodeError", rc == BZ_OK ? "bz2: truncated data" : "bz2: corrupt data");
    } while (pos < in.size());
    return done(std::move(out));
}
#endif

// -------------------------------------------------------------------- xz

#ifdef DOLPHIN_HAVE_LZMA
Result xzCompress(const std::string& in, int level) {
    if (level == -1) level = 6;
    if (level < 0 || level > 9) return badLevel("xz", 0, 9);
    std::string out(lzma_stream_buffer_bound(in.size()), '\0');
    size_t n = 0;
    lzma_ret rc = lzma_easy_buffer_encode(static_cast<uint32_t>(level), LZMA_CHECK_CRC64, nullptr,
                                          reinterpret_cast<const uint8_t*>(in.data()), in.size(),
                                          reinterpret_cast<uint8_t*>(out.data()), &n, out.size());
    if (rc != LZMA_OK) return fail("ValueError", "xz: compression failed");
    out.resize(n);
    return done(std::move(out));
}

Result xzDecompress(const std::string& in) {
    lzma_stream s = LZMA_STREAM_INIT;
    if (lzma_stream_decoder(&s, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK)
        return fail("DecodeError", "xz: cannot initialize decompressor");
    std::string out;
    s.next_in = reinterpret_cast<const uint8_t*>(in.data());
    s.avail_in = in.size();
    lzma_ret rc;
    for (;;) {
        size_t have = out.size();
        out.resize(have + std::max(kChunk, in.size()));
        s.next_out = reinterpret_cast<uint8_t*>(out.data() + have);
        s.avail_out = out.size() - have;
        rc = lzma_code(&s, s.avail_in == 0 ? LZMA_FINISH : LZMA_RUN);
        out.resize(out.size() - s.avail_out);
        if (rc != LZMA_OK) break;
    }
    lzma_end(&s);
    if (rc != LZMA_STREAM_END)
        return fail("DecodeError", rc == LZMA_BUF_ERROR ? "xz: truncated data" : "xz: corrupt data");
    return done(std::move(out));
}
#endif

// ------------------------------------------------------------------ zstd

#ifdef DOLPHIN_HAVE_ZSTD
Result zstdCompress(const std::string& in, int level) {
    if (level == -1) level = ZSTD_CLEVEL_DEFAULT;
    if (level < 1 || level > ZSTD_maxCLevel()) return badLevel("zstd", 1, ZSTD_maxCLevel());
    std::string out(ZSTD_compressBound(in.size()), '\0');
    size_t n = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), level);
    if (ZSTD_isError(n)) return fail("ValueError", std::string("zstd: ") + ZSTD_getErrorName(n));
    out.resize(n);
    return done(std::move(out));
}

Result zstdDecompress(const std::string& in) {
    ZSTD_DStream* ds = ZSTD_createDStream();
    if (!ds) return fail("DecodeError", "zstd: cannot initialize decompressor");
    std::string out;
    ZSTD_inBuffer ib{in.data(), in.size(), 0};
    size_t rc = 1;
    while (ib.pos < ib.size) {
        size_t have = out.size();
        out.resize(have + std::max(kChunk, in.size()));
        ZSTD_outBuffer ob{out.data() + have, out.size() - have, 0};
        rc = ZSTD_decompressStream(ds, &ob, &ib);
        out.resize(have + ob.pos);
        if (ZSTD_isError(rc)) break;
        if (ob.pos == 0 && ib.pos == ib.size) break;
    }
    // Input may end exactly at a frame boundary with data still buffered.
    while (!ZSTD_isError(rc) && rc != 0) {
        size_t have = out.size();
        out.resize(have + kChunk);
        ZSTD_outBuffer ob{out.data() + have, kChunk, 0};
        rc = ZSTD_decompressStream(ds, &ob, &ib);
        out.resize(have + ob.pos);
        if (ob.pos == 0) break;
    }
    ZSTD_freeDStream(ds);
    if (ZSTD_isError(rc)) return fail("DecodeError", std::string("zstd: ") + ZSTD_getErrorName(rc));
    if (rc != 0) return fail("DecodeError", "zstd: truncated data");
    return done(std::move(out));
}
#endif

} // namespace

std::vector<std::string> available() {
    std::vector<std::string> v;
#ifdef DOLPHIN_HAVE_ZLIB
    v.insert(v.end(), {"gzip", "zlib", "deflate"});
#endif
#ifdef DOLPHIN_HAVE_BZIP2
    v.push_back("bz2");
#endif
#ifdef DOLPHIN_HAVE_LZMA
    v.push_back("xz");
#endif
#ifdef DOLPHIN_HAVE_ZSTD
    v.push_back("zstd");
#endif
    return v;
}

std::string detect(const std::string& in) {
    auto b = [&](size_t i) { return i < in.size() ? static_cast<unsigned char>(in[i]) : 0u; };
    if (b(0) == 0x1F && b(1) == 0x8B) return "gzip";
    if (in.size() >= 4 && in.compare(0, 3, "BZh") == 0 && b(3) >= '1' && b(3) <= '9') return "bz2";
    if (in.size() >= 6 && std::memcmp(in.data(), "\xFD" "7zXZ\0", 6) == 0) return "xz";
    if (b(0) == 0x28 && b(1) == 0xB5 && b(2) == 0x2F && b(3) == 0xFD) return "zstd";
    // zlib header (RFC 1950): method 8 (deflate), and the two bytes are a multiple of 31.
    if (in.size() >= 2 && (b(0) & 0x0F) == 8 && (b(0) >> 4) <= 7 && (b(0) * 256 + b(1)) % 31 == 0) return "zlib";
    return "";
}

Result compress(const std::string& f, const std::string& in, int level) {
    try {
#ifdef DOLPHIN_HAVE_ZLIB
        if (f == "gzip" || f == "zlib" || f == "deflate") return zlibCompress(f, in, level);
#endif
#ifdef DOLPHIN_HAVE_BZIP2
        if (f == "bz2") return bz2Compress(in, level);
#endif
#ifdef DOLPHIN_HAVE_LZMA
        if (f == "xz") return xzCompress(in, level);
#endif
#ifdef DOLPHIN_HAVE_ZSTD
        if (f == "zstd") return zstdCompress(in, level);
#endif
    } catch (const std::bad_alloc&) {
        return fail("ValueError", f + ": out of memory");
    }
    (void)in;
    (void)level;
    return unavailable(f);
}

Result decompress(const std::string& format, const std::string& in) {
    std::string f = format;
    if (f.empty()) {
        f = detect(in);
        if (f.empty())
            return fail("DecodeError", "decompress(): unrecognized data (not gzip, zlib, bz2, xz or zstd); "
                                       "raw deflate needs the format: decompress(data, \"deflate\")");
    }
    try {
#ifdef DOLPHIN_HAVE_ZLIB
        if (f == "gzip" || f == "zlib" || f == "deflate") return zlibDecompress(f, in);
#endif
#ifdef DOLPHIN_HAVE_BZIP2
        if (f == "bz2") return bz2Decompress(in);
#endif
#ifdef DOLPHIN_HAVE_LZMA
        if (f == "xz") return xzDecompress(in);
#endif
#ifdef DOLPHIN_HAVE_ZSTD
        if (f == "zstd") return zstdDecompress(in);
#endif
    } catch (const std::bad_alloc&) {
        return fail("DecodeError", f + ": out of memory");
    }
    return unavailable(f);
}

} // namespace dolphin::codec
