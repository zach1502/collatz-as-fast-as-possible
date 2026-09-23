#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
// The order in which --gpu tries the OpenCL devices and gpu-info lists them,
// and the COLLATZ_GPU_DEVICE parser (src/gpu/device_order.hpp). Header-only, so
// it runs without an OpenCL device.

#include "device_order.hpp"

#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <vector>

using collatz::gpu::device_order;
using collatz::gpu::DeviceRankKey;
using collatz::gpu::parse_device_request;

namespace {

constexpr std::uint64_t kGiB = std::uint64_t{1} << 30;

DeviceRankKey gpu(bool unified, std::uint64_t mem_gib, bool available = true) {
    return DeviceRankKey{true, available, unified, mem_gib * kGiB};
}

DeviceRankKey cpu_device(std::uint64_t mem_gib) {
    return DeviceRankKey{false, true, true, mem_gib * kGiB};
}

} // namespace

TEST_CASE("device order: one GPU is the only candidate, first in the list") {
    CHECK(device_order({gpu(true, 24)}) == std::vector<std::size_t>{0});
    // A CPU device listed first by its platform stays behind the GPU.
    CHECK(device_order({cpu_device(64), gpu(false, 8)}) == std::vector<std::size_t>{1, 0});
}

TEST_CASE("device order: discrete before integrated, then the larger memory") {
    // An integrated GPU with more (shared) memory still comes after a discrete one.
    const std::vector<DeviceRankKey> devs = {
        gpu(true, 32), gpu(false, 8), gpu(false, 12), cpu_device(64), gpu(true, 16)};
    CHECK(device_order(devs) == std::vector<std::size_t>{2, 1, 0, 4, 3});
}

TEST_CASE("device order: unavailable GPUs and other devices go last, in enumeration order") {
    const std::vector<DeviceRankKey> devs = {
        gpu(false, 16, false), cpu_device(8), gpu(true, 4), gpu(false, 8, false)};
    const std::vector<std::size_t> order = device_order(devs);
    CHECK(order == std::vector<std::size_t>{2, 0, 1, 3});
    CHECK(collatz::gpu::gpu_candidate(devs[order[0]]));
    CHECK_FALSE(collatz::gpu::gpu_candidate(devs[order[1]]));
}

TEST_CASE("device order: equal devices keep their enumeration order") {
    CHECK(device_order({gpu(false, 8), gpu(false, 8), gpu(false, 8)}) ==
          std::vector<std::size_t>{0, 1, 2});
    CHECK(device_order({}).empty());
}

TEST_CASE("COLLATZ_GPU_DEVICE: unset, an index, and values that name no index") {
    CHECK_FALSE(parse_device_request(nullptr).set);
    CHECK_FALSE(parse_device_request("").set);
    const auto one = parse_device_request("1");
    CHECK(one.set);
    CHECK(one.valid);
    CHECK(one.index == 1u);
    CHECK(parse_device_request("12").index == 12u);
    for (const char* bad : {"-1", " 1", "1 ", "x", "1x", "+2", "99999999999999999999999"}) {
        INFO(bad);
        const auto r = parse_device_request(bad);
        CHECK(r.set);
        CHECK_FALSE(r.valid);
    }
}
