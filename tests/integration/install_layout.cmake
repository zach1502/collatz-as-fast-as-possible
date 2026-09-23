# CTest test_integration_install_layout (tests/CMakeLists.txt), a CMake script:
# `cmake --install BUILD_DIR --prefix PREFIX` must install EXPECTED
# (bin/collatz) and no other file, so an install carries the verifier and none
# of the test framework. PREFIX is removed before and after the check.

file(REMOVE_RECURSE "${PREFIX}")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${PREFIX}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "cmake --install failed (exit ${rc}):\n${out}${err}")
endif()
file(GLOB_RECURSE installed RELATIVE "${PREFIX}" "${PREFIX}/*")
list(SORT installed)
if(NOT installed STREQUAL "${EXPECTED}")
    message(FATAL_ERROR "cmake --install put [${installed}] under ${PREFIX}; expected [${EXPECTED}]")
endif()
message(STATUS "cmake --install put only ${EXPECTED} under the prefix")
file(REMOVE_RECURSE "${PREFIX}")
