# collatz_git_commit_stamp(<target>)
#
# Defines COLLATZ_GIT_COMMIT, the git commit of PROJECT_SOURCE_DIR as a string
# literal (WriteGitCommitStamp.cmake lists the values), in every source file of
# <target>. The value is computed at build time: the custom target
# collatz_git_commit runs WriteGitCommitStamp.cmake on every build, before <target>
# compiles, and the script rewrites its header only when the value changes. An
# edit, a commit or a checkout therefore changes the value at the next build
# without a reconfigure, and an unchanged value recompiles nothing. The configure
# step writes the header too, so that clangd and run-clang-tidy find it before the
# first build.
#
# The header reaches the sources through -include (GCC and Clang, the supported
# compilers), so they need no #include of it. git is the executable that
# find_package(Git) finds; without one the script reads .git itself.

find_package(Git QUIET)

function(collatz_git_commit_stamp target)
    set(_header "${PROJECT_BINARY_DIR}/git_commit_generated/collatz_git_commit.hpp")
    if(NOT TARGET collatz_git_commit)
        set(_command "${CMAKE_COMMAND}"
            "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
            "-DOUTPUT=${_header}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/WriteGitCommitStamp.cmake")
        execute_process(COMMAND ${_command} COMMAND_ERROR_IS_FATAL ANY)
        add_custom_target(collatz_git_commit
            COMMAND ${_command}
            BYPRODUCTS "${_header}"
            COMMENT "Checking the git commit of ${PROJECT_SOURCE_DIR}"
            VERBATIM)
    endif()
    add_dependencies(${target} collatz_git_commit)
    target_compile_options(${target} PRIVATE "SHELL:-include \"${_header}\"")
endfunction()
