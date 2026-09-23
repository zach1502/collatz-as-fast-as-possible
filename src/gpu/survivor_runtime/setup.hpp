#ifndef COLLATZ_GPU_SURVIVOR_RUNTIME_SETUP_HPP
#define COLLATZ_GPU_SURVIVOR_RUNTIME_SETUP_HPP

// SurvivorRunner::Setup, the steps of SurvivorRunner::create, and the knob and
// device-query helpers they share. The steps are defined in survivor_runtime.cpp
// and survivor_runtime_create.cpp.

#include "survivor_runtime/internal.hpp"
#include "tail_defaults.hpp"

#include <cstdlib>
#include <memory>
#include <string>

namespace collatz::gpu {

namespace survivor_detail {

// Boolean env knob: unset or empty gives `dflt`, a leading '0' gives false.
[[nodiscard]] inline bool env_flag(const char* name, bool dflt) {
    if (const char* e = std::getenv(name); e != nullptr && e[0] != '\0') {
        return e[0] != '0';
    }
    return dflt;
}

template <typename T>
[[nodiscard]] T dev_scalar(cl_device_id dev, cl_device_info param) {
    T value{};
    clGetDeviceInfo(dev, param, sizeof(T), &value, nullptr);
    return value;
}

} // namespace survivor_detail

// The steps of SurvivorRunner::create, in the order create runs them; each
// fills its part of `runner`. build_on runs them all on one device.
struct SurvivorRunner::Setup {
    static std::unique_ptr<SurvivorRunner>
    build_on(cl_device_id dev, const TailHint& hint, std::string& why);
    static void device_info(SurvivorRunner& runner, cl_device_id chosen_dev);
    static void queues(SurvivorRunner& runner, cl_device_id chosen_dev);
    static std::string build_tail16(SurvivorRunner& runner, cl_device_id chosen_dev);
    static void tail_variant(SurvivorRunner& runner, cl_device_id chosen_dev, bool u64n);
    static void create_cpi_program(SurvivorRunner& runner);
    static std::string build_cpi(SurvivorRunner& runner, cl_device_id chosen_dev);
    static void cpi_kernels(SurvivorRunner& runner, cl_device_id chosen_dev);
    static void tables(SurvivorRunner& runner);
    static void dispatch_settings(SurvivorRunner& runner);
};

} // namespace collatz::gpu

#endif // COLLATZ_GPU_SURVIVOR_RUNTIME_SETUP_HPP
