#ifndef COLLATZ_STEP2_CACHE_HPP
#define COLLATZ_STEP2_CACHE_HPP

// On-disk cache for the Step-2 look-ahead filter. The dip_B table depends only
// on B (and the window tag tw), and the packed bitvectors depend only on
// (B, m_bits = N-A, num_bitvectors, tw), never on the thread count, kernel, or
// the individual survivors. So both are built once and cached under a per-user
// cache directory; later runs at the same key load a memory-mapped file and
// skip the O(2^B) dip walk and the per-vector packing.
//
// Two caches share one little-endian header format:
//   * dip cache   -- key (B, tw):              dip_B<B>_tw<tag>_v4.u16  (2^B uint16)
//   * bitvector   -- key (B, m_bits, nbv, tw): bv_B<B>_M<M>_k<k>_tw<tag>_v4.bin
// The header carries the format version (5), the key fields, the payload size,
// the builder fingerprint of the writing binary (builder_fingerprint) and a
// hash of the whole payload. A load uses a file only when every field equals
// what this binary would write and the payload hashes to the stored value
// (XXH64 over 1 MiB chunks on every hardware thread).
// inv3f is recomputed and compared. Any mismatch or I/O error makes the caller
// rebuild (and the next store replace the file); the cache never changes the
// computed mask. Writes go to a temp file and are atomically renamed into
// place, so a crashed or partial write never yields a half file.
//
// Platform layer: the on-disk format, validation, and path logic are
// single-sourced in step2_cache.cpp, step2_cache_dip.cpp and
// step2_cache_internal.hpp; only a narrow set of platform primitives (map a
// file read-only, unmap, atomically write a buffer, make a directory tree, in
// step2_cache_map.cpp and step2_cache_write.cpp) call the Win32 API: the file
// mapping, FlushFileBuffers and MoveFileExW (cache dir %LOCALAPPDATA%\collatz,
// or $COLLATZ_CACHE_DIR). The files are host-endian and host-word-size and are
// not portable across machines of different endianness.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace collatz::step2::cache {

/// Resolves the cache directory: $COLLATZ_CACHE_DIR if set (and non-empty), else
/// %LOCALAPPDATA%\collatz. Returns "" when neither is available (then all cache
/// operations become no-ops / misses). Creates the directory if it is missing,
/// and deletes the <cache file>.tmp.<pid> files of writers that died before
/// their rename (pid no longer running, file unchanged for 10 minutes).
[[nodiscard]] std::string resolve_dir();

/// Fingerprint of this binary's table builder: a hash of the bitvectors and
/// dips it builds for small fixed inputs (computed once per process, a few
/// milliseconds). Every cache file stores the fingerprint of its writer, and a
/// file with another fingerprint is never loaded.
[[nodiscard]] std::uint64_t builder_fingerprint();

/// True when cache mappings get an OS read-ahead hint (PrefetchVirtualMemory):
/// COLLATZ_MMAP_WILLNEED is not "0". Logged as
/// `mmap_prefetch=` on the `gpu startup:` line.
[[nodiscard]] bool mmap_prefetch_enabled();

/// Key for the packed-bitvector cache. fmin is a function of m_bits; it is
/// stored in the header and re-validated on load.
struct BvKey {
    unsigned B = 0;
    unsigned m_bits = 0;
    unsigned nbv = 0;
    unsigned fmin = 0;
    unsigned tw = 0; ///< window_chains t_w, part of the dip version (0 = no chain terms).
};

/// Zero-copy load: a live read-only mapping of a plain packed-bitvector cache
/// file (bv_..._v4.bin) plus spans into it, so the multi-GB payload is used in
/// place instead of being copied into owned vectors (a copy means a 2 GiB
/// zero-fill and memcpy at B=31). `keepalive` holds the OS mapping open for the
/// object's lifetime; `bv[i]` points at bitvector i's `words` u64 inside the
/// mapping. `inv3f` is recomputed from (fmin, B); `popcount` is copied out.
/// A mapped file can make a concurrent writer's replace fail; that cache write
/// is then skipped.
struct MappedBitvectors {
    std::shared_ptr<void> keepalive;      ///< holds the mapping and its handles open.
    std::vector<const std::uint64_t*> bv; ///< nbv pointers into the mapping.
    std::uint64_t words = 0;              ///< u64 words per bitvector (ceil(2^B/64)).
    std::vector<std::uint64_t> inv3f;     ///< nbv (copied; tiny).
    std::vector<std::uint64_t> popcount;  ///< nbv (copied; tiny).
    bool ok = false;
};

/// Maps the plain bitvector cache file (bv_..._v4.bin) for `key` read-only
/// and returns spans into it (see MappedBitvectors). Validates the header, key
/// fields, builder fingerprint, exact size, the hash of the whole payload and
/// the stored inv3f; on any miss/error returns `ok == false`.
[[nodiscard]] MappedBitvectors map_bitvectors(const std::string& dir, const BvKey& key);

/// Writes the packed bitvectors for `key` into `dir` atomically (temp file +
/// rename). Best effort: returns true on success, false on any error (the caller
/// ignores the result; after a failed write the next run rebuilds).
bool store_bitvectors(const std::string& dir,
                      const BvKey& key,
                      const std::vector<std::vector<std::uint64_t>>& bvs,
                      const std::vector<std::uint64_t>& inv3f,
                      const std::vector<std::uint64_t>& popcount);

/// Loads the 2^B-entry uint16 dip table for depth `B` and window-chain
/// tag `tw` (part of the dip version; tw = 0 is the dip without chain terms). On a
/// validated hit fills `dip` (size 2^B) and returns true; else returns false.
[[nodiscard]] bool
load_dip(const std::string& dir, unsigned B, unsigned tw, std::vector<std::uint16_t>& dip);

/// Writes the 2^B-entry uint16 dip table for depth `B` and window-chain tag `tw`
/// atomically. Best effort.
bool store_dip(const std::string& dir,
               unsigned B,
               unsigned tw,
               const std::vector<std::uint16_t>& dip);

/// The dip table in class-major layout, `cm[((r mod 2^L) << H) | (r >> L)]
/// == dip(r)` with L = B - H. Same file as store_dip / load_dip (the file stays
/// in natural residue order and byte-identical); these two convert on the fly
/// in bounded parallel chunks, so neither ever holds a second 2^B table.
[[nodiscard]] bool load_dip_classmajor(const std::string& dir,
                                       unsigned B,
                                       unsigned tw,
                                       unsigned H,
                                       unsigned threads,
                                       std::uint16_t* cm);
/// Best effort and never throws: any failure (I/O, bad_alloc, thread spawn)
/// returns false and leaves no temp file behind.
bool store_dip_classmajor(const std::string& dir,
                          unsigned B,
                          unsigned tw,
                          unsigned H,
                          unsigned threads,
                          const std::uint16_t* cm);

} // namespace collatz::step2::cache

#endif // COLLATZ_STEP2_CACHE_HPP
