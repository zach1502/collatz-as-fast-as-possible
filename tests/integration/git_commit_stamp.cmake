# Test of cmake/WriteGitCommitStamp.cmake on scratch git repositories (a CMake
# script, so it needs no shell or Python). Invoked by ctest as
#   cmake -DGIT_EXECUTABLE=<git> -DSCRIPT=<WriteGitCommitStamp.cmake> -DWORK=<dir> -P git_commit_stamp.cmake
#
# It checks each value the script writes: <sha> for a clean checkout, and also after
# a touch; <sha>-dirty for a changed, staged or untracked source and a changed
# tracked file anywhere, but not for an untracked file outside the source
# directories or an ignored one; <sha>-nogit without git (loose and packed refs, a
# detached HEAD, a worktree); unknown for an unborn branch and for a tarball, also
# one unpacked inside another repository. The header is rewritten only when the
# value changes.

cmake_minimum_required(VERSION 3.25)
if(NOT SCRIPT OR NOT WORK)
    message(FATAL_ERROR "need -DGIT_EXECUTABLE=... -DSCRIPT=... -DWORK=...")
endif()
if(NOT GIT_EXECUTABLE)
    message("git is not available; skipped")
    return()
endif()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/no-hooks")
# git stops looking for a repository at WORK, so a tarball below it is outside any
# checkout wherever the build directory is.
set(ENV{GIT_CEILING_DIRECTORIES} "${WORK}")

# Runs git in <dir> without the user's hooks, signing or identity.
function(g dir)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${dir}"
            -c "core.hooksPath=${WORK}/no-hooks" -c commit.gpgsign=false -c core.autocrlf=false
            -c user.name=stamp-test -c user.email=stamp-test@example.invalid ${ARGN}
        OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "git ${ARGN} in ${dir} failed (${_rc}): ${_err}")
    endif()
    set(GIT_OUT "${_out}" PARENT_SCOPE)
endfunction()

# Runs the script on <dir> with git <git> ("" for none) into <header>; sets STAMP to
# the value written and STAMP_LOG to the script's output.
function(stamp_into header dir git)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${dir}" "-DGIT_EXECUTABLE=${git}"
            "-DOUTPUT=${header}" -P "${SCRIPT}"
        OUTPUT_VARIABLE _log ERROR_VARIABLE _err RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "the script failed on ${dir} (${_rc}): ${_err}")
    endif()
    file(READ "${header}" _text)
    if(NOT _text MATCHES "\n#define COLLATZ_GIT_COMMIT \"([^\"]*)\"\n$")
        message(FATAL_ERROR "unexpected header for ${dir}:\n${_text}")
    endif()
    set(STAMP "${CMAKE_MATCH_1}" PARENT_SCOPE)
    set(STAMP_LOG "${_log}" PARENT_SCOPE)
endfunction()

# Fails unless the script gives <expected> for <dir> with git <git>.
function(expect what dir git expected)
    stamp_into("${WORK}/stamp.hpp" "${dir}" "${git}")
    if(NOT STAMP STREQUAL expected)
        message(FATAL_ERROR "${what}: expected '${expected}', got '${STAMP}'")
    endif()
    message(STATUS "ok: ${what}: ${STAMP}")
endfunction()

set(R "${WORK}/repo")
file(WRITE "${R}/CMakeLists.txt" "project(p)\n")
file(WRITE "${R}/src/a.cpp" "int a;\n")
file(WRITE "${R}/include/a.hpp" "int b;\n")
file(WRITE "${R}/docs/readme.txt" "docs\n")
file(WRITE "${R}/.gitignore" "*.o\n")
g("${WORK}" init -q -b main "${R}")
g("${R}" add -A)
g("${R}" commit -q --no-verify -m one)
g("${R}" rev-parse HEAD)
set(SHA "${GIT_OUT}")

expect("clean checkout" "${R}" "${GIT_EXECUTABLE}" "${SHA}")
file(APPEND "${R}/src/a.cpp" "// edit\n")
expect("edited tracked source" "${R}" "${GIT_EXECUTABLE}" "${SHA}-dirty")
g("${R}" add src/a.cpp)
expect("staged edit" "${R}" "${GIT_EXECUTABLE}" "${SHA}-dirty")
g("${R}" checkout -q HEAD -- src/a.cpp)
expect("edit undone by checkout" "${R}" "${GIT_EXECUTABLE}" "${SHA}")
file(TOUCH "${R}/src/a.cpp" "${R}/include/a.hpp")
expect("touched sources" "${R}" "${GIT_EXECUTABLE}" "${SHA}")
file(APPEND "${R}/docs/readme.txt" "more\n")
expect("edited tracked file outside the sources" "${R}" "${GIT_EXECUTABLE}" "${SHA}-dirty")
g("${R}" checkout -q -- docs/readme.txt)
foreach(_new src/new.cpp include/collatz/new.hpp ref/new.cpp cmake/New.cmake)
    file(WRITE "${R}/${_new}" "new\n")
    expect("untracked ${_new}" "${R}" "${GIT_EXECUTABLE}" "${SHA}-dirty")
    file(REMOVE "${R}/${_new}")
