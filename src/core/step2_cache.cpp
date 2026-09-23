// On-disk cache for the Step-2 look-ahead filter. See collatz/step2_cache.hpp
// for the design. The on-disk format (magic constants, header, payload hash,
// path builders, in step2_cache_internal.hpp and step2_cache_hash.cpp) and all
// load/store validation are plain C++; only a narrow set of platform
// primitives (map a file read-only, unmap, atomically write a buffer, make a
// directory tree, check a process id) call the Win32 API (file mapping,
// MoveFileExW), in step2_cache_map.cpp, step2_cache_write.cpp and
// step2_cache_sweep.cpp.
//
// This file has the packed-bitvector cache and step2_cache_dip.cpp the dip cache.

#include "collatz/step2_cache.hpp"

#include "collatz/step2.hpp"
#include "step2_cache_internal.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace collatz::step2::cache {

using namespace detail;

namespace {

// Payload bytes of a bitvector file: inv3f and popcount (nbv u64 each), then
// nbv packed vectors.
std::uint64_t bv_payload_bytes(const BvKey& key) {
    const std::uint64_t nbv = key.nbv;
    return (nbv * 2u + nbv * bv_words(key.B)) * sizeof(std::uint64_t);
}

Header bv_header(const BvKey& key) {
    return make_header(
        Kind::bv, key.B, key.m_bits, key.nbv, key.fmin, key.tw, bv_payload_bytes(key));
}

} // namespace

// Zero-copy load: check_file validates every header field, the exact size and
// the hash of the whole payload, then the mapping stays alive (via a
// shared_ptr<MappedFile>) and the result holds spans into it instead of a copy
// of the ~2 GiB payload. The hash pass reads every page, so it also brings a
// cold file into memory before the run uses it.
//
// A file that stays mapped for the whole run may make a concurrent writer's
// atomic replace of the same cache file fail; platform::map_readonly maps with
// FILE_SHARE_DELETE so the writer's MoveFileExW can still succeed, and if it
// does not, write_atomic_stream drops its temp file and returns false (a
// skipped cache write, never a fault). Any failure here (open, map, size,
// header, hash, inv3f) returns ok == false, and the caller rebuilds. The views
// are into a 64 KiB-aligned mapping at the 64-byte header offset, so the u64
// reinterpretation is aligned.
MappedBitvectors map_bitvectors(const std::string& dir, const BvKey& key) {
    MappedBitvectors out;
    if (dir.empty() || key.nbv == 0) {
        return out;
    }
    const std::string path = bv_path(dir, key);
    auto mf = std::make_shared<MappedFile>(path);
    if (!check_file(*mf, bv_header(key), path)) {
        return out;
    }
    // body is 8-aligned (sizeof(Header) == 64) over a page-aligned mapping, so the
    // reinterpret to const u64* is well-defined. Each bitvector is viewed in
    // place (no 2 GiB copy or zero-fill).
    const auto* u64 = reinterpret_cast<const std::uint64_t*>(mf->data() + sizeof(Header));
    // inv3f is a function of (fmin, B), so it is recomputed; a stored copy that
    // differs means the writer packed against other multipliers.
    out.inv3f.resize(key.nbv);
    for (unsigned i = 0; i < key.nbv; ++i) {
        out.inv3f[i] = Filter::inverse_pow3_mod2b(key.fmin + i, key.B);
        if (u64[i] != out.inv3f[i]) {
            std::fprintf(stderr, "step2 cache: ignoring %s: wrong inv3f\n", path.c_str());
            return MappedBitvectors{};
        }
    }
    const std::uint64_t words = bv_words(key.B);
    out.popcount.assign(u64 + key.nbv, u64 + 2u * key.nbv);
    out.bv.resize(key.nbv);
    for (unsigned i = 0; i < key.nbv; ++i) {
        out.bv[i] =
            u64 + 2u * static_cast<std::uint64_t>(key.nbv) + static_cast<std::uint64_t>(i) * words;
    }
    out.words = words;
    out.keepalive = std::move(mf);
    out.ok = true;
    return out;
}

bool store_bitvectors(const std::string& dir,
                      const BvKey& key,
                      const std::vector<std::vector<std::uint64_t>>& bvs,
                      const std::vector<std::uint64_t>& inv3f,
                      const std::vector<std::uint64_t>& popcount) {
    if (dir.empty() || key.nbv == 0 || bvs.size() != key.nbv || inv3f.size() != key.nbv ||
        popcount.size() != key.nbv) {
        return false;
    }
    const std::uint64_t words = bv_words(key.B);
    // Streams inv3f, popcount, then each bitvector straight from the vectors
    // instead of copying them into one buffer first (1 GB at B=30, 2 GB at B=31).
    Segments segs;
    segs.add(inv3f.data(), inv3f.size() * sizeof(std::uint64_t));
    segs.add(popcount.data(), popcount.size() * sizeof(std::uint64_t));
    for (unsigned i = 0; i < key.nbv; ++i) {
        if (bvs[i].size() != words) {
            return false;
        }
        segs.add(bvs[i].data(), words * sizeof(std::uint64_t));
    }
    // Best effort and never throws (the fingerprint and the hash allocate).
    try {
        Header h = bv_header(key);
        if (h.payload_bytes != segs.size()) {
            return false;
        }
        h.payload_hash = payload_hash(segs, hash_threads());
        return platform::write_atomic_stream(
            bv_path(dir, key), &h, sizeof(h), [&segs](const platform::PayloadSink& sink) {
                return segs.write(sink);
            });
    } catch (...) {
        return false;
    }
}

} // namespace collatz::step2::cache
