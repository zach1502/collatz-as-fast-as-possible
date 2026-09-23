// The Step-2 cache payload hash (XXH64 over 1 MiB chunks, then over the chunk
// digests), the header a reader expects, and the file check every load runs.

#include "collatz/step2_cache.hpp"
#include "collatz/threading.hpp"
#include "step2_cache_internal.hpp"
#include "step2_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace collatz::step2::cache::detail {

namespace {

constexpr std::uint64_t kP1 = 0x9E3779B185EBCA87ull;
constexpr std::uint64_t kP2 = 0xC2B2AE3D27D4EB4Full;
constexpr std::uint64_t kP3 = 0x165667B19E3779F9ull;
constexpr std::uint64_t kP4 = 0x85EBCA77C2B2AE63ull;
constexpr std::uint64_t kP5 = 0x27D4EB2F165667C5ull;

inline std::uint64_t rotl(std::uint64_t x, unsigned r) noexcept {
    return (x << r) | (x >> (64u - r));
}
inline std::uint64_t rd64(const unsigned char* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
inline std::uint64_t mix(std::uint64_t acc, std::uint64_t in) noexcept {
    return rotl(acc + in * kP2, 31) * kP1;
}
inline std::uint64_t merge(std::uint64_t h, std::uint64_t v) noexcept {
    return (h ^ mix(0, v)) * kP1 + kP4;
}

} // namespace

Xxh64::Xxh64() noexcept : v_{kP1 + kP2, kP2, 0, std::uint64_t{0} - kP1} {}

void Xxh64::update(const void* data, std::size_t len) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    total_ += len;
    if (buffered_ + len < sizeof(buf_)) {
        if (len != 0) {
            std::memcpy(buf_ + buffered_, p, len);
        }
        buffered_ += static_cast<unsigned>(len);
        return;
    }
    std::uint64_t a = v_[0];
    std::uint64_t b = v_[1];
    std::uint64_t c = v_[2];
    std::uint64_t d = v_[3];
    if (buffered_ != 0) {
        const std::size_t fill = sizeof(buf_) - buffered_;
        std::memcpy(buf_ + buffered_, p, fill);
        a = mix(a, rd64(buf_));
        b = mix(b, rd64(buf_ + 8));
        c = mix(c, rd64(buf_ + 16));
        d = mix(d, rd64(buf_ + 24));
        p += fill;
        len -= fill;
        buffered_ = 0;
    }
    // Four independent lanes per 32-byte stripe, so the multiplies overlap.
    for (; len >= 32u; p += 32, len -= 32u) {
        a = mix(a, rd64(p));
        b = mix(b, rd64(p + 8));
        c = mix(c, rd64(p + 16));
        d = mix(d, rd64(p + 24));
    }
    v_[0] = a;
    v_[1] = b;
    v_[2] = c;
    v_[3] = d;
    if (len != 0) {
        std::memcpy(buf_, p, len);
        buffered_ = static_cast<unsigned>(len);
    }
}

std::uint64_t Xxh64::digest() const noexcept {
    std::uint64_t h = 0;
    if (total_ >= 32u) {
        h = rotl(v_[0], 1) + rotl(v_[1], 7) + rotl(v_[2], 12) + rotl(v_[3], 18);
        for (const std::uint64_t v : v_) {
            h = merge(h, v);
        }
    } else {
        h = v_[2] + kP5; // v_[2] is the seed, 0.
    }
    h += total_;
    const unsigned char* p = buf_;
    unsigned n = buffered_;
    for (; n >= 8u; p += 8, n -= 8u) {
        h = rotl(h ^ mix(0, rd64(p)), 27) * kP1 + kP4;
    }
    if (n >= 4u) {
        std::uint32_t w;
        std::memcpy(&w, p, sizeof(w));
        h = rotl(h ^ (std::uint64_t{w} * kP1), 23) * kP2 + kP3;
        p += 4;
        n -= 4u;
    }
    for (; n != 0u; ++p, --n) {
        h = rotl(h ^ (std::uint64_t{*p} * kP5), 11) * kP1;
    }
    h ^= h >> 33;
    h *= kP2;
    h ^= h >> 29;
    h *= kP3;
    h ^= h >> 32;
    return h;
}

void PayloadHasher::update(const void* data, std::uint64_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    while (len != 0) {
        const std::uint64_t take = std::min(len, kChunkBytes - in_chunk_);
        chunk_.update(p, static_cast<std::size_t>(take));
        in_chunk_ += take;
        p += take;
        len -= take;
        if (in_chunk_ == kChunkBytes) {
            digests_.push_back(chunk_.digest());
            chunk_ = Xxh64();
            in_chunk_ = 0;
        }
    }
}