endforeach()
file(WRITE "${R}/docs/notes.txt" "notes\n")
file(WRITE "${R}/src/a.o" "object\n")
expect("untracked file outside the sources, ignored file in src" "${R}" "${GIT_EXECUTABLE}" "${SHA}")
file(REMOVE "${R}/docs/notes.txt" "${R}/src/a.o")

# The header is rewritten only when the value changes.
set(H "${WORK}/rewrite.hpp")
stamp_into("${H}" "${R}" "${GIT_EXECUTABLE}")
if(NOT STAMP_LOG MATCHES "git commit of")
    message(FATAL_ERROR "the first run did not write ${H}")
endif()
stamp_into("${H}" "${R}" "${GIT_EXECUTABLE}")
if(STAMP_LOG MATCHES "git commit of")
    message(FATAL_ERROR "an unchanged value rewrote ${H}: ${STAMP_LOG}")
endif()
file(APPEND "${R}/src/a.cpp" "// edit\n")
stamp_into("${H}" "${R}" "${GIT_EXECUTABLE}")
if(NOT STAMP_LOG MATCHES "git commit of" OR NOT STAMP STREQUAL "${SHA}-dirty")
    message(FATAL_ERROR "a changed value did not rewrite ${H} (${STAMP})")
endif()
g("${R}" checkout -q -- src/a.cpp)
message(STATUS "ok: the header is rewritten only when the value changes")

# Without git: the commit read from .git, through a loose ref, a missing executable,
# a detached HEAD and packed refs.
expect("no git, loose ref" "${R}" "" "${SHA}-nogit")
expect("git executable missing" "${R}" "${WORK}/no-such-git" "${SHA}-nogit")
set(W "${WORK}/worktree")
g("${R}" worktree add -q -b side "${W}")
expect("worktree" "${W}" "${GIT_EXECUTABLE}" "${SHA}")
file(APPEND "${W}/src/a.cpp" "// edit\n")
expect("edited worktree" "${W}" "${GIT_EXECUTABLE}" "${SHA}-dirty")
g("${W}" checkout -q -- src/a.cpp)
expect("worktree, no git" "${W}" "" "${SHA}-nogit")
g("${R}" pack-refs --all)
if(EXISTS "${R}/.git/refs/heads/main")
    message(FATAL_ERROR "pack-refs left refs/heads/main loose")
endif()
expect("no git, packed ref" "${R}" "" "${SHA}-nogit")
expect("worktree, no git, packed ref" "${W}" "" "${SHA}-nogit")
g("${R}" checkout -q --detach)
expect("no git, detached HEAD" "${R}" "" "${SHA}-nogit")

# Outside a checkout.
set(U "${WORK}/unborn")
g("${WORK}" init -q -b main "${U}")
expect("unborn branch" "${U}" "${GIT_EXECUTABLE}" "unknown")
expect("unborn branch, no git" "${U}" "" "unknown")
foreach(_dir "${WORK}/tarball" "${WORK}/outer/tarball")
    file(COPY "${R}/CMakeLists.txt" "${R}/src" "${R}/include" DESTINATION "${_dir}")
endforeach()
expect("tarball" "${WORK}/tarball" "${GIT_EXECUTABLE}" "unknown")
expect("tarball, no git" "${WORK}/tarball" "" "unknown")
# A repository that ignores everything, like a home directory kept in git: its
# commit would otherwise pass as this tree's, with nothing to show as changed.
file(WRITE "${WORK}/outer/.gitignore" "*\n")
g("${WORK}" init -q -b main "${WORK}/outer")
g("${WORK}/outer" add -f .gitignore)
g("${WORK}/outer" commit -q --no-verify -m outer)
expect("tarball inside another repository" "${WORK}/outer/tarball" "${GIT_EXECUTABLE}" "unknown")

file(REMOVE_RECURSE "${WORK}")
message(STATUS "all git commit stamp cases passed")
