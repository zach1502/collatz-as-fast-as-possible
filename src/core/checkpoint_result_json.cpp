// The `verify --json` document (result_json) and the JSON string escape.

#include "checkpoint_internal.hpp"
#include "collatz/checkpoint.hpp"
#include "collatz/oracle.hpp"
#include "collatz/threading.hpp"

#include <cstdarg>
#include <cstdio>

namespace collatz::checkpoint {

using namespace detail;

// JSON writer. checkpoint::parse_slice and the work server (server/src/result)
// read this format.
namespace detail {

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (const char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                // Other control characters as \u00XX (read back by the parser).
                static constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out += kHex[(static_cast<unsigned char>(c) >> 4) & 0x0Fu];
                out += kHex[static_cast<unsigned char>(c) & 0x0Fu];
            } else {
                out += c;
            }
        }
    }
    return out;
}

} // namespace detail

namespace {

// Appends printf output to a std::string. On MinGW the format attribute uses
// the archetype that its <stdio.h> declares printf with (ms_printf or
// gnu_printf, by runtime).
#if defined(__MINGW32__) && defined(__MINGW_PRINTF_FORMAT)
__attribute__((format(__MINGW_PRINTF_FORMAT, 2, 3)))
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void appendf(std::string& out, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (static_cast<std::size_t>(n) < sizeof(buf)) {
        out.append(buf, static_cast<std::size_t>(n));
        return;
    }
    std::string big(static_cast<std::size_t>(n) + 1, '\0');
    va_start(ap, fmt);
    std::vsnprintf(big.data(), big.size(), fmt, ap);
    va_end(ap);
    big.resize(static_cast<std::size_t>(n));
    out += big;
}

/// Decimal of (h + add) * 2^K in exact arithmetic.
std::string block_bound_decimal(u128 h, unsigned add, unsigned K) {
    mpz_class v = oracle::to_mpz(h);
    v += add;
    mpz_mul_2exp(v.get_mpz_t(), v.get_mpz_t(), K);
    return oracle::to_decimal(v);
}

} // namespace

