#pragma once

// Private declarations shared by the Step-2 cache sources: the platform
// primitives, the on-disk header with its magic numbers, the payload hash and
// the builder fingerprint, the file check, the segmented payload, the RAII
// read-only mapping, and the file names.

#include "collatz/step2_cache.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace collatz::step2::cache::detail {

// Platform primitives: the only code that calls the Win32 file API. Everything
// below it (header/magic/hash/paths and every load_*/store_* validation) is
// plain C++. A primitive that fails returns a "nothing happened" result (null
// mapping / false write), which the shared layer turns into a cache miss or a
// skipped write: the cache is a pure speedup and must never fail a
// verification.
namespace platform {

// A read-only whole-file mapping plus the native handles that back it.
// `addr == nullptr` means "not mapped" (any failure funnels here).
struct Mapping {
    void* addr = nullptr;
    std::uint64_t size = 0;
    void* file = nullptr;    // HANDLE to the open file.
    void* mapping = nullptr; // HANDLE to the file-mapping object.
};

/// Sink for a streamed payload: appends `n` bytes, returns false on I/O error.
using PayloadSink = std::function<bool(const void*, std::uint64_t)>;
/// Emits the whole payload through the sink (any number of calls, in file
/// order); returns false to abort the write. The temp file is then discarded.
using PayloadProducer = std::function<bool(const PayloadSink&)>;

// Defined in step2_cache_map.cpp.
Mapping map_readonly(const std::string& path);
void unmap(Mapping& m) noexcept;

// Defined in step2_cache_write.cpp. The header bytes are written before the
// payload and again at offset 0 after produce() returns true, so a producer
// may fill in a header field it learns while streaming (the payload hash).
bool write_atomic_stream(const std::string& final_path,
                         const void* header,
                         std::size_t header_len,
                         const PayloadProducer& produce);
bool write_atomic(const std::string& final_path,
                  const void* header,
                  std::size_t header_len,
                  const void* payload,
                  std::uint64_t payload_bytes);
std::wstring to_wide(const std::string& s);

} // namespace platform

// Format version of the cache files (dip, bv), stored in the header. Version 5
// added the full-payload hash and the builder fingerprint. A file of another
// version is not read; the next store replaces it. The file names keep the
// _v4 suffix of the earlier format, so a version-4 file is replaced in place
// instead of being left next to its successor.
constexpr std::uint32_t kFormatVersion = 5;
constexpr std::uint64_t kMagicDip = 0x3130504944'5A43ull; // "CZ" "DIP" "01" packed, host-endian.
constexpr std::uint64_t kMagicBv = 0x31305642'5A43ull;    // "CZ" "BV" "01".

// Kind discriminates the payload shapes sharing one header.
enum class Kind : std::uint32_t { dip = 1, bv = 2 };

// Common on-disk header. Fixed size, host-endian. The reader rebuilds every
// field except payload_hash from its own key and builder, so a file is used
// only when all of them are equal and the payload hashes to payload_hash.
struct Header {
    std::uint64_t magic;
    std::uint32_t version;
    std::uint32_t kind;
    std::uint32_t B;
    std::uint32_t m_bits;        // 0 for the dip cache.
    std::uint32_t nbv;           // 0 for the dip cache.
    std::uint32_t fmin;          // 0 for the dip cache.
    std::uint32_t window_chains; // Dip tag t_w + 32*D; 0 without chain terms.
    std::uint32_t reserved;      // 0.
    std::uint64_t payload_bytes;
    std::uint64_t builder;      // builder_fingerprint() of the writer.
    std::uint64_t payload_hash; // PayloadHasher digest of the payload.
};
static_assert(sizeof(Header) == 64, "the payload must start 8-aligned at a fixed offset");

// The header a writer of `kind` stores and a reader expects, without the
// payload hash.
Header make_header(Kind kind,
                   unsigned B,
                   unsigned m_bits,
                   unsigned nbv,
                   unsigned fmin,
                   unsigned tw,
                   std::uint64_t payload_bytes);

// Payload hash: XXH64 (seed 0, host byte order) of each 1 MiB chunk of the
// payload, then XXH64 of the chunk digests in order. The chunks are independent,
// so a load hashes them on several threads; the digest does not depend on the
// thread count or on how a writer splits the bytes. Defined in
// step2_cache_hash.cpp.
constexpr unsigned kChunkLog2 = 20;
constexpr std::uint64_t kChunkBytes = std::uint64_t{1} << kChunkLog2;

// Streaming XXH64 (the published algorithm, so a chunk digest can be checked
// with any xxHash tool).
class Xxh64 {
public:
    Xxh64() noexcept;
    void update(const void* data, std::size_t len) noexcept;
    [[nodiscard]] std::uint64_t digest() const noexcept;

private:
    std::uint64_t v_[4];
    std::uint64_t total_ = 0;
    unsigned char buf_[32] = {};
    unsigned buffered_ = 0;
};

