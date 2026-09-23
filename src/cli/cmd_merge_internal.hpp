#ifndef COLLATZ_CLI_CMD_MERGE_INTERNAL_HPP
#define COLLATZ_CLI_CMD_MERGE_INTERNAL_HPP

// Private declarations shared by the src/cli/cmd_merge*.cpp pieces of
// `collatz merge`: the report lines printed for the merged Result.

#include "collatz/checkpoint.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"

namespace collatz::cli::cmd_merge {

// Defined in cmd_merge_print.cpp.
void print_merged_counts(unsigned N, const collatz::verify::Result& r);
void print_merged_max_exact(unsigned N,
                            collatz::u128 block_h,
                            const collatz::checkpoint::MergeInfo& mi,
                            const collatz::verify::Result& r);
void print_merged_records(const collatz::verify::Options& o, const collatz::verify::Result& r);

} // namespace collatz::cli::cmd_merge

#endif // COLLATZ_CLI_CMD_MERGE_INTERNAL_HPP
