# Registers the integration tests that run on the GPU or check its kernels.

# The tests that link the OpenCL backend exist only when it is built.
if(COLLATZ_OPENCL)
    # The GPU verifier (verify_gpu) against the CPU verifier. Each needs the gpu
    # lock; the ones that load a B=24 filter need the bvcache fixture, and those that
    # run the GPU path several times get headroom above the 10 s unit default for a
    # handful of GPU dispatches under `ctest -j` contention.
    collatz_add_test(integration/test_cpu_gpu_agreement.cpp LABEL unit OPENCL GPU TIMEOUT 60)
    # The excursion the GPU path records against the CPU and the definition:
    # random small blocks, and a full run (B=24 tables) whose list holds
    # preflight numbers.
    collatz_add_test(integration/test_gpu_excursion.cpp LABEL unit OPENCL GPU BVCACHE TIMEOUT 300)
    collatz_add_test(integration/test_gpu_batch_stress.cpp LABEL unit OPENCL GPU BVCACHE TIMEOUT 90)
    # Without the fixture the device-mask split test builds the cold dip under ASan
    # while holding the gpu lock, stalling every other GPU test behind it.
    collatz_add_test(integration/test_gpu_bv_split.cpp LABEL unit OPENCL GPU BVCACHE TIMEOUT 60)
    # The pipeline sweep and the injection sweep each run as their own entry.
    collatz_add_test(integration/test_gpu_host_pipeline.cpp LABEL unit OPENCL GPU BVCACHE TIMEOUT 90
        SHARDS
            "pipeline=*in-flight pipeline is bit-identical*"
            "inject=*self-check injections*")
    collatz_add_test(integration/test_gpu_host_pipeline_pack.cpp LABEL unit OPENCL GPU BVCACHE TIMEOUT 90)
    collatz_add_test(integration/test_gpu_producer.cpp LABEL unit OPENCL GPU TIMEOUT 60)
    # Failures of the device path: injected OpenCL and producer errors end the run
    # with an exception, and setup failures fall back to the CPU.
    collatz_add_test(integration/test_gpu_errors.cpp LABEL unit OPENCL GPU BVCACHE TIMEOUT 90)
    # One entry per self-check sweep; the longest is the wgscan mask case.
    collatz_add_test(integration/test_gpu_selfcheck.cpp LABEL unit OPENCL GPU BVCACHE TIMEOUT 90
        SHARDS
            "wgscan=*work-group-aggregated mask scatter*"
            "dropwg=*dropped mask work-group*"
            "split=*split table tail*"
            "inflight=*two-deep in-flight pipeline*")
    # The cases are spread over these files of one executable, so the shards above
    # select from all of them.
    target_sources(test_integration_gpu_selfcheck PRIVATE
        integration/test_gpu_selfcheck/gpu_selfcheck_02_cpi_transfer.cpp
        integration/test_gpu_selfcheck/gpu_selfcheck_03_mask_workgroups.cpp
        integration/test_gpu_selfcheck/gpu_selfcheck_04_tail_pipeline.cpp)
    # The GPU block against the CPU block at K=32/36: the gpu lock, the B=24 tables
    # from the bvcache fixture, and room for the CPU reference runs on a loaded
    # machine.
    collatz_add_test(integration/test_gpu_frontier_blocks.cpp LABEL property OPENCL GPU BVCACHE TIMEOUT 300
        PROCESSORS 4
        SHARDS
            "k32=*K=32 blocks from*"
            "k36=*K=36 block*"
            "overflow=*128-bit overflow*"
            "records=*record list below*"
            "slices=*case slices*"
            "inject=*injected self-check*")
    # GPU against CPU at A=13 and A=14 with the default flush cap, where a batch
    # without the 2^(32-A) survivor cap overflows the 32-bit candidate ids. One
    # entry per A, so a device that cannot run A=14 reports only that one as
    # skipped.
    collatz_add_test(integration/test_gpu_wide_a.cpp LABEL property OPENCL GPU BVCACHE TIMEOUT 120
        PROCESSORS 4
        SHARDS
            "a13=*A=13*"
            "a14=*A=14*")

    # The kernels embedded by the build (cmake/kernel_sources.hpp.in) are
    # byte-identical to their source files. Runs without a GPU device (pure string
    # comparison).
    collatz_add_test(integration/test_kernel_embed.cpp LABEL unit OPENCL)
    target_compile_definitions(test_integration_kernel_embed PRIVATE
        COLLATZ_KERNEL_SRC_DIR="${PROJECT_SOURCE_DIR}/src/gpu/kernels"
        COLLATZ_U128_LIMBS_PATH="${PROJECT_SOURCE_DIR}/include/collatz/gpu/u128_limbs.h")
endif()

# `verify --progress-file` through the binary at N=36 on the CPU and with --gpu
# (B=24 dip from the bvcache fixture); the GPU entry holds the gpu lock, and its
# name keeps it out of the `quick` label.
collatz_add_test(integration/test_progress_file.cpp LABEL unit BVCACHE TIMEOUT 120 PROCESSORS 4
    SHARDS "gpu=*verify --progress-file with --gpu*")
set_tests_properties(test_integration_progress_file.gpu PROPERTIES RESOURCE_LOCK gpu)
if(TARGET collatz)
    target_compile_definitions(test_integration_progress_file PRIVATE
        COLLATZ_BINARY="$<TARGET_FILE:collatz>")
    add_dependencies(test_integration_progress_file collatz)
endif()
