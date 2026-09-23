// The Step-2 dip cache: the natural-order dip file, its load and store, and the
// class-major load and streamed store of the same file. Every load checks the
// header, the size and the hash of the whole payload (check_file) first.

#include "collatz/step2_cache.hpp"
#include "step2_cache_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace collatz::step2::cache {

using namespace detail;

namespace {

std::uint64_t dip_payload_bytes(unsigned B) {
    return (std::uint64_t{1} << B) * sizeof(std::uint16_t);
}

Header dip_header(unsigned B, unsigned tw) {
    return make_header(Kind::dip, B, 0u, 0u, 0u, tw, dip_payload_bytes(B));
}

} // namespace

bool load_dip(const std::string& dir, unsigned B, unsigned tw, std::vector<std::uint16_t>& dip) {
    if (dir.empty()) {
        return false;
    }
    const std::string path = dip_path(dir, B, tw);
    const MappedFile f(path);
    if (!check_file(f, dip_header(B, tw), path)) {
        return false;
    }
    const std::uint64_t payload = dip_payload_bytes(B);
    dip.resize(static_cast<std::size_t>(std::uint64_t{1} << B));
    std::memcpy(dip.data(), f.data() + sizeof(Header), static_cast<std::size_t>(payload));
    return true;
}

bool store_dip(const std::string& dir,
               unsigned B,
               unsigned tw,
               const std::vector<std::uint16_t>& dip) {
    if (dir.empty() || dip.size() != (std::uint64_t{1} << B)) {
        return false;
    }
    // Best effort and never throws (the fingerprint and the hash allocate).
    try {
        Header h = dip_header(B, tw);
        const auto* bytes = reinterpret_cast<const unsigned char*>(dip.data());
        h.payload_hash = payload_hash(bytes, h.payload_bytes, hash_threads());
        return atomic_write(dip_path(dir, B, tw), h, dip.data(), h.payload_bytes);
    } catch (...) {
        return false;
    }
}

namespace {

// Splits [0, n) into `threads` contiguous ranges and runs body(lo, hi) on each.
template <class Body>
void parallel_ranges(std::uint64_t n, unsigned threads, const Body& body) {
    const unsigned nt =
        static_cast<unsigned>(std::max<std::uint64_t>(1u, std::min<std::uint64_t>(threads, n)));
    if (nt <= 1u) {
        body(std::uint64_t{0}, n);
        return;
    }
    const std::uint64_t chunk = (n + nt - 1u) / nt;
    std::vector<std::thread> pool;
    pool.reserve(nt);
    // If a spawn fails (std::system_error), this thread runs the remaining
    // ranges: destroying a joinable std::thread during unwinding would call
    // std::terminate.
    std::uint64_t inline_lo = n;
    for (unsigned t = 0; t < nt; ++t) {
        const std::uint64_t lo = static_cast<std::uint64_t>(t) * chunk;
        const std::uint64_t hi = std::min(lo + chunk, n);
        if (lo >= hi) {
            break;
        }
        try {
            pool.emplace_back([&body, lo, hi]() { body(lo, hi); });
        } catch (const std::system_error&) {
            inline_lo = lo;
            break;
        }
    }
    if (inline_lo < n) {
        body(inline_lo, n);
    }
    for (std::thread& th : pool) {
        th.join();
    }
}

} // namespace

bool load_dip_classmajor(const std::string& dir,
                         unsigned B,
                         unsigned tw,
                         unsigned H,
                         unsigned threads,
                         std::uint16_t* cm) {
    if (dir.empty() || H == 0 || H >= B) {
        return false;
    }
    const std::string path = dip_path(dir, B, tw);
    const MappedFile f(path);
    if (!check_file(f, dip_header(B, tw), path)) {
        return false;
    }
    const unsigned char* body = f.data() + sizeof(Header);
    // Transposes natural nat[c + (hh << L)] -> cm[(c << H) | hh]. Threads own
    // disjoint class ranges; a 64-class block keeps its 64 destination rows
    // (64 x 2^H x 2 B) cache-resident while hh sweeps the natural rows.
    const unsigned L = B - H;
    const std::uint64_t classes = std::uint64_t{1} << L;
    const std::uint64_t hspan = std::uint64_t{1} << H;
    const unsigned char* nat = body;
    parallel_ranges(classes, threads, [&](std::uint64_t lo, std::uint64_t hi) {
        constexpr std::uint64_t kBlk = 64;
        for (std::uint64_t c0 = lo; c0 < hi; c0 += kBlk) {
            const std::uint64_t c1 = std::min(c0 + kBlk, hi);
            for (std::uint64_t hh = 0; hh < hspan; ++hh) {
                for (std::uint64_t c = c0; c < c1; ++c) {
                    std::uint16_t v;
                    std::memcpy(&v, nat + 2u * (c + (hh << L)), sizeof(v));
                    cm[(c << H) | hh] = v;
                }
            }
        }
    });
    return true;
}