std::uint64_t PayloadHasher::finish() {
    if (in_chunk_ != 0) {
        digests_.push_back(chunk_.digest());
    }
    Xxh64 top;
    top.update(digests_.data(), digests_.size() * sizeof(std::uint64_t));
    return top.digest();
}

namespace {

// Hashes chunk k with digest_of(k) on `threads` threads, then the digests.
template <class DigestOf>
std::uint64_t chunked(std::uint64_t len, unsigned threads, const DigestOf& digest_of) {
    const std::uint64_t chunks = (len + kChunkBytes - 1u) / kChunkBytes;
    std::vector<std::uint64_t> d(static_cast<std::size_t>(chunks));
    step2::detail::run_chunked(
        std::max(1u, threads), chunks, 4, [&](std::uint64_t lo, std::uint64_t hi) {
            for (std::uint64_t k = lo; k < hi; ++k) {
                d[static_cast<std::size_t>(k)] =
                    digest_of(k, std::min(kChunkBytes, len - k * kChunkBytes));
            }
        });
    Xxh64 top;
    top.update(d.data(), d.size() * sizeof(std::uint64_t));
    return top.digest();
}

} // namespace

unsigned hash_threads() noexcept {
    return std::max(1u, hardware_threads());
}

std::uint64_t payload_hash(const unsigned char* data, std::uint64_t len, unsigned threads) {
    return chunked(len, threads, [data](std::uint64_t k, std::uint64_t n) {
        Xxh64 x;
        x.update(data + k * kChunkBytes, static_cast<std::size_t>(n));
        return x.digest();
    });
}

std::uint64_t payload_hash(const Segments& segs, unsigned threads) {
    return chunked(segs.size(), threads, [&segs](std::uint64_t k, std::uint64_t n) {
        Xxh64 x;
        segs.for_each_piece(k * kChunkBytes, n, [&x](const unsigned char* p, std::uint64_t m) {
            x.update(p, static_cast<std::size_t>(m));
        });
        return x.digest();
    });
}

Header make_header(Kind kind,
                   unsigned B,
                   unsigned m_bits,
                   unsigned nbv,
                   unsigned fmin,
                   unsigned tw,
                   std::uint64_t payload_bytes) {
    Header h{};
    h.magic = kind == Kind::dip ? kMagicDip : kMagicBv;
    h.version = kFormatVersion;
    h.kind = static_cast<std::uint32_t>(kind);
    h.B = B;
    h.m_bits = m_bits;
    h.nbv = nbv;
    h.fmin = fmin;
    h.window_chains = tw;
    h.payload_bytes = payload_bytes;
    h.builder = builder_fingerprint();
    return h;
}

namespace {

bool reject(const std::string& path, const char* why) {
    std::fprintf(stderr, "step2 cache: ignoring %s: %s\n", path.c_str(), why);
    return false;
}

bool check_file_impl(const MappedFile& f, const Header& want, const std::string& path) {
    if (!f.ok()) {
        return false; // absent or empty: a plain miss.
    }
    if (f.size() < sizeof(Header)) {
        return reject(path, "shorter than its header");
    }
    Header h{};
    std::memcpy(&h, f.data(), sizeof(h));
    if (h.magic != want.magic || h.kind != want.kind) {
        return reject(path, "not a cache file of this kind");
    }
    if (h.version != want.version) {
        return reject(path, "another format version");
    }
    if (h.B != want.B || h.m_bits != want.m_bits || h.nbv != want.nbv || h.fmin != want.fmin ||
        h.window_chains != want.window_chains || h.reserved != 0) {
        return reject(path, "its header names another table");
    }
    if (h.builder != want.builder) {
        return reject(path, "written by another table builder");
    }
    if (h.payload_bytes != want.payload_bytes || f.size() != sizeof(Header) + want.payload_bytes) {
        return reject(path, "wrong size");
    }
    if (payload_hash(f.data() + sizeof(Header), want.payload_bytes, hash_threads()) !=
        h.payload_hash) {
        return reject(path, "payload hash mismatch (the file is corrupt)");
    }
    return true;
}

} // namespace

bool check_file(const MappedFile& f, const Header& want, const std::string& path) {
    // The cache must never fail a verification: an allocation failure is a miss.
    try {
        return check_file_impl(f, want, path);
    } catch (...) {
        return false;
    }
}

} // namespace collatz::step2::cache::detail
