# Writes the git commit of a source tree to a header, as one line
#   #define COLLATZ_GIT_COMMIT "<value>"
# collatz_git_commit_stamp() (GitCommitStamp.cmake) runs it on every build:
#   cmake -DSOURCE_DIR=<dir> -DOUTPUT=<file> [-DGIT_EXECUTABLE=<git>] -P WriteGitCommitStamp.cmake
#
# The value is one of
#   <sha>        the full SHA of HEAD, when SOURCE_DIR is the top of a git checkout,
#                no tracked file differs from HEAD (in the index or the work tree)
#                and no untracked, non-ignored file is in src/, include/, ref/ or
#                cmake/ or is CMakeLists.txt (CMakeLists.txt globs new sources in);
#   <sha>-dirty  when git reports such a file, or its status check fails;
#   <sha>-nogit  when git cannot run (not found, or it refuses the checkout) and the
#                commit is read from .git directly; the tree was not checked;
#   unknown      outside a git checkout: a source tarball, also one unpacked inside
#                another repository, whose commit says nothing about this tree.
# OUTPUT is rewritten only when the value changes, so an unchanged value recompiles
# nothing. A change of content counts; touching a file does not.

cmake_minimum_required(VERSION 3.25)

if(NOT SOURCE_DIR OR NOT OUTPUT)
    message(FATAL_ERROR
        "usage: cmake -DSOURCE_DIR=<dir> -DOUTPUT=<file> [-DGIT_EXECUTABLE=<git>] -P ${CMAKE_CURRENT_LIST_FILE}")
endif()

# git finds the checkout from SOURCE_DIR alone, also when the build runs from a git
# hook or a shell that sets these. With GIT_OPTIONAL_LOCKS=0, `git status` does not
# take index.lock, which would make a git command running at the same time fail.
foreach(_var GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_COMMON_DIR)
    unset(ENV{${_var}})
endforeach()
set(ENV{GIT_OPTIONAL_LOCKS} 0)

# Sets <out_var> to TRUE if <text> is a SHA as git prints it: 40 lowercase hex
# digits, or 64 in a SHA-256 repository.
function(_stamp_is_sha text out_var)
    string(LENGTH "${text}" _len)
    set(_ok FALSE)
    if(text MATCHES "^[0-9a-f]+$" AND (_len EQUAL 40 OR _len EQUAL 64))
        set(_ok TRUE)
    endif()
    set(${out_var} ${_ok} PARENT_SCOPE)
endfunction()

# Runs `git -C SOURCE_DIR <args>`; sets <out_var> to its standard output without
# trailing whitespace and <rc_var> to its exit code, which is an error text when git
# cannot be started.
function(_stamp_git out_var rc_var)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" ${ARGN}
        OUTPUT_VARIABLE _out
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _rc)
    set(${out_var} "${_out}" PARENT_SCOPE)
    set(${rc_var} "${_rc}" PARENT_SCOPE)
endfunction()

# Sets <out_var> to the SHA of HEAD read from SOURCE_DIR/.git without git, or to ""
# when it cannot be read (no .git, an unborn branch, a reftable repository). .git is
# a directory, or in a worktree or submodule a file "gitdir: <dir>"; a worktree's
# <dir>/commondir names the directory that holds the branch refs.
function(_stamp_read_git_dir out_var)
    set(${out_var} "" PARENT_SCOPE)
    set(_gitdir "${SOURCE_DIR}/.git")
    if(NOT EXISTS "${_gitdir}")
        return()
    endif()
    if(NOT IS_DIRECTORY "${_gitdir}")
        file(READ "${_gitdir}" _link)
        if(NOT _link MATCHES "^gitdir: *([^\r\n]+)")
            return()
        endif()
        set(_gitdir "${CMAKE_MATCH_1}")
        if(NOT IS_ABSOLUTE "${_gitdir}")
            set(_gitdir "${SOURCE_DIR}/${_gitdir}")
        endif()
    endif()
    set(_common "${_gitdir}")
    if(EXISTS "${_gitdir}/commondir")
        file(READ "${_gitdir}/commondir" _common)
        string(STRIP "${_common}" _common)
        if(NOT IS_ABSOLUTE "${_common}")
            set(_common "${_gitdir}/${_common}")
        endif()
    endif()
    if(NOT EXISTS "${_gitdir}/HEAD" OR IS_DIRECTORY "${_gitdir}/HEAD")
        return()
    endif()
    file(READ "${_gitdir}/HEAD" _sha)
    string(STRIP "${_sha}" _sha)
    if(_sha MATCHES "^ref: (.+)$")
        set(_ref "${CMAKE_MATCH_1}")
        set(_sha "")
        if(EXISTS "${_common}/${_ref}" AND NOT IS_DIRECTORY "${_common}/${_ref}")
            file(READ "${_common}/${_ref}" _sha)
            string(STRIP "${_sha}" _sha)
        elseif(EXISTS "${_common}/packed-refs")
            # Lines "<sha> <ref>"; the ref is compared as text, not as a regex.
            file(STRINGS "${_common}/packed-refs" _lines)
            foreach(_line IN LISTS _lines)
                if(_line MATCHES "^([0-9a-f]+) (.+)$" AND CMAKE_MATCH_2 STREQUAL _ref)
                    set(_sha "${CMAKE_MATCH_1}")
                    break()
                endif()
            endforeach()
        endif()
    endif()
    _stamp_is_sha("${_sha}" _ok)
    if(_ok)
        set(${out_var} "${_sha}" PARENT_SCOPE)
    endif()
endfunction()

set(_stamp "")
if(GIT_EXECUTABLE)
    _stamp_git(_prefix _prefix_rc rev-parse --show-prefix)
    if(_prefix_rc EQUAL 0 AND NOT _prefix STREQUAL "")
        # SOURCE_DIR lies below the top of another checkout.
        set(_stamp "unknown")
    elseif(_prefix_rc EQUAL 0)
        _stamp_git(_sha _sha_rc rev-parse --verify HEAD)
        _stamp_is_sha("${_sha}" _sha_ok)
        if(_sha_rc EQUAL 0 AND _sha_ok)
            _stamp_git(_tracked _tracked_rc status --porcelain --untracked-files=no)
            _stamp_git(_untracked _untracked_rc status --porcelain --untracked-files=all
                -- src include ref cmake CMakeLists.txt)
            set(_stamp "${_sha}")
            if(NOT _tracked_rc EQUAL 0 OR NOT _untracked_rc EQUAL 0
                    OR NOT _tracked STREQUAL "" OR NOT _untracked STREQUAL "")
                string(APPEND _stamp "-dirty")
            endif()
        endif()
    endif()
endif()
if(_stamp STREQUAL "")
    _stamp_read_git_dir(_sha)
    if(_sha)
        set(_stamp "${_sha}-nogit")
    else()
        set(_stamp "unknown")
    endif()
endif()

set(_text "// Written by cmake/WriteGitCommitStamp.cmake on every build; do not edit.\n")
string(APPEND _text "#define COLLATZ_GIT_COMMIT \"${_stamp}\"\n")
set(_old "")
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" _old)
endif()
if(NOT _old STREQUAL _text)
    file(WRITE "${OUTPUT}" "${_text}")
    message(STATUS "git commit of ${SOURCE_DIR}: ${_stamp}")
endif()
