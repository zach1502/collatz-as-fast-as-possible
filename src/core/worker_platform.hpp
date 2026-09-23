#ifndef COLLATZ_CORE_WORKER_PLATFORM_HPP
#define COLLATZ_CORE_WORKER_PLATFORM_HPP

// System headers of the src/core/worker*.cpp pieces that call the OS: Win32,
// BCrypt and SDDL.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>

#endif // COLLATZ_CORE_WORKER_PLATFORM_HPP
