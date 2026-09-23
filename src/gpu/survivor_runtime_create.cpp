#include "opencl_runtime.hpp" // gpu_choice
#include "survivor_runtime/setup.hpp"

#include <cstddef>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace collatz::gpu {

using namespace survivor_detail;

// The device limits the runner sizes its buffers and grid from.
void SurvivorRunner::Setup::device_info(SurvivorRunner& runner, cl_device_id chosen_dev) {
    runner.info_.max_mem_alloc_size =
        dev_scalar<cl_ulong>(chosen_dev, CL_DEVICE_MAX_MEM_ALLOC_SIZE);
    runner.info_.global_mem_size = dev_scalar<cl_ulong>(chosen_dev, CL_DEVICE_GLOBAL_MEM_SIZE);
    runner.info_.max_compute_units = dev_scalar<cl_uint>(chosen_dev, CL_DEVICE_MAX_COMPUTE_UNITS);
}

// The context and the in-order queues.
void SurvivorRunner::Setup::queues(SurvivorRunner& runner, cl_device_id chosen_dev) {
    cl_int status = CL_SUCCESS;
    runner.context_ = clCreateContext(nullptr, 1, &chosen_dev, nullptr, nullptr, &status);
    check(status, "clCreateContext");
    // Queues have profiling on so kernel and transfer time can be told apart.
    const cl_command_queue_properties qprops = CL_QUEUE_PROFILING_ENABLE;
    runner.queue_ = clCreateCommandQueue(runner.context_, chosen_dev, qprops, &status);
    check(status, "clCreateCommandQueue");
    // Second in-order queue: the in-flight pipeline's flag-list reads run on
    // it so they do not queue behind the other slot's tail. If the driver
    // refuses a second queue, those reads use queue_.
    {
        cl_int qs2 = CL_SUCCESS;
        runner.queue_alt_ = clCreateCommandQueue(runner.context_, chosen_dev, qprops, &qs2);
        if (qs2 != CL_SUCCESS) {
            runner.queue_alt_ = nullptr;
        }
    }
    // The copy queue (see survivor_runtime/state_transfer.hpp) is a third
    // in-order queue that carries only host->device input copies
    // (prefetch_inputs), never a kernel.
    runner.xfer_.inject_copy = env_flag("COLLATZ_GPU_SELFCHECK_INJECT_COPY", false);
    {
        cl_int qs3 = CL_SUCCESS;
        runner.xfer_.copy_q = clCreateCommandQueue(runner.context_, chosen_dev, qprops, &qs3);
        if (qs3 != CL_SUCCESS) {
            runner.xfer_.copy_q = nullptr; // serial uploads on queue_ (always correct)
        }
    }
}

// Builds a runner on `dev`: the context and queues, both programs and the
// tables. Returns nullptr with `why` set when a build fails or a step throws,
// so that create() can try the next device.
std::unique_ptr<SurvivorRunner>
SurvivorRunner::Setup::build_on(cl_device_id dev, const TailHint& hint, std::string& why) {
    try {
        std::unique_ptr<SurvivorRunner> runner(new SurvivorRunner());
        device_info(*runner, dev);
        queues(*runner, dev);
        if (std::string w = build_tail16(*runner, dev); !w.empty()) {
            why = std::move(w);
            return nullptr;
        }

        // Candidate-per-item (cpi) program; it shares the context, queue, tbl16
        // and bitvectors.
        tail_variant(*runner, dev, tail_u64n_on(hint));
        create_cpi_program(*runner);
        // Test hook: the dropped-work-group injection is compiled in only when
        // requested, so the shipped kernels carry no trace of it.
        runner->sc_inject_dropwg_ = env_flag("COLLATZ_GPU_SELFCHECK_INJECT_DROPWG", false);
        if (std::string w = build_cpi(*runner, dev); !w.empty()) {
            why = std::move(w);
            return nullptr;
        }
        cpi_kernels(*runner, dev);
        tables(*runner);
        dispatch_settings(*runner);
        return runner;
    } catch (const std::exception& e) {
        // OpenCLError from a failed call, std::runtime_error from tail_variant.
        why = e.what();
        return nullptr;
    }
}

// Builds the runner on the first GPU of gpu_choice() that accepts the programs.
// Returns nullptr with *err set when there is no GPU or every one failed.
// `hint` (the value width) turns COLLATZ_GPU_TAIL_U64N off when some n can
// reach 2^64 (see tail_defaults.hpp).
std::unique_ptr<SurvivorRunner> SurvivorRunner::create(std::string* err, const TailHint& hint) {
    const auto set_err = [err](const std::string& msg) {
        if (err != nullptr) {
            *err = msg;
        }
    };
    const GpuChoice choice = gpu_choice();
    if (choice.tries.empty()) {
        set_err(choice.why);
        return nullptr;
    }
    std::string why_all;
    for (std::size_t k = 0; k < choice.tries.size(); ++k) {
        const DeviceInfo& d = choice.tries[k];
        std::string why;
        if (std::unique_ptr<SurvivorRunner> runner = Setup::build_on(d.id, hint, why)) {
            return runner;
        }
        if (k + 1 < choice.tries.size()) {
            std::fprintf(stderr,
                         "gpu: %s failed (%s); trying the next GPU\n",
                         d.device_name.c_str(),
                         why.c_str());
        }
        // A single GPU's reason is passed on as it is.
        if (choice.tries.size() > 1) {
            why = d.device_name + ": " + why;
        }
        why_all += (why_all.empty() ? "" : "; ") + why;
    }
    set_err(why_all);
    return nullptr;
}

} // namespace collatz::gpu