// The payload hash of bytes fed in file order, in pieces of any size.
class PayloadHasher {
public:
    void update(const void* data, std::uint64_t len);
    [[nodiscard]] std::uint64_t finish();

private:
    Xxh64 chunk_;
    std::uint64_t in_chunk_ = 0;
    std::vector<std::uint64_t> digests_;
};

// A payload made of several contiguous byte segments, in file order, so
// store_bitvectors writes inv3f, popcount and each vector without first copying
// them into one 1-2 GB buffer. Same bytes and hash as the concatenation.
struct Segments {
    std::vector<std::pair<const unsigned char*, std::uint64_t>> seg;

    void add(const void* p, std::uint64_t n) {
        seg.emplace_back(static_cast<const unsigned char*>(p), n);
    }
    [[nodiscard]] std::uint64_t size() const noexcept {
        std::uint64_t t = 0;
        for (const auto& s : seg) {
            t += s.second;
        }
        return t;
    }
    // Calls fn(ptr, n) on the pieces of bytes [off, off+n) of the concatenation.
    template <class Fn>
    void for_each_piece(std::uint64_t off, std::uint64_t n, const Fn& fn) const {
        for (const auto& s : seg) {
            if (n == 0) {
                return;
            }
            if (off >= s.second) {
                off -= s.second;
                continue;
            }
            const std::uint64_t take = std::min(n, s.second - off);
            fn(s.first + off, take);
            n -= take;
            off = 0;
        }
    }
    [[nodiscard]] bool write(const platform::PayloadSink& sink) const {
        for (const auto& s : seg) {
            if (!sink(s.first, s.second)) {
                return false;
            }
        }
        return true;
    }
};

// Threads for the payload hash: a load runs before any verifier worker exists,
// so it takes every hardware thread, like the cold build.
[[nodiscard]] unsigned hash_threads() noexcept;
[[nodiscard]] std::uint64_t
payload_hash(const unsigned char* data, std::uint64_t len, unsigned threads);
[[nodiscard]] std::uint64_t payload_hash(const Segments& segs, unsigned threads);

// RAII wrapper for a read-only mapping of a whole file (platform::map_readonly).
class MappedFile {
public:
    explicit MappedFile(const std::string& path) : m_(platform::map_readonly(path)) {}
    ~MappedFile() {
        platform::unmap(m_);
    }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    [[nodiscard]] bool ok() const noexcept {
        return m_.addr != nullptr;
    }
    [[nodiscard]] const unsigned char* data() const noexcept {
        return static_cast<const unsigned char*>(m_.addr);
    }
    [[nodiscard]] std::uint64_t size() const noexcept {
        return m_.size;
    }

private:
    platform::Mapping m_;
};

// True when the mapped file `f` (at `path`) can be used: its header equals
// `want` in every field but payload_hash, its size is the header plus
// want.payload_bytes, and the whole payload hashes to the stored payload_hash.
// A file that is present but fails prints one line to stderr. Defined in
// step2_cache_hash.cpp.
[[nodiscard]] bool check_file(const MappedFile& f, const Header& want, const std::string& path);

// Writes `header` followed by `payload` atomically to `final_path`; returns
// true on success.
inline bool atomic_write(const std::string& final_path,
                         const Header& header,
                         const void* payload,
                         std::uint64_t payload_bytes) {
    return platform::write_atomic(final_path, &header, sizeof(header), payload, payload_bytes);
}

// Removes the temp files of dead writers from `dir` (step2_cache_sweep.cpp).
void sweep_stale_temps(const std::string& dir) noexcept;

inline std::string join(const std::string& dir, const std::string& file) {
    if (dir.empty()) {
        return file;
    }
    return dir.back() == '/' ? dir + file : dir + "/" + file;
}

inline std::string dip_path(const std::string& dir, unsigned B, unsigned tw) {
    return join(dir, "dip_B" + std::to_string(B) + "_tw" + std::to_string(tw) + "_v4.u16");
}

inline std::string bv_path(const std::string& dir, const BvKey& key) {
    return join(dir,
                "bv_B" + std::to_string(key.B) + "_M" + std::to_string(key.m_bits) + "_k" +
                    std::to_string(key.nbv) + "_tw" + std::to_string(key.tw) + "_v4.bin");
}

// u64 words per packed 2^B-bit vector: ceil(2^B / 64), matching build_level in
// step2_build.cpp. Equals 2^B / 64 for every B >= 6; B < 6 is one (sub-)word.
constexpr std::uint64_t bv_words(unsigned B) noexcept {
    return ((std::uint64_t{1} << B) + 63u) / 64u;
}

} // namespace collatz::step2::cache::detail
