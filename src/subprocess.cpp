#include "fh6/subprocess.hpp"

namespace fh6::subprocess {

namespace {

std::wstring read_registry_path(HKEY root, const wchar_t* subkey) {
    DWORD bytes           = 0;
    constexpr DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
    if (RegGetValueW(root, subkey, L"Path", flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t))
        return {};

    std::wstring raw(bytes / sizeof(wchar_t), L'\0');
    DWORD type = 0;
    if (RegGetValueW(root, subkey, L"Path", flags, &type, raw.data(), &bytes) != ERROR_SUCCESS)
        return {};
    while (!raw.empty() && raw.back() == L'\0') raw.pop_back();
    if (type != REG_EXPAND_SZ || raw.empty()) return raw;

    DWORD need = ExpandEnvironmentStringsW(raw.c_str(), nullptr, 0);
    if (need == 0) return raw;
    std::wstring expanded(need, L'\0');
    DWORD got = ExpandEnvironmentStringsW(raw.c_str(), expanded.data(), need);
    if (got == 0 || got > need) return raw;
    expanded.resize(got - 1);
    return expanded;
}

bool refresh_path_from_registry() {
    auto sys = read_registry_path(
        HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment");
    auto usr = read_registry_path(HKEY_CURRENT_USER, L"Environment");
    if (sys.empty() && usr.empty()) return false;

    std::wstring merged = std::move(sys);
    if (!usr.empty()) {
        if (!merged.empty() && merged.back() != L';') merged += L';';
        merged += usr;
    }

    DWORD cur_len = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (cur_len > 1) {
        std::wstring current(cur_len, L'\0');
        DWORD written = GetEnvironmentVariableW(L"PATH", current.data(), cur_len);
        if (written > 0 && written < cur_len) {
            current.resize(written);
            if (current == merged) return false;
        }
    }
    return SetEnvironmentVariableW(L"PATH", merged.c_str()) != 0;
}

} // namespace

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

std::wstring quote(const std::wstring& s) {
    if (s.empty()) return L"\"\"";
    if (s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring out{L"\""};
    for (auto c : s) {
        if (c == L'"') out += L'\\';
        out += c;
    }
    out += L'"';
    return out;
}

HANDLE open_nul(DWORD access) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE h = CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                           0, nullptr);
    return h == INVALID_HANDLE_VALUE ? nullptr : h;
}

std::filesystem::path stderr_log_path() {
    return std::filesystem::temp_directory_path() / "fh6-plex-stderr.log";
}

HANDLE open_stderr_log() {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE h = CreateFileW(stderr_log_path().wstring().c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    return h == INVALID_HANDLE_VALUE ? open_nul(GENERIC_WRITE) : h;
}

const wchar_t* safe_spawn_cwd() {
    static const std::wstring dir = [] {
        wchar_t buf[MAX_PATH];
        UINT n = GetSystemDirectoryW(buf, MAX_PATH);
        return (n > 0 && n < MAX_PATH) ? std::wstring{buf, n} : std::wstring{};
    }();
    return dir.empty() ? nullptr : dir.c_str();
}

HANDLE create_kill_on_close_job() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

HANDLE spawn_in_job(HANDLE job, const std::wstring& cmd, HANDLE stdin_h, HANDLE stdout_h,
                    HANDLE stderr_h) {
    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = stdin_h;
    si.hStdOutput = stdout_h;
    si.hStdError  = stderr_h;

    PROCESS_INFORMATION pi{};
    std::wstring mut;
    auto launch = [&] {
        mut = cmd;
        return CreateProcessW(nullptr, mut.data(), nullptr, nullptr, TRUE,
                              CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, safe_spawn_cwd(), &si,
                              &pi) != 0;
    };
    if (!launch()) {
        const DWORD ec = GetLastError();
        const bool stale_path_miss =
            (ec == ERROR_FILE_NOT_FOUND || ec == ERROR_PATH_NOT_FOUND) &&
            refresh_path_from_registry();
        if (!stale_path_miss || !launch()) {
            SetLastError(ec);
            return nullptr;
        }
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

} // namespace fh6::subprocess