namespace {

bool store_dip_classmajor_impl(const std::string& dir,
                               unsigned B,
                               unsigned tw,
                               unsigned H,
                               unsigned threads,
                               const std::uint16_t* cm) {
    if (dir.empty() || H == 0 || H >= B) {
        return false;
    }
    const unsigned L = B - H;
    // The header goes out first with payload_hash 0; the natural-order chunks
    // are hashed in file order as they are written, and write_atomic_stream
    // writes the header again once produce() has filled in the hash.
    Header h = dip_header(B, tw);
    PayloadHasher hasher;
    // Natural rows hh in [h0, h0 + hb) stream through two bounded buffers
    // (<= 2^25 entries = 64 MB each), gathered in parallel over class ranges;
    // chunk k+1 is gathered while chunk k is being written.
    const std::uint64_t hspan = std::uint64_t{1} << H;
    const std::uint64_t classes = std::uint64_t{1} << L;
    const std::uint64_t hb =
        std::max<std::uint64_t>(1u, std::min<std::uint64_t>(hspan, (std::uint64_t{1} << 25) >> L));
    std::vector<std::uint16_t> buf[2];
    buf[0].resize(static_cast<std::size_t>(hb << L));
    buf[1].resize(static_cast<std::size_t>(hb << L));
    return platform::write_atomic_stream(
        dip_path(dir, B, tw), &h, sizeof(h), [&](const platform::PayloadSink& sink) {
            std::thread wr;
            bool wok = true;
            // Join the in-flight chunk writer before any exception leaves this
            // scope (a joinable std::thread destructor calls std::terminate);
            // write_atomic_stream turns the rethrown exception into `false`.
            struct JoinGuard {
                std::thread& t;
                ~JoinGuard() {
                    if (t.joinable()) {
                        t.join();
                    }
                }
            } guard{wr};
            std::uint64_t k = 0;
            for (std::uint64_t h0 = 0; h0 < hspan; h0 += hb, ++k) {
                std::vector<std::uint16_t>& out = buf[k & 1u];
                parallel_ranges(classes, threads, [&](std::uint64_t lo, std::uint64_t hi) {
                    for (std::uint64_t c = lo; c < hi; ++c) {
                        const std::uint16_t* row = cm + (c << H) + h0;
                        for (std::uint64_t j = 0; j < hb; ++j) {
                            out[(j << L) | c] = row[j];
                        }
                    }
                });
                if (wr.joinable()) {
                    wr.join(); // chunk k-1 (the other buffer) is on disk.
                }
                if (!wok) {
                    return false;
                }
                const std::uint16_t* data = out.data();
                const std::uint64_t bytes = (hb << L) * sizeof(std::uint16_t);
                // One writer at a time, so the hasher sees the chunks in order.
                wr = std::thread([&wok, &sink, &hasher, data, bytes]() {
                    try {
                        hasher.update(data, bytes);
                        wok = sink(data, bytes);
                    } catch (...) {
                        wok = false;
                    }
                });
            }
            if (wr.joinable()) {
                wr.join();
            }
            if (wok) {
                h.payload_hash = hasher.finish();
            }
            return wok;
        });
}

} // namespace

bool store_dip_classmajor(const std::string& dir,
                          unsigned B,
                          unsigned tw,
                          unsigned H,
                          unsigned threads,
                          const std::uint16_t* cm) {
    // Best effort, never throws: a failed allocation of the transpose buffers
    // (or any other exception) is a failed store, not a failed verification.
    try {
        return store_dip_classmajor_impl(dir, B, tw, H, threads, cm);
    } catch (...) {
        return false;
    }
}

} // namespace collatz::step2::cache
