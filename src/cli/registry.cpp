#include "collatz/cli.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

// Command registry and dispatch for the self-registering CLI; see collatz/cli.hpp.

namespace collatz::cli {

namespace {

// A function-local static is constructed on first use, so it exists when the
// registrars in other translation units run during static initialization. A
// namespace-scope vector might not be constructed yet at that point.
std::vector<Command>& registry() {
    static std::vector<Command> commands;
    return commands;
}

int print_help(const char* prog) {
    std::fprintf(stderr, "usage: %s <command> [args]\n", prog);
    for (const Command& c : registry()) {
        std::fprintf(stderr, "  %-15s %s\n", c.name, c.help);
    }
    return 2;
}

} // namespace

void register_command(const Command& cmd) {
    registry().push_back(cmd);
}

int dispatch(int argc, char** argv) {
    const char* prog = (argc > 0) ? argv[0] : "collatz";
    if (argc < 2) {
        return print_help(prog);
    }
    const char* cmd = argv[1];
    for (const Command& c : registry()) {
        if (std::strcmp(c.name, cmd) == 0) {
            // Shift argv so the command sees argv[0] == its own name.
            return c.run(argc - 1, argv + 1);
        }
    }
    std::fprintf(stderr, "error: unknown command '%s'\n", cmd);
    return print_help(prog);
}

} // namespace collatz::cli
