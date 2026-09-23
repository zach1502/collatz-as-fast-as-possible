#ifndef COLLATZ_WORKER_HPP
#define COLLATZ_WORKER_HPP

// Client side of the work-unit protocol (`collatz worker`; the server is in
// server/): the config file with the host id, the worker token file, the
// lease a unit is handed out with, the verify command line for it, the
// result-digest check, a subprocess runner and HTTP through the system curl
// executable (no TLS library is linked).
//
// C++20, with the Win32 API (MinGW-w64) for the process, lock and path
// functions.

#include "collatz/worker/config.hpp"
#include "collatz/worker/http.hpp"
#include "collatz/worker/json.hpp"
#include "collatz/worker/lease.hpp"
#include "collatz/worker/process.hpp"

#endif // COLLATZ_WORKER_HPP
