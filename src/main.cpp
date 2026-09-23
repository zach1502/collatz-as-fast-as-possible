#include "collatz/cli.hpp"

// Entry point of the collatz CLI. Subcommands register themselves with
// COLLATZ_REGISTER_COMMAND (src/cli/*.cpp) and are dispatched by name, with no
// external argument-parsing library. With no command, or an unknown one,
// dispatch prints help listing every registered command.

int main(int argc, char** argv) {
    return collatz::cli::dispatch(argc, argv);
}
