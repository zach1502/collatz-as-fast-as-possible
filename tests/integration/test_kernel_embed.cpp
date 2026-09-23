#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
// Byte-identity check for the embedded GPU kernel sources.
//
// The CMake codegen (cmake/EmbedKernelSource.cmake + cmake/kernel_sources.hpp.in)
// replaces each part include of a source with the part's bytes and splits the
// result into <= 16 KiB raw-string pieces joined at runtime, so no single string
// literal approaches the 65536-byte -Woverlength-strings limit. The test reads
// each source and its parts from disk (paths supplied by CMake), expands the part
// includes itself and checks that the joined, embedded std::string equals the
// result exactly, and that the parts are every file of their directory in name
// order. An edit that corrupts the split or the expansion, or that embeds a single
// over-length literal, is caught here.

#include "collatz/gpu/kernel_sources.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// `text` without carriage returns. A checkout with CRLF line ends (git autocrlf on
// Windows) changes only the line ends of the files, and compilers read a raw
// string literal with LF line ends, so the comparison ignores them.
std::string lf(std::string text) {
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot open kernel source file: " << path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return lf(ss.str());
}

// The text of `path` with each line `#include "<prefix>NAME"` replaced by the
// bytes of `part_dir`/NAME, as collatz_embed_source_expr does with PART_DIR and
// PART_PREFIX. Appends each included NAME to `names`.
std::string expand_parts(const std::string& path,
                         const std::string& part_dir,
                         const std::string& prefix,
                         std::vector<std::string>& names) {
    const std::string text = read_file(path);
    const std::string head = "#include \"" + prefix;
    std::string out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::size_t end = nl == std::string::npos ? text.size() : nl + 1;
        const std::string line = text.substr(pos, end - pos);
        if (line.rfind(head, 0) == 0) {
            const std::size_t close = line.find('"', head.size());
            REQUIRE_MESSAGE(close != std::string::npos, "malformed part include: " << line);
            names.push_back(line.substr(head.size(), close - head.size()));
            out += read_file(part_dir + "/" + names.back());
        } else {
            out += line;
        }
        pos = end;
    }
    return out;
}

// The names of the files in `dir` with extension `ext`, sorted.
std::vector<std::string> part_files(const std::string& dir, const std::string& ext) {
    std::vector<std::string> out;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ext) {
            out.push_back(entry.path().filename().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

TEST_CASE("embedded OpenCL kernel sources are byte-identical to their files") {
    using namespace collatz::gpu::kernels;
    const std::string dir = COLLATZ_KERNEL_SRC_DIR;
    const std::string limbs = COLLATZ_U128_LIMBS_PATH;
    // The limb header names its parts by their path below include/.
    const std::string include_dir =
        std::filesystem::path(limbs).parent_path().parent_path().parent_path().string();

    std::vector<std::string> limb_parts;
    const std::string limb_dir = include_dir + "/collatz/gpu/u128_limbs";
    CHECK(lf(std::string(u128_limbs_h)) ==
          expand_parts(limbs, limb_dir, "collatz/gpu/u128_limbs/", limb_parts));
    CHECK(!limb_parts.empty());
    CHECK(limb_parts == part_files(limb_dir, ".h"));

    CHECK(lf(std::string(survivor_tail16_cl)) == read_file(dir + "/survivor_tail16.cl"));

    std::vector<std::string> cpi_parts;
    const std::string cpi_dir = dir + "/survivor_cpi";
    CHECK(lf(std::string(survivor_cpi_cl)) ==
          expand_parts(dir + "/survivor_cpi.cl", cpi_dir, "survivor_cpi/", cpi_parts));
    CHECK(!cpi_parts.empty());
    CHECK(cpi_parts == part_files(cpi_dir, ".cl"));
}
