#ifndef COLLATZ_CLI_HPP
#define COLLATZ_CLI_HPP

// Self-registering subcommand framework for the `collatz` CLI.
//
// Each subcommand is a translation unit under src/cli/ that registers itself
// during static initialization through COLLATZ_REGISTER_COMMAND. Those TUs are
// compiled into the `collatz` executable, not archived into a static library,
// so the linker keeps their registrars.
//
// A command is one file, src/cli/cmd_<name>.cpp, with a `run` function and a
// COLLATZ_REGISTER_COMMAND line; CMake and main.cpp need no change (see
// CONTRIBUTING.md).

namespace collatz::cli {

// A CLI subcommand. `run` receives argv shifted so that argv[0] is the command
// name and argv[1..] are its arguments.
struct Command {
    const char* name;
    const char* help;
    int (*run)(int argc, char** argv);
};

// Registers a command. The registry is a function-local static vector (see
// src/cli/registry.cpp), so registration does not depend on the
// static-initialization order of translation units.
void register_command(const Command& cmd);

// Runs the registered command that argv names. With no command, or an unknown
// one, prints help listing every registered command and returns nonzero.
int dispatch(int argc, char** argv);

} // namespace collatz::cli

// Defines a static registrar whose constructor registers the command at
// program startup. The anonymous namespace gives each TU's registrar internal
// linkage.
#define COLLATZ_REGISTER_COMMAND(cmd_name, cmd_help, cmd_fn)                                       \
    namespace {                                                                                    \
    struct cmd_fn##_Registrar {                                                                    \
        cmd_fn##_Registrar() {                                                                     \
            ::collatz::cli::register_command({cmd_name, cmd_help, cmd_fn});                        \
        }                                                                                          \
    } cmd_fn##_registrar_instance;                                                                 \
    } // namespace

#endif // COLLATZ_CLI_HPP
