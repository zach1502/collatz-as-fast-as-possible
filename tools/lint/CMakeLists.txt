# Style checks, label `lint`: `ctest --test-dir build/windows-release -L lint`. Each
# test fails on any finding ("enforce" is true in tools/lint/config.json)
# above the counts in tools/lint/baseline.json, which is empty. lint_checker
# runs the unit tests of the checker and of lint_all.py (the test_*.py modules),
# lint_style runs check_style.py --check over the tree, and lint_tools runs
# clang-format, ESLint, ruff, shellcheck and PSScriptAnalyzer through
# lint_all.py. lint_tools never fetches a tool (--offline) and writes its raw
# output into this build directory; a tool that is not installed, or that uvx
# has not cached, is skipped, and -DCOLLATZ_LINT_STRICT=ON makes such a skip
# fail the test.
option(COLLATZ_LINT_STRICT "lint_tools fails when a style tool is skipped" OFF)
find_program(PYTHON3_EXECUTABLE NAMES python3)
if(PYTHON3_EXECUTABLE)
    add_test(NAME lint_checker
        COMMAND ${PYTHON3_EXECUTABLE} -m unittest
            test_check_style_rules test_check_style_lexers test_check_style_baseline
            test_lint_all
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME lint_style
        COMMAND ${PYTHON3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/check_style.py --check
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set(_lint_tools_args --skip check_style --offline --out-dir ${CMAKE_CURRENT_BINARY_DIR})
    if(COLLATZ_LINT_STRICT)
        list(APPEND _lint_tools_args --strict)
    endif()
    add_test(NAME lint_tools
        COMMAND ${PYTHON3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/lint_all.py ${_lint_tools_args}
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(lint_checker lint_style PROPERTIES LABELS lint TIMEOUT 120)
    set_tests_properties(lint_tools PROPERTIES LABELS lint TIMEOUT 600)
endif()