std::string result_json(const JsonMeta& meta,
                        const verify::Options& opts,
                        const verify::Result& r,
                        const std::string& extra_block) {
    std::string f;
    const auto u = [](std::uint64_t x) { return static_cast<unsigned long long>(x); };
    const unsigned depth =
        meta.unwalked_depth ? *meta.unwalked_depth : verify::unwalked_depth(opts);
    appendf(f, "{\n");
    appendf(f, "  \"schema\": \"collatz-verify-json/1\",\n");
    appendf(f, "  \"commit\": \"%s\",\n", json_escape(meta.commit).c_str());
    appendf(f,
            "  \"machine\": {\"host\": \"%s\", \"hardware_concurrency\": %u},\n",
            json_escape(host_name()).c_str(),
            collatz::hardware_threads());
    appendf(f, "  \"backend\": \"%s\",\n", json_escape(meta.backend).c_str());
    // The checked range [L, end) = [h*2^K, (h+1)*2^K); h = 0 is a full run.
    appendf(f,
            "  \"block\": {\"h\": \"%s\", \"K\": %u, \"L\": \"%s\", \"end\": \"%s\"},\n",
            collatz::to_string(opts.block_h).c_str(),
            opts.N,
            block_bound_decimal(opts.block_h, 0, opts.N).c_str(),
            block_bound_decimal(opts.block_h, 1, opts.N).c_str());
    appendf(f, "  \"params\": {\n");
    appendf(f,
            "    \"N\": %u, \"A\": %u, \"B\": %u, \"bitvectors\": %u, \"threads\": %u,\n",
            opts.N,
            opts.A,
            opts.B,
            opts.num_bitvectors,
            opts.threads);
    appendf(f,
            "    \"kernel\": \"table16\", \"mod3t\": %u, \"partial_oee\": %s, \"gpu\": %s,\n",
            opts.step1.mod3t,
            opts.partial_oee ? "true" : "false",
            meta.use_gpu ? "true" : "false");
    appendf(f,
            "    \"block_h\": \"%s\", \"window_chains\": %u, \"window_templates_D\": %u,\n",
            collatz::to_string(opts.block_h).c_str(),
            opts.window_chains,
            opts.window_templates_D);
    // The hypothesis a block result is conditional on: every n < assumes_below
    // converges, by assumption_source.
    appendf(f,
            "    \"assumes_below\": \"%s\", \"assumption_source\": \"%s\",\n",
            verify::assumes_below(opts).c_str(),
            json_escape(verify::assumption_source(opts)).c_str());
    if (opts.record_threshold.empty()) {
        appendf(f, "    \"record_threshold\": null,\n");
    } else {
        appendf(
            f, "    \"record_threshold\": \"%s\",\n", json_escape(opts.record_threshold).c_str());
    }
    // The work server requires params.window_landing and accepts a result only
    // when it is false (server/src/result/check.ts); this binary always writes false.
    appendf(f, "    \"window_landing\": false,\n");
    // The un-walked depth K behind max_exact, so that a merge recomputes
    // exactness from the recorded depth instead of assuming M+B.
    appendf(f, "    \"unwalked_depth\": %u,\n", depth);
    appendf(f,
            "    \"case_c\": %llu, \"case_k\": %llu\n  },\n",
            u(opts.step1.case_c),
            u(opts.step1.case_k));
    appendf(f, "  \"result\": {\n");
    appendf(f, "    \"candidates_checked\": %llu,\n", u(r.candidates_checked));
    appendf(f, "    \"step2_masked_out\": %llu,\n", u(r.step2_masked_out));
    appendf(f, "    \"step3_steps_total\": %llu,\n", u(r.step3_steps_total));
    appendf(f, "    \"coverage_checksum\": %llu,\n", u(r.coverage_checksum));
    appendf(f, "    \"pow_checksum\": %llu,\n", u(r.pow_checksum));
    appendf(f, "    \"pow_available\": %s,\n", r.pow_available ? "true" : "false");
    appendf(f, "    \"pow_kernel_id\": %u,\n", r.pow_kernel_id);
    appendf(f, "    \"blocks_total\": %llu,\n", u(r.blocks_total));
    // Exact decimal, also above 2^128 (max_excursion_wide).
    appendf(f, "    \"max_excursion\": \"%s\",\n", verify::max_excursion_decimal(r).c_str());
    appendf(f, "    \"max_excursion_n\": \"%s\",\n", collatz::to_string(r.max_excursion_n).c_str());
    appendf(f, "    \"max_exceeds_u128\": %s,\n", r.max_exceeds_u128 ? "true" : "false");
    appendf(f, "    \"max_exact\": %s,\n", r.max_exact ? "true" : "false");
    appendf(f, "    \"records\": [");
    for (std::size_t i = 0; i < r.records.size(); ++i) {
        appendf(f,
                "%s[\"%s\", \"%s\"]",
                i == 0 ? "" : ", ",
                collatz::to_string(r.records[i].n).c_str(),
                r.records[i].peak.c_str());
    }
    appendf(f, "],\n");
    appendf(f, "    \"records_truncated\": %s,\n", r.records_truncated ? "true" : "false");
    appendf(f, "    \"overflow_fallbacks\": %llu,\n", u(r.overflow_fallbacks));
    appendf(f, "    \"prefix_walks\": %llu,\n", u(r.prefix_walks));
    appendf(f, "    \"step1_survivors\": %llu,\n", u(r.step1.survivors));
    appendf(f, "    \"seconds_step1_2\": %.6f,\n", r.seconds_step1_2);
    appendf(f, "    \"seconds_filter\": %.6f,\n", r.seconds_filter);
    appendf(f, "    \"bv_cache_hit\": %s,\n", r.bv_cache_hit ? "true" : "false");
    appendf(f, "    \"seconds_total\": %.6f,\n", r.seconds_total);
    appendf(f, "    \"result_digest_schema\": \"%s\",\n", kResultDigestSchema);
    appendf(f, "    \"result_digest\": \"%s\"\n  },\n", result_digest(opts, depth, r).c_str());
    appendf(f, "  \"preflight\": {\n");
    appendf(f, "    \"candidates\": %llu,\n", u(r.preflight_candidates));
    appendf(f, "    \"coverage_checksum\": %llu,\n", u(r.preflight_coverage));
    appendf(f, "    \"pow_checksum\": %llu,\n", u(r.preflight_pow));
    appendf(f, "    \"blocks_total\": %llu,\n", u(r.preflight_blocks));
    appendf(f, "    \"step3_steps_total\": %llu\n  }", u(r.preflight_step3_steps));
    if (r.gpu_selfcheck_on) {
        appendf(f,
                ",\n  \"gpu_selfcheck\": {\"batches\": %llu, \"retried\": %llu, "
                "\"cpu_fallback\": %llu, \"unchecked\": %llu}",
                u(r.gpu_selfcheck_batches),
                u(r.gpu_selfcheck_retried),
                u(r.gpu_selfcheck_cpu_fallback),
                u(r.gpu_selfcheck_unchecked));
    }
    if (!extra_block.empty()) {
        f += ",\n  ";
        f += extra_block;
    }
    appendf(f, "\n}\n");
    return f;
}

} // namespace collatz::checkpoint
