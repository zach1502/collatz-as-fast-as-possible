# Registers the CPU integration tests: pins, oracles, sieves, certify, ref/, block mode.

# Regression pins and end-to-end results of the CPU verifier. The B=24 runs need
# the bvcache fixture: their 2^24 dip table takes about 100 s to build cold in
# an ASan+UBSan build, and TIMEOUT 450 leaves room for a cold build in
# case the best-effort warm step could not run, while staying bounded so a
# deadlock in a sanitizer build still fails.
collatz_add_test(integration/test_pins.cpp LABEL property BVCACHE TIMEOUT 450)
collatz_add_test(integration/test_tail_pins.cpp LABEL unit BVCACHE TIMEOUT 120)
collatz_add_test(integration/test_verify_oracle.cpp LABEL property PROCESSORS 4)

# Native arithmetic and the linear verifier against the GMP oracle.
# test_integration_oracle is a single-threaded, CPU-bound 10^6-seed x
# up-to-64-step native-vs-GMP lockstep differential (20,000 seeds under
# sanitizers, scale.hpp). Under `ctest -j 4` on a 4-core machine
# its thread is time-sliced, which brings it close to the 60 s property default.
# A lock would not help since it contends only for CPU, so it gets TIMEOUT 180,
# which still fails a hang within 3 minutes.
collatz_add_test(integration/test_oracle.cpp LABEL property TIMEOUT 180)
collatz_add_test(integration/test_u128_domain.cpp LABEL property)
# test_u128_domain_kernels.cpp is a second source of this executable, so this
# CTest entry runs the cases of both files.
target_sources(test_integration_u128_domain PRIVATE integration/test_u128_domain_kernels.cpp)
collatz_add_test(integration/test_linear_oracle.cpp LABEL property PROCESSORS 4)

# The sieves against GMP: Step-1 survivors, the Step-2 counts and masks, and the
# GMP certificate of every exclusion.
collatz_add_test(integration/test_step1_oracle.cpp LABEL property PROCESSORS 4)
collatz_add_test(integration/test_step2_counts.cpp LABEL property BVCACHE TIMEOUT 450 PROCESSORS 4)
# The wide-A sweep loads its Filter from the ctest-scoped bv cache and is split
# into one entry per TEST_CASE, so its entries co-schedule freely. The three GMP
# soundness entries walk their sample on up to 4 threads (soundness_workers()),
# hence the PROCESSORS 4 hint on those.
collatz_add_test(integration/test_step2_wide_a.cpp LABEL property BVCACHE TIMEOUT 450
    SHARDS
        "defn_a8=wide-A mask (A=8)*"
        "defn_a12=wide-A mask (A=12)*"
        "defn_a14=wide-A mask (A=14)*"
        "sound_a8=Theorem 4.4 soundness at A=8*"
        "sound_a12=Theorem 4.4 soundness at A=12*"
        "sound_a14=Theorem 4.4 soundness at A=14*")
set_tests_properties(test_integration_step2_wide_a.sound_a8 test_integration_step2_wide_a.sound_a12
    test_integration_step2_wide_a.sound_a14 PROPERTIES PROCESSORS 4)
collatz_add_test(integration/test_exclusion_certificates.cpp LABEL property BVCACHE TIMEOUT 450)
# A second source of the same executable and CTest entry.
target_sources(test_integration_exclusion_certificates PRIVATE
    integration/test_exclusion_certificates_partial_oee.cpp)

# `collatz certify` re-derives the exclusions in GMP: full runs and block audits.
collatz_add_test(integration/test_certify_full.cpp LABEL property BVCACHE TIMEOUT 450)
collatz_add_test(integration/test_certify_audit.cpp LABEL property BVCACHE TIMEOUT 450)

# The production pipeline against the independent ref/ implementation.
collatz_add_test(integration/test_ref_step1_recount.cpp LABEL property)
collatz_add_test(integration/test_ref_step1_survivors.cpp LABEL property)
collatz_add_test(integration/test_ref_partial_oee.cpp LABEL property
    SHARDS "m24=*at M=24 (sampled)*")
collatz_add_test(integration/test_ref_random.cpp LABEL property)

# Frontier block mode, property tier: test_frontier_blocks.cpp brute-forces every
# number of blocks up to 2^26 wide and runs the B=24 H=0 pins, _records checks
# the slice merge and the record list, and _certify runs the B=24 block `certify`
# audits, all with the bvcache fixture. Each file is split per TEST_CASE so the
# entries co-schedule.
collatz_add_test(integration/test_frontier_blocks.cpp LABEL property BVCACHE TIMEOUT 300 PROCESSORS 4
    SHARDS
        "pins=*H=0 is the full run*"
        "model=*small blocks match*"
        "compose=*compose with the brute-force*"
        "union=*two half blocks*")
collatz_add_test(integration/test_frontier_blocks_records.cpp LABEL property BVCACHE TIMEOUT 300
    PROCESSORS 4
    SHARDS
        "slices=*case slices*"
        "overflow=*128-bit overflow*"
        "records=*record list*"
        "reccap=*record cap*")
collatz_add_test(integration/test_frontier_blocks_certify.cpp LABEL property BVCACHE TIMEOUT 300
    PROCESSORS 4
    SHARDS
        "certify=*certify audits a block*"
        "certneg=*certify catches*")
