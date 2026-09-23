#ifndef COLLATZ_CLI_CMD_CERTIFY_INTERNAL_HPP
#define COLLATZ_CLI_CMD_CERTIFY_INTERNAL_HPP

// Private declarations shared by the src/cli/cmd_certify*.cpp pieces of
// `collatz certify`: the parsed arguments and the functions the pieces call
// across files.

#include "collatz/certify.hpp"

#include <string>

namespace collatz::cli::cmd_certify {

// Parsed values that are not certify::Options fields.
struct CertifyArgs {
    bool have_N = false;
    bool have_B = false;
    collatz::certify::AuditOptions audit;
    bool have_seed = false;
    bool audit_only = false; ///< an option that applies only to an audit was given
    bool cache_only = false; ///< --audit-cache-only
    bool help = false;       ///< --help or -h
    std::string result_path;
    std::string json_path;
};

// Defined in cmd_certify.cpp.
void print_usage();

// Defined in cmd_certify_args.cpp.
int parse_certify_args(int argc, char** argv, collatz::certify::Options& opts, CertifyArgs& ca);
int check_certify_args(collatz::certify::Options& opts, const CertifyArgs& ca);
void apply_certify_defaults(collatz::certify::Options& opts, const CertifyArgs& ca);

// Defined in cmd_certify_audit.cpp.
int run_audit(collatz::certify::Options& o,
              collatz::certify::AuditOptions& ao,
              const std::string& result_path,
              const std::string& json_path);

// Defined in cmd_certify_result.cpp.
[[nodiscard]] bool load_run_result(const std::string& path,
                                   const collatz::certify::Options& o,
                                   collatz::certify::AuditOptions& ao,
                                   std::string& digest,
                                   std::string& err);

} // namespace collatz::cli::cmd_certify

#endif // COLLATZ_CLI_CMD_CERTIFY_INTERNAL_HPP
