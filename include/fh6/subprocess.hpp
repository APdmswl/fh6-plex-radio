#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace fh6::subprocess {

std::wstring widen(std::string_view s);
std::string narrow(std::wstring_view ws);
std::wstring quote(const std::wstring& s);

HANDLE open_nul(DWORD access);
HANDLE open_stderr_log();
std::filesystem::path stderr_log_path();

HANDLE create_kill_on_close_job();
const wchar_t* safe_spawn_cwd();

// CREATE_SUSPENDED + AssignProcessToJobObject + ResumeThread so fast children
// cannot spawn descendants outside the job before we attach them. If PATH
// lookup fails once, refresh PATH from HKLM/HKCU registry values and retry.
HANDLE spawn_in_job(HANDLE job, const std::wstring& cmd, HANDLE stdin_h, HANDLE stdout_h,
                    HANDLE stderr_h);

} // namespace fh6::subprocess
