// Owner-only directories and files, the OS random source, and the Windows path
// and program-lookup helpers that the other src/core/worker*.cpp pieces call.

#include "collatz/worker.hpp"

#include "worker_internal.hpp"
#include "worker_platform.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <system_error>

namespace collatz::worker {

namespace fs = std::filesystem;
using namespace detail;

namespace detail {

/// ASCII form of a wide path: as is when ASCII, else its 8.3 short name; empty
/// when neither is ASCII. The verify child reads its arguments through the
/// narrow (ANSI) argv, so paths handed to it must be ASCII.
std::string ascii_path(const std::wstring& w) {
    const auto narrow = [](const std::wstring& s, std::string& out) {
        out.clear();
        for (const wchar_t c : s) {
            if (c >= 0x80) {
                return false;
            }
            out += static_cast<char>(c);
        }
        return true;
    };
    std::string out;
    if (narrow(w, out)) {
        return out;
    }
    const DWORD n = GetShortPathNameW(w.c_str(), nullptr, 0);
    if (n == 0) {
        return {};
    }
    std::wstring s(n, L'\0');
    const DWORD m = GetShortPathNameW(w.c_str(), s.data(), n);
    if (m == 0 || m >= n) {
        return {};
    }
    s.resize(m);
    return narrow(s, out) ? out : std::string();
}

std::wstring widen(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

/// Security attributes that give the owner and SYSTEM full access and inherit
/// nothing from the parent; a directory passes the same ACEs on to what is
/// created in it. get() is null when the descriptor cannot be built, and the
/// object is then created with the inherited ACL.
class PrivateAcl {
public:
    explicit PrivateAcl(bool directory) {
        const wchar_t* sddl =
            directory ? L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;OW)" : L"D:P(A;;FA;;;SY)(A;;FA;;;OW)";
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl, SDDL_REVISION_1, &sd_, nullptr) == 0) {
            sd_ = nullptr;
        }
        sa_.nLength = sizeof(sa_);
        sa_.lpSecurityDescriptor = sd_;
        sa_.bInheritHandle = FALSE;
    }
    ~PrivateAcl() {
        if (sd_ != nullptr) {
            LocalFree(sd_);
        }
    }
    PrivateAcl(const PrivateAcl&) = delete;
    PrivateAcl& operator=(const PrivateAcl&) = delete;
    SECURITY_ATTRIBUTES* get() {
        return sd_ != nullptr ? &sa_ : nullptr;
    }

private:
    PSECURITY_DESCRIPTOR sd_ = nullptr;
    SECURITY_ATTRIBUTES sa_{};
};

/// Full name of program `name` for CreateProcessW's lpApplicationName, which
/// keeps CreateProcessW from searching the current directory: `name` itself
/// when it has a directory part, else the first of <system directory>\name and
/// the absolute PATH entries that holds it. ".exe" is appended to a name
/// without an extension. Empty when not found.
std::wstring find_program(const std::wstring& name) {
    fs::path file(name);
    if (!file.has_extension()) {
        file += L".exe";
    }
    if (name.find_first_of(L"\\/:") != std::wstring::npos) {
        return file.wstring();
    }
    const auto found = [&file](const fs::path& dir, std::wstring& out) {
        std::error_code ec;
        const fs::path p = dir / file;
        if (!fs::is_regular_file(p, ec)) {
            return false;
        }
        out = p.wstring();
        return true;
    };
    std::wstring out;
    wchar_t sys[MAX_PATH];
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n > 0 && n < MAX_PATH && found(fs::path(std::wstring(sys, n)), out)) {
        return out;
    }
    const wchar_t* env = _wgetenv(L"PATH");
    const std::wstring list = env != nullptr ? env : L"";
    std::size_t pos = 0;
    while (pos < list.size()) {
        std::size_t end = list.find(L';', pos);
        if (end == std::wstring::npos) {
            end = list.size();
        }
        std::wstring dir = list.substr(pos, end - pos);
        if (dir.size() >= 2 && dir.front() == L'"' && dir.back() == L'"') {
            dir = dir.substr(1, dir.size() - 2);
        }
        // A relative entry (".", "bin") would resolve against the current directory.
        if (!dir.empty() && fs::path(dir).is_absolute() && found(fs::path(dir), out)) {
            return out;
        }
        pos = end + 1;
    }
    return {};
}

/// Creates `p` exclusively (CREATE_NEW, so a file or link already there makes
/// it fail) and owner-only (PrivateAcl) and writes `content`; `sync` also
/// flushes it to disk.
bool create_private_file(const fs::path& p,
                         const std::string& content,
                         bool sync,
                         std::string& err) {
    PrivateAcl acl(false);
    HANDLE h = CreateFileW(
        p.c_str(), GENERIC_WRITE, 0, acl.get(), CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = "cannot create " + p.string() + " (Windows error " + std::to_string(GetLastError()) +
              ")";
        return false;
    }
    DWORD written = 0;
    bool ok =
        WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) != 0 &&
        written == content.size();
    ok = (!sync || FlushFileBuffers(h) != 0) && ok;
    ok = CloseHandle(h) != 0 && ok;
    if (!ok) {
        err = "cannot write " + p.string();
        std::error_code ec;
        fs::remove(p, ec);
    }
    return ok;
}

} // namespace detail

bool make_private_dirs(const fs::path& dir, std::string& err) {
    std::vector<fs::path> missing;
    std::error_code ec;
    for (fs::path d = dir; !d.empty() && !fs::exists(d, ec); d = d.parent_path()) {
        missing.push_back(d);
        if (d == d.parent_path()) {
            break;
        }
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        PrivateAcl acl(true);
        if (CreateDirectoryW(it->c_str(), acl.get()) == 0) {
            const DWORD e = GetLastError();
            if (e != ERROR_ALREADY_EXISTS) {
                err =
                    "cannot create " + it->string() + " (Windows error " + std::to_string(e) + ")";
                return false;
            }
        }
    }
    return true;
}

bool random_bytes(unsigned char* out, std::size_t n) noexcept {
    while (n > 0) {
        const ULONG chunk = static_cast<ULONG>(std::min<std::size_t>(n, 1u << 20));
        // 0 is STATUS_SUCCESS (BCRYPT_SUCCESS is a C-style cast, which -Wold-style-cast rejects).
        if (BCryptGenRandom(nullptr, out, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            return false;
        }
        out += chunk;
        n -= chunk;
    }
    return true;
}

} // namespace collatz::worker
