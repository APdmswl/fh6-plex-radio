#include "fh6/sources/plex_source.hpp"
#include "fh6/log.hpp"

#include <windows.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <utility>

namespace fh6::sources {

namespace {

using json = nlohmann::json;

constexpr std::uint64_t kPcmBytesPerSec = 48000ull * 2ull * 2ull;

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), nullptr, 0,
                                nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), out.data(), n, nullptr, nullptr);
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

std::string trim(std::string s) {
    auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_ws((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && is_ws((unsigned char)s.back())) s.pop_back();
    return s;
}

std::string normalize_server_url(std::string s) {
    s = trim(std::move(s));
    if (s.empty()) return {};
    if (s.find("://") == std::string::npos) s = "https://" + s;
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

bool starts_with_http(std::string_view s) {
    return s.starts_with("http://") || s.starts_with("https://");
}

std::string url_encode(std::string_view s) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string add_token_query(std::string url, std::string_view token) {
    if (token.empty()) return url;
    url += (url.find('?') == std::string::npos) ? '?' : '&';
    url += "X-Plex-Token=";
    url += url_encode(token);
    return url;
}

std::string url_for_path(const PlexConfig& cfg, const std::string& path, bool include_token) {
    std::string url = starts_with_http(path) ? path : normalize_server_url(cfg.server_url) + path;
    return include_token ? add_token_query(std::move(url), cfg.token) : url;
}

HANDLE open_nul(DWORD access) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE h = CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                           0, nullptr);
    return h == INVALID_HANDLE_VALUE ? nullptr : h;
}

HANDLE open_stderr_log() {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    auto path = std::filesystem::temp_directory_path() / "fh6-plex-stderr.log";
    HANDLE h  = CreateFileW(path.wstring().c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    return h == INVALID_HANDLE_VALUE ? open_nul(GENERIC_WRITE) : h;
}

std::filesystem::path stderr_log_path() {
    return std::filesystem::temp_directory_path() / "fh6-plex-stderr.log";
}

std::string win32_message(DWORD ec) {
    LPWSTR raw = nullptr;
    DWORD len  = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                    FORMAT_MESSAGE_IGNORE_INSERTS,
                                nullptr, ec, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                (LPWSTR)&raw, 0, nullptr);
    HMODULE winhttp = nullptr;
    bool free_winhttp = false;
    if ((!raw || !len) && (winhttp = GetModuleHandleW(L"winhttp.dll")) == nullptr) {
        winhttp = LoadLibraryW(L"winhttp.dll");
        free_winhttp = winhttp != nullptr;
    }
    if ((!raw || !len) && winhttp) {
        if (raw) {
            LocalFree(raw);
            raw = nullptr;
        }
        len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             winhttp, ec, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                             (LPWSTR)&raw, 0, nullptr);
    }
    std::string msg;
    if (raw && len) {
        while (len && (raw[len - 1] == L'\r' || raw[len - 1] == L'\n' || raw[len - 1] == L' '))
            --len;
        msg = narrow({raw, len});
    }
    if (raw) LocalFree(raw);
    if (free_winhttp) FreeLibrary(winhttp);
    return msg.empty() ? "unknown" : msg;
}

std::string win32_error(DWORD ec) {
    return "ec=" + std::to_string(ec) + " (" + win32_message(ec) + ")";
}

bool is_ipv4_literal(std::string_view host) {
    if (host.empty() || host.find('.') == std::string_view::npos) return false;
    return std::ranges::all_of(host, [](unsigned char c) {
        return std::isdigit(c) != 0 || c == '.';
    });
}

std::string local_http_url(std::string_view host, INTERNET_PORT port) {
    std::string out = "http://";
    out += host;
    if (port != INTERNET_DEFAULT_HTTP_PORT) {
        out += ":";
        out += std::to_string(port);
    }
    return out;
}

std::string winhttp_hint(DWORD ec, INTERNET_SCHEME scheme, std::wstring_view host_w,
                         INTERNET_PORT port) {
    const auto host = narrow(host_w);
    const bool https = scheme == INTERNET_SCHEME_HTTPS;
    const bool ip_host = is_ipv4_literal(host);
    if (https && ip_host) {
        return " -- HTTPS to an IP address can fail when the Plex certificate is not trusted "
               "by Windows or does not match the IP. For a local Plex server, try " +
               local_http_url(host, port) + ".";
    }
    if (https && ec == ERROR_WINHTTP_SECURE_FAILURE) {
        return " -- Windows rejected the HTTPS certificate. For local Plex, use HTTP or a "
               "trusted HTTPS hostname.";
    }
    if (ec == ERROR_WINHTTP_CANNOT_CONNECT || ec == ERROR_WINHTTP_CONNECTION_ERROR) {
        return " -- could not connect to Plex. Check the server URL and port; local Plex "
               "usually uses http://<server>:32400.";
    }
    if (ec == ERROR_WINHTTP_TIMEOUT) {
        return " -- connection timed out. Check that the Plex server is reachable from this PC.";
    }
    if (ec == ERROR_WINHTTP_NAME_NOT_RESOLVED) {
        return " -- hostname was not resolved. Check the Plex server URL.";
    }
    return {};
}

std::string winhttp_failure(std::string_view op, DWORD ec, INTERNET_SCHEME scheme,
                            std::wstring_view host, INTERNET_PORT port) {
    std::string out{op};
    out += " failed: ";
    out += win32_error(ec);
    out += winhttp_hint(ec, scheme, host, port);
    return out;
}

std::string plex_http_error(DWORD status) {
    if (status == 401) {
        return "Plex HTTP 401 -- token was rejected. Use a Plex token for the account "
               "that can access this server. You can test it with "
               "/library/sections?X-Plex-Token=YOUR_TOKEN on the same server.";
    }
    if (status == 403) {
        return "Plex HTTP 403 -- token is valid but does not have access to this server "
               "or library.";
    }
    return "Plex HTTP " + std::to_string(status);
}

std::string describe_launch_failure(const std::wstring& bin, DWORD ec, bool from_config) {
    wchar_t resolved[MAX_PATH] = {};
    DWORD got = SearchPathW(nullptr, bin.c_str(), L".exe", MAX_PATH, resolved, nullptr);
    std::string where = got ? narrow({resolved, got})
                            : (from_config ? "(configured path not found on disk)"
                                           : "(not found on PATH)");

    const char* hint = "";
    switch (ec) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            hint = from_config ? " -- the path in [plex].ffmpeg_path does not exist."
                               : " -- ffmpeg is not on PATH. Install it or set "
                                 "[plex].ffmpeg_path in config.toml.";
            break;
        case ERROR_ACCESS_DENIED:
            hint = " -- likely blocked by antivirus. Whitelist ffmpeg and the game folder.";
            break;
        case ERROR_BAD_EXE_FORMAT:
            hint = " -- the configured file is not a valid Win64 executable.";
            break;
        default:
            break;
    }

    return win32_error(ec) + " tried=" + narrow(bin) + " resolved=" + where + hint;
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
    std::wstring mut = cmd;
    if (!CreateProcessW(nullptr, mut.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi))
        return nullptr;
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        const DWORD assign_ec = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        SetLastError(assign_ec);
        return nullptr;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

struct InternetHandle {
    HINTERNET h = nullptr;
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET v) : h{v} {}
    ~InternetHandle() {
        if (h) WinHttpCloseHandle(h);
    }
    InternetHandle(const InternetHandle&)            = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    operator HINTERNET() const noexcept { return h; }
};

struct HttpResult {
    DWORD status = 0;
    std::string body;
    std::string error;
};

HttpResult winhttp_get(const PlexConfig& cfg, std::string path) {
    HttpResult out;
    const auto base = normalize_server_url(cfg.server_url);
    if (base.empty()) {
        out.error = "Plex server URL is empty";
        return out;
    }
    if (path.empty() || path.front() != '/') path.insert(path.begin(), '/');
    const auto full = base + path;
    const auto url  = widen(full);

    std::array<wchar_t, 512> host{};
    std::array<wchar_t, 8192> url_path{};
    std::array<wchar_t, 8192> extra{};
    URL_COMPONENTSW uc{};
    uc.dwStructSize      = sizeof(uc);
    uc.lpszHostName      = host.data();
    uc.dwHostNameLength  = (DWORD)host.size();
    uc.lpszUrlPath       = url_path.data();
    uc.dwUrlPathLength   = (DWORD)url_path.size();
    uc.lpszExtraInfo     = extra.data();
    uc.dwExtraInfoLength = (DWORD)extra.size();
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        out.error = "Plex server URL could not be parsed";
        return out;
    }

    std::wstring host_name{uc.lpszHostName, uc.dwHostNameLength};
    std::wstring object{uc.lpszUrlPath, uc.dwUrlPathLength};
    object.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (object.empty()) object = L"/";

    InternetHandle session{WinHttpOpen(L"FH6 Universal Radio/1.0",
                                       WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) {
        out.error = "WinHttpOpen failed: " + win32_message(GetLastError());
        return out;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 8000, 12000);

    InternetHandle conn{WinHttpConnect(session, host_name.c_str(), uc.nPort, 0)};
    if (!conn.h) {
        const DWORD ec = GetLastError();
        out.error = winhttp_failure("WinHttpConnect", ec, uc.nScheme, host_name, uc.nPort);
        return out;
    }

    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    InternetHandle req{WinHttpOpenRequest(conn, L"GET", object.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)};
    if (!req.h) {
        const DWORD ec = GetLastError();
        out.error = winhttp_failure("WinHttpOpenRequest", ec, uc.nScheme, host_name, uc.nPort);
        return out;
    }

    std::wstring headers =
        L"Accept: application/json\r\n"
        L"X-Plex-Accept: application/json\r\n"
        L"X-Plex-Product: FH6 Universal Radio\r\n"
        L"X-Plex-Client-Identifier: fh6-universal-radio-plex\r\n"
        L"X-Plex-Platform: Windows\r\n";
    if (!cfg.token.empty()) {
        headers += L"X-Plex-Token: ";
        headers += widen(cfg.token);
        headers += L"\r\n";
    }

    if (!WinHttpSendRequest(req, headers.c_str(), (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0,
                            0)) {
        const DWORD ec = GetLastError();
        out.error = winhttp_failure("WinHttpSendRequest", ec, uc.nScheme, host_name, uc.nPort);
        return out;
    }
    if (!WinHttpReceiveResponse(req, nullptr)) {
        const DWORD ec = GetLastError();
        out.error = winhttp_failure("WinHttpReceiveResponse", ec, uc.nScheme, host_name,
                                    uc.nPort);
        return out;
    }

    DWORD status_len = sizeof(out.status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &out.status, &status_len,
                        WINHTTP_NO_HEADER_INDEX);

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) {
            const DWORD ec = GetLastError();
            out.error = winhttp_failure("WinHttpQueryDataAvailable", ec, uc.nScheme,
                                        host_name, uc.nPort);
            return out;
        }
        if (avail == 0) break;

        std::string chunk(avail, '\0');
        DWORD total = 0;
        while (total < avail) {
            DWORD got = 0;
            if (!WinHttpReadData(req, chunk.data() + total, avail - total, &got)) {
                const DWORD ec = GetLastError();
                out.error = winhttp_failure("WinHttpReadData", ec, uc.nScheme, host_name,
                                            uc.nPort);
                return out;
            }
            if (got == 0) break;
            total += got;
        }
        chunk.resize(total);
        out.body += chunk;
    }
    if (out.status < 200 || out.status >= 300) {
        out.error = plex_http_error(out.status);
    }
    return out;
}

bool winhttp_download_to_file(const PlexConfig& cfg, std::string path,
                              const std::filesystem::path& out_file, std::string& error) {
    const auto base = normalize_server_url(cfg.server_url);
    if (base.empty()) {
        error = "Plex server URL is empty";
        return false;
    }
    std::string full;
    if (starts_with_http(path)) {
        full = std::move(path);
    } else {
        if (path.empty() || path.front() != '/') path.insert(path.begin(), '/');
        full = base + path;
    }
    const auto url = widen(full);

    std::array<wchar_t, 512> host{};
    std::array<wchar_t, 8192> url_path{};
    std::array<wchar_t, 8192> extra{};
    URL_COMPONENTSW uc{};
    uc.dwStructSize      = sizeof(uc);
    uc.lpszHostName      = host.data();
    uc.dwHostNameLength  = (DWORD)host.size();
    uc.lpszUrlPath       = url_path.data();
    uc.dwUrlPathLength   = (DWORD)url_path.size();
    uc.lpszExtraInfo     = extra.data();
    uc.dwExtraInfoLength = (DWORD)extra.size();
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        error = "Plex URL could not be parsed";
        return false;
    }

    std::wstring host_name{uc.lpszHostName, uc.dwHostNameLength};
    std::wstring object{uc.lpszUrlPath, uc.dwUrlPathLength};
    object.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (object.empty()) object = L"/";

    std::error_code fs_ec;
    std::filesystem::create_directories(out_file.parent_path(), fs_ec);
    std::ofstream os{out_file, std::ios::binary | std::ios::trunc};
    if (!os) {
        error = "Could not create temp audio file: " + out_file.string();
        return false;
    }

    InternetHandle session{WinHttpOpen(L"FH6 Universal Radio/1.0",
                                       WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) {
        error = "WinHttpOpen failed: " + win32_message(GetLastError());
        return false;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 30000);

    InternetHandle conn{WinHttpConnect(session, host_name.c_str(), uc.nPort, 0)};
    if (!conn.h) {
        const DWORD ec = GetLastError();
        error = winhttp_failure("WinHttpConnect", ec, uc.nScheme, host_name, uc.nPort);
        return false;
    }

    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    InternetHandle req{WinHttpOpenRequest(conn, L"GET", object.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)};
    if (!req.h) {
        const DWORD ec = GetLastError();
        error = winhttp_failure("WinHttpOpenRequest", ec, uc.nScheme, host_name, uc.nPort);
        return false;
    }

    std::wstring headers =
        L"Accept: audio/mpeg, audio/*, application/octet-stream, */*\r\n"
        L"X-Plex-Accept: application/json\r\n"
        L"X-Plex-Product: FH6 Universal Radio\r\n"
        L"X-Plex-Client-Identifier: fh6-universal-radio-plex\r\n"
        L"X-Plex-Platform: Windows\r\n";
    if (!cfg.token.empty()) {
        headers += L"X-Plex-Token: ";
        headers += widen(cfg.token);
        headers += L"\r\n";
    }

    if (!WinHttpSendRequest(req, headers.c_str(), (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0,
                            0)) {
        const DWORD ec = GetLastError();
        error = winhttp_failure("WinHttpSendRequest", ec, uc.nScheme, host_name, uc.nPort);
        return false;
    }
    if (!WinHttpReceiveResponse(req, nullptr)) {
        const DWORD ec = GetLastError();
        error = winhttp_failure("WinHttpReceiveResponse", ec, uc.nScheme, host_name,
                                uc.nPort);
        return false;
    }

    DWORD status = 0;
    DWORD status_len = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_len,
                        WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300) {
        error = plex_http_error(status);
        return false;
    }

    std::array<char, 64 * 1024> buf{};
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) {
            const DWORD ec = GetLastError();
            error = winhttp_failure("WinHttpQueryDataAvailable", ec, uc.nScheme, host_name,
                                    uc.nPort);
            return false;
        }
        if (avail == 0) break;
        while (avail > 0) {
            DWORD want = std::min<DWORD>(avail, (DWORD)buf.size());
            DWORD got = 0;
            if (!WinHttpReadData(req, buf.data(), want, &got)) {
                const DWORD ec = GetLastError();
                error = winhttp_failure("WinHttpReadData", ec, uc.nScheme, host_name,
                                        uc.nPort);
                return false;
            }
            if (got == 0) break;
            os.write(buf.data(), got);
            if (!os) {
                error = "Could not write temp audio file";
                return false;
            }
            avail -= got;
        }
    }

    os.flush();
    if (!os) {
        error = "Could not flush temp audio file";
        return false;
    }
    return true;
}

std::string get_string(const json& obj, const char* key) {
    if (!obj.is_object()) return {};
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    if (it->is_number_unsigned()) return std::to_string(it->get<unsigned long long>());
    return {};
}

std::uint64_t get_u64(const json& obj, const char* key) {
    if (!obj.is_object()) return 0;
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return 0;
    try {
        if (it->is_number_unsigned()) return it->get<std::uint64_t>();
        if (it->is_number_integer()) return static_cast<std::uint64_t>(it->get<long long>());
        if (it->is_string()) return static_cast<std::uint64_t>(std::stoull(it->get<std::string>()));
    } catch (...) {}
    return 0;
}

int get_int(const json& obj, const char* key) {
    const auto v = get_u64(obj, key);
    return v > static_cast<std::uint64_t>(INT_MAX) ? INT_MAX : static_cast<int>(v);
}

const json* media_container(const json& root) {
    if (!root.is_object()) return nullptr;
    auto it = root.find("MediaContainer");
    if (it == root.end() || !it->is_object()) return nullptr;
    return &*it;
}

template <class Fn>
void for_each_child(const json& obj, const char* key, Fn fn) {
    if (!obj.is_object()) return;
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return;
    if (it->is_array()) {
        for (const auto& item : *it) fn(item);
    } else if (it->is_object()) {
        fn(*it);
    }
}

const json* first_child(const json& obj, const char* key) {
    if (!obj.is_object()) return nullptr;
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return nullptr;
    if (it->is_array()) return it->empty() ? nullptr : &(*it)[0];
    if (it->is_object()) return &*it;
    return nullptr;
}

bool parse_json_response(const HttpResult& res, json& root, std::string& error) {
    if (!res.error.empty()) {
        error = res.error;
        return false;
    }
    try {
        root = json::parse(res.body);
    } catch (const std::exception& e) {
        error = std::string{"Plex did not return JSON: "} + e.what();
        return false;
    }
    if (!media_container(root)) {
        error = "Plex JSON response did not include MediaContainer";
        return false;
    }
    return true;
}

PlexTrack parse_track(const PlexConfig& cfg, const json& item) {
    PlexTrack t;
    t.key         = get_string(item, "ratingKey");
    t.title       = get_string(item, "title");
    t.artist      = get_string(item, "grandparentTitle");
    t.album       = get_string(item, "parentTitle");
    t.duration_ms = get_u64(item, "duration");

    if (t.artist.empty()) t.artist = get_string(item, "originalTitle");
    if (t.artist.empty()) t.artist = "Plex";

    auto art = get_string(item, "thumb");
    if (art.empty()) art = get_string(item, "parentThumb");
    if (art.empty()) art = get_string(item, "grandparentThumb");
    if (!art.empty()) t.artwork_url = url_for_path(cfg, art, true);

    if (const json* media = first_child(item, "Media")) {
        if (const json* part = first_child(*media, "Part")) {
            t.part_key = get_string(*part, "key");
        }
    }
    return t;
}

std::string metadata_id(const json& item) {
    auto id = get_string(item, "ratingKey");
    if (!id.empty()) return id;

    auto key = get_string(item, "key");
    constexpr std::string_view needle = "/library/metadata/";
    auto pos = key.find(needle);
    if (pos == std::string::npos) return key;
    pos += needle.size();
    auto end = key.find('/', pos);
    return key.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

PlexArtist parse_artist(const json& item) {
    PlexArtist a;
    a.key        = metadata_id(item);
    a.title      = get_string(item, "title");
    a.leaf_count = get_int(item, "leafCount");
    return a;
}

PlexAlbum parse_album(const PlexConfig& cfg, const json& item) {
    PlexAlbum a;
    a.key        = metadata_id(item);
    a.title      = get_string(item, "title");
    a.artist     = get_string(item, "parentTitle");
    a.leaf_count = get_int(item, "leafCount");
    auto art     = get_string(item, "thumb");
    if (art.empty()) art = get_string(item, "parentThumb");
    if (!art.empty()) a.artwork_url = url_for_path(cfg, art, true);
    return a;
}

std::string playlist_items_path(std::string key) {
    if (key.empty()) return {};
    if (key.front() == '/') return key;
    return "/playlists/" + url_encode(key) + "/items";
}

std::string library_artists_path(std::string key) {
    if (key.empty()) return {};
    return "/library/sections/" + url_encode(key) + "/all?type=8";
}

std::string library_albums_path(std::string key) {
    if (key.empty()) return {};
    return "/library/sections/" + url_encode(key) + "/all?type=9";
}

std::string library_tracks_path(std::string key) {
    if (key.empty()) return {};
    return "/library/sections/" + url_encode(key) + "/all?type=10";
}

std::string metadata_children_path(std::string key) {
    if (key.empty()) return {};
    if (key.front() == '/') return key;
    return "/library/metadata/" + url_encode(key) + "/children";
}

std::string metadata_all_leaves_path(std::string key) {
    if (key.empty()) return {};
    if (key.front() == '/') return key + (key.find('?') == std::string::npos ? "?includeGuids=1" : "");
    return "/library/metadata/" + url_encode(key) + "/allLeaves";
}

std::string plex_metadata_path(const PlexTrack& track) {
    if (!track.key.empty()) return "/library/metadata/" + url_encode(track.key);
    return {};
}

std::string transcode_mp3_path(const PlexTrack& track) {
    const auto meta = plex_metadata_path(track);
    if (meta.empty()) return {};

    static std::atomic_uint64_t counter{0};
    std::string session = "fh6-radio-";
    session += std::to_string(GetCurrentProcessId());
    session += "-";
    session += std::to_string(++counter);

    const std::string profile =
        "add-transcode-target(type=musicProfile&context=streaming&protocol=http&container=mp3&audioCodec=mp3)";

    std::string path = "/music/:/transcode/universal/start?hasMDE=1&path=";
    path += url_encode(meta);
    path += "&mediaIndex=0&partIndex=0&protocol=http&directPlay=0&directStream=0";
    path += "&directStreamAudio=0&musicBitrate=320&maxAudioBitrate=320&offset=0&copyts=0";
    path += "&session=" + url_encode(session);
    path += "&X-Plex-Client-Profile-Extra=" + url_encode(profile);
    return path;
}

std::filesystem::path temp_audio_path() {
    static std::atomic_uint64_t counter{0};
    auto p = std::filesystem::temp_directory_path();
    p /= "fh6-radio";
    p /= "plex-";
    p += std::to_string(GetCurrentProcessId());
    p += "-";
    p += std::to_string(GetTickCount64());
    p += "-";
    p += std::to_string(++counter);
    p += ".mp3";
    return p;
}

} // namespace

struct PlexSource::Pipe {
    HANDLE job       = nullptr;
    HANDLE proc_ff   = nullptr;
    HANDLE read_pipe = nullptr;
    std::uint64_t bytes_written = 0;
    bool ended = false;

    ~Pipe() {
        if (read_pipe) CloseHandle(read_pipe);
        if (job)       CloseHandle(job);
        if (proc_ff)   CloseHandle(proc_ff);
    }
};

PlexSource::PlexSource(PlexConfig cfg) : cfg_{std::move(cfg)} {}

PlexSource::~PlexSource() {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

bool PlexSource::initialize() {
    if (!cfg_.enabled) return false;
    auth_.store(cfg_.server_url.empty() || cfg_.token.empty() ? AuthState::needs_auth
                                                              : AuthState::authenticated,
                std::memory_order_release);
    return true;
}

void PlexSource::shutdown() noexcept {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

void PlexSource::update_config(PlexConfig cfg) {
    std::scoped_lock lk{mu_};
    const bool changed = cfg_.server_url != cfg.server_url || cfg_.token != cfg.token ||
                         cfg_.library_key != cfg.library_key ||
                         cfg_.playlist_key != cfg.playlist_key ||
                         cfg_.artist_key != cfg.artist_key || cfg_.album_key != cfg.album_key ||
                         cfg_.ffmpeg_path != cfg.ffmpeg_path ||
                         cfg_.shuffle != cfg.shuffle;
    cfg_ = std::move(cfg);
    if (changed) {
        stop_pipe_locked();
        queue_.clear();
        libraries_.clear();
        playlists_.clear();
        artists_.clear();
        albums_.clear();
        queue_idx_ = 0;
        last_error_.clear();
    }
    auth_.store(cfg_.server_url.empty() || cfg_.token.empty() ? AuthState::needs_auth
                                                              : AuthState::authenticated,
                std::memory_order_release);
}

void PlexSource::set_error_locked(std::string msg) {
    last_error_ = std::move(msg);
    auth_.store(AuthState::error, std::memory_order_release);
    log::warn("[plex] {}", last_error_);
}

bool PlexSource::refresh_libraries_locked() {
    if (cfg_.server_url.empty() || cfg_.token.empty()) {
        auth_.store(AuthState::needs_auth, std::memory_order_release);
        last_error_ = "Enter a Plex server URL and token.";
        return false;
    }

    json root;
    std::string error;
    if (!parse_json_response(winhttp_get(cfg_, "/library/sections"), root, error)) {
        set_error_locked("Could not read Plex libraries: " + error);
        return false;
    }

    std::vector<PlexLibrary> fresh;
    const json* mc = media_container(root);
    for_each_child(*mc, "Directory", [&](const json& item) {
        PlexLibrary lib;
        lib.key        = get_string(item, "key");
        lib.title      = get_string(item, "title");
        lib.type       = get_string(item, "type");
        lib.leaf_count = get_int(item, "leafCount");
        if (!lib.key.empty() && !lib.title.empty()) fresh.push_back(std::move(lib));
    });

    libraries_ = std::move(fresh);
    last_error_.clear();
    auth_.store(AuthState::authenticated, std::memory_order_release);
    return true;
}

bool PlexSource::refresh_playlists_locked() {
    if (cfg_.server_url.empty() || cfg_.token.empty()) {
        auth_.store(AuthState::needs_auth, std::memory_order_release);
        last_error_ = "Enter a Plex server URL and token.";
        return false;
    }

    json root;
    std::string error;
    if (!parse_json_response(winhttp_get(cfg_, "/playlists"), root, error)) {
        set_error_locked("Could not read Plex playlists: " + error);
        return false;
    }

    std::vector<PlexPlaylist> fresh;
    const json* mc = media_container(root);
    for_each_child(*mc, "Metadata", [&](const json& item) {
        const auto playlist_type = get_string(item, "playlistType");
        if (!playlist_type.empty() && playlist_type != "audio") return;

        PlexPlaylist pl;
        pl.key = get_string(item, "key");
        if (pl.key.empty()) pl.key = get_string(item, "ratingKey");
        pl.title      = get_string(item, "title");
        pl.leaf_count = get_int(item, "leafCount");
        if (!pl.key.empty() && !pl.title.empty()) fresh.push_back(std::move(pl));
    });

    playlists_ = std::move(fresh);
    last_error_.clear();
    auth_.store(AuthState::authenticated, std::memory_order_release);
    return true;
}

bool PlexSource::refresh_artists_locked() {
    if (cfg_.server_url.empty() || cfg_.token.empty()) {
        auth_.store(AuthState::needs_auth, std::memory_order_release);
        last_error_ = "Enter a Plex server URL and token.";
        return false;
    }
    if (cfg_.library_key.empty()) {
        auth_.store(AuthState::needs_auth, std::memory_order_release);
        last_error_ = "Choose a Plex music library first.";
        return false;
    }

    json root;
    std::string error;
    if (!parse_json_response(winhttp_get(cfg_, library_artists_path(cfg_.library_key)), root,
                             error)) {
        set_error_locked("Could not read Plex artists: " + error);
        return false;
    }

    std::vector<PlexArtist> fresh;
    const json* mc = media_container(root);
    auto add_artist = [&](const json& item) {
        PlexArtist artist = parse_artist(item);
        if (!artist.key.empty() && !artist.title.empty()) fresh.push_back(std::move(artist));
    };
    for_each_child(*mc, "Directory", add_artist);
    for_each_child(*mc, "Metadata", add_artist);

    artists_ = std::move(fresh);
    last_error_.clear();
    auth_.store(AuthState::authenticated, std::memory_order_release);
    return true;
}

bool PlexSource::refresh_albums_locked() {
    if (cfg_.server_url.empty() || cfg_.token.empty()) {
        auth_.store(AuthState::needs_auth, std::memory_order_release);
        last_error_ = "Enter a Plex server URL and token.";
        return false;
    }
    if (cfg_.library_key.empty() && cfg_.artist_key.empty()) {
        auth_.store(AuthState::needs_auth, std::memory_order_release);
        last_error_ = "Choose a Plex music library or artist first.";
        return false;
    }

    const auto path = !cfg_.artist_key.empty() ? metadata_children_path(cfg_.artist_key)
                                               : library_albums_path(cfg_.library_key);

    json root;
    std::string error;
    if (!parse_json_response(winhttp_get(cfg_, path), root, error)) {
        set_error_locked("Could not read Plex albums: " + error);
        return false;
    }

    std::vector<PlexAlbum> fresh;
    const json* mc = media_container(root);
    auto add_album = [&](const json& item) {
        PlexAlbum album = parse_album(cfg_, item);
        const auto type = get_string(item, "type");
        if (!type.empty() && type != "album") return;
        if (!album.key.empty() && !album.title.empty()) fresh.push_back(std::move(album));
    };
    for_each_child(*mc, "Directory", add_album);
    for_each_child(*mc, "Metadata", add_album);

    albums_ = std::move(fresh);
    last_error_.clear();
    auth_.store(AuthState::authenticated, std::memory_order_release);
    return true;
}

bool PlexSource::append_tracks_from_path_locked(const std::string& path, std::vector<PlexTrack>& out,
                                                std::string& error) {
    json root;
    if (!parse_json_response(winhttp_get(cfg_, path), root, error)) return false;

    const json* mc = media_container(root);
    for_each_child(*mc, "Metadata", [&](const json& item) {
        PlexTrack t = parse_track(cfg_, item);
        if (!t.part_key.empty()) out.push_back(std::move(t));
    });
    for_each_child(*mc, "Track", [&](const json& item) {
        PlexTrack t = parse_track(cfg_, item);
        if (!t.part_key.empty()) out.push_back(std::move(t));
    });
    return true;
}

bool PlexSource::refresh_catalog_locked() {
    if (cfg_.server_url.empty() || cfg_.token.empty()) {
        auth_.store(AuthState::needs_auth, std::memory_order_release);
        last_error_ = "Enter a Plex server URL and token.";
        return false;
    }

    std::vector<PlexTrack> fresh;
    std::string path;
    if (!cfg_.playlist_key.empty()) {
        path = playlist_items_path(cfg_.playlist_key);
    } else if (!cfg_.album_key.empty()) {
        path = metadata_children_path(cfg_.album_key);
    } else if (!cfg_.artist_key.empty()) {
        path = metadata_all_leaves_path(cfg_.artist_key);
    } else if (!cfg_.library_key.empty()) {
        path = library_tracks_path(cfg_.library_key);
    } else {
        auth_.store(AuthState::needs_auth, std::memory_order_release);
        last_error_ = "Choose a Plex library, playlist, artist, or album.";
        return false;
    }

    std::string error;
    const bool artist_scope = !cfg_.artist_key.empty() && cfg_.album_key.empty() &&
                              cfg_.playlist_key.empty();
    if (!append_tracks_from_path_locked(path, fresh, error) && !artist_scope) {
        set_error_locked("Could not read Plex tracks: " + error);
        return false;
    }

    if (fresh.empty() && artist_scope) {
        if (albums_.empty()) refresh_albums_locked();
        for (const auto& album : albums_) {
            if (!append_tracks_from_path_locked(metadata_children_path(album.key), fresh, error))
                log::warn("[plex] could not read album {}: {}", album.title, error);
        }
    }

    if (cfg_.shuffle) {
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::shuffle(fresh.begin(), fresh.end(), rng);
    }

    queue_ = std::move(fresh);
    queue_idx_ = 0;
    last_error_.clear();
    auth_.store(queue_.empty() ? AuthState::needs_auth : AuthState::authenticated,
                std::memory_order_release);
    if (queue_.empty()) {
        last_error_ = "No playable tracks found in the selected Plex source.";
        log::warn("[plex] no playable tracks found in selected source");
        return false;
    }

    log::info("[plex] loaded {} track(s)", queue_.size());
    return true;
}

bool PlexSource::refresh_catalog() {
    std::scoped_lock lk{mu_};
    return refresh_catalog_locked();
}

bool PlexSource::refresh_libraries() {
    std::scoped_lock lk{mu_};
    return refresh_libraries_locked();
}

bool PlexSource::refresh_playlists() {
    std::scoped_lock lk{mu_};
    return refresh_playlists_locked();
}

bool PlexSource::refresh_artists() {
    std::scoped_lock lk{mu_};
    return refresh_artists_locked();
}

bool PlexSource::refresh_albums() {
    std::scoped_lock lk{mu_};
    return refresh_albums_locked();
}

void PlexSource::start_pipe_locked() {
    stop_pipe_locked();
    if (queue_.empty() && !refresh_catalog_locked()) return;
    if (queue_.empty()) return;
    if (queue_idx_ >= queue_.size()) queue_idx_ = 0;

    const auto& track = queue_[queue_idx_];
    auto pipe = std::make_unique<Pipe>();

    if (track.part_key.empty()) {
        set_error_locked("Selected Plex track has no playable media part.");
        return;
    }

    pipe->job = create_kill_on_close_job();
    if (!pipe->job) {
        log::warn("[plex] CreateJobObject failed ({})", GetLastError());
        return;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE ff_out_r = nullptr;
    HANDLE ff_out_w = nullptr;
    if (!CreatePipe(&ff_out_r, &ff_out_w, &sa, 1 << 20)) return;
    SetHandleInformation(ff_out_r, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul_in  = open_nul(GENERIC_READ);
    HANDLE err_log = open_stderr_log();

    const auto ff = cfg_.ffmpeg_path.empty() ? L"ffmpeg" : cfg_.ffmpeg_path.wstring();
    const auto input_url = url_for_path(cfg_, track.part_key, false);
    std::wstring input_headers;
    if (!cfg_.token.empty()) {
        input_headers = L" -headers ";
        input_headers += quote(L"X-Plex-Token: " + widen(cfg_.token) + L"\r\n");
    }
    std::wstring ff_cmd = quote(ff) +
                          L" -loglevel error -nostdin -reconnect 1 -reconnect_streamed 1 "
                          L"-reconnect_delay_max 5" +
                          input_headers +
                          L" -i " +
                          quote(widen(input_url)) +
                          L" -vn -f s16le -acodec pcm_s16le -ar 48000 -ac 2 pipe:1";

    pipe->proc_ff = spawn_in_job(pipe->job, ff_cmd, nul_in, ff_out_w, err_log);
    const DWORD ec_ff = pipe->proc_ff ? 0u : GetLastError();
    CloseHandle(ff_out_w);
    if (!pipe->proc_ff) {
        CloseHandle(ff_out_r);
        if (nul_in)  CloseHandle(nul_in);
        if (err_log) CloseHandle(err_log);
        set_error_locked("Failed to launch ffmpeg: " +
                         describe_launch_failure(std::wstring{ff}, ec_ff,
                                                 !cfg_.ffmpeg_path.empty()));
        return;
    }

    if (nul_in)  CloseHandle(nul_in);
    if (err_log) CloseHandle(err_log);

    pipe->read_pipe = ff_out_r;
    pipe_           = std::move(pipe);

    info_              = TrackInfo{};
    info_.title        = track.title.empty() ? "(untitled)" : track.title;
    info_.artist       = track.artist;
    info_.album        = track.album;
    info_.artwork_url  = track.artwork_url;
    info_.duration_ms  = track.duration_ms;
    position_ms_.store(0, std::memory_order_release);
    state_.store(PlaybackState::buffering, std::memory_order_release);

    log::info("[plex] pipe started for track {}/{} via direct file; child stderr -> {}",
              queue_idx_ + 1, queue_.size(),
              stderr_log_path().string());
}

void PlexSource::stop_pipe_locked() {
    pipe_.reset();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void PlexSource::play() {
    std::scoped_lock lk{mu_};
    if (!pipe_) start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

void PlexSource::pause() {
    state_.store(PlaybackState::paused, std::memory_order_release);
}

bool PlexSource::restart_current() {
    std::scoped_lock lk{mu_};
    if (queue_.empty() && !refresh_catalog_locked()) return false;
    if (queue_.empty()) return false;
    consecutive_failed_ = 0;
    start_pipe_locked();
    if (!pipe_) return false;
    state_.store(PlaybackState::playing, std::memory_order_release);
    return true;
}

bool PlexSource::skip_next() {
    std::scoped_lock lk{mu_};
    if (queue_.empty() && !refresh_catalog_locked()) return false;
    if (queue_.empty()) return false;
    consecutive_failed_ = 0;
    const auto n = static_cast<std::ptrdiff_t>(queue_.size());
    auto i       = static_cast<std::ptrdiff_t>(queue_idx_) + 1;
    queue_idx_   = static_cast<std::size_t>(((i % n) + n) % n);
    start_pipe_locked();
    if (!pipe_) return false;
    state_.store(PlaybackState::playing, std::memory_order_release);
    return true;
}

void PlexSource::stop() {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

void PlexSource::next() {
    std::scoped_lock lk{mu_};
    if (queue_.empty() && !refresh_catalog_locked()) return;
    if (queue_.empty()) return;
    consecutive_failed_ = 0;
    const auto n = static_cast<std::ptrdiff_t>(queue_.size());
    auto i       = static_cast<std::ptrdiff_t>(queue_idx_) + 1;
    queue_idx_   = static_cast<std::size_t>(((i % n) + n) % n);
    start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

void PlexSource::previous() {
    std::scoped_lock lk{mu_};
    if (queue_.empty() && !refresh_catalog_locked()) return;
    if (queue_.empty()) return;
    consecutive_failed_ = 0;
    const auto n = static_cast<std::ptrdiff_t>(queue_.size());
    auto i       = static_cast<std::ptrdiff_t>(queue_idx_) - 1;
    queue_idx_   = static_cast<std::size_t>(((i % n) + n) % n);
    start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

TrackInfo PlexSource::current_track() const {
    std::scoped_lock lk{mu_};
    TrackInfo t   = info_;
    t.position_ms = position_ms_.load(std::memory_order_acquire);
    return t;
}

std::string PlexSource::auth_instructions() const {
    std::scoped_lock lk{mu_};
    if (cfg_.server_url.empty() || cfg_.token.empty()) {
        return "Enter your Plex server URL and token in the Plex panel.";
    }
    if (cfg_.library_key.empty() && cfg_.playlist_key.empty() && cfg_.artist_key.empty() &&
        cfg_.album_key.empty()) {
        return "Choose a Plex library, playlist, artist, or album in the Plex panel.";
    }
    if (!last_error_.empty()) return last_error_;
    return {};
}

void PlexSource::pump(RingBuffer& ring) {
    {
        auto st = state_.load(std::memory_order_acquire);
        if (st != PlaybackState::playing && st != PlaybackState::buffering) return;
    }

    std::scoped_lock lk{mu_};
    Pipe* p = pipe_.get();
    if (!p) return;

    auto advance_to_next = [&] {
        if (queue_.empty()) {
            stop_pipe_locked();
            return;
        }
        const auto n = static_cast<std::ptrdiff_t>(queue_.size());
        auto i       = static_cast<std::ptrdiff_t>(queue_idx_) + 1;
        queue_idx_   = static_cast<std::size_t>(((i % n) + n) % n);
        start_pipe_locked();
        if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
    };

    auto update_position = [&] {
        const std::size_t r = ring.readable();
        const std::uint64_t played = p->bytes_written > r ? p->bytes_written - r : 0;
        position_ms_.store(played * 1000ull / kPcmBytesPerSec, std::memory_order_release);
    };

    if (p->ended) {
        update_position();
        if (ring.readable() == 0) advance_to_next();
        return;
    }

    if (!p->read_pipe) return;

    auto on_eof = [&] {
        if (p->bytes_written == 0) {
            if (++consecutive_failed_ >= 3) {
                set_error_locked("Plex ffmpeg produced no PCM data.");
                stop_pipe_locked();
                return;
            }
            advance_to_next();
            return;
        }

        consecutive_failed_ = 0;
        p->ended            = true;
        if (p->read_pipe) {
            CloseHandle(p->read_pipe);
            p->read_pipe = nullptr;
        }
    };

    DWORD avail = 0;
    if (!PeekNamedPipe(p->read_pipe, nullptr, 0, nullptr, &avail, nullptr)) {
        on_eof();
        return;
    }
    while (avail > 0) {
        const std::size_t writable = ring.writable();
        if (writable < 4) break;
        std::size_t want = std::min<std::size_t>(writable, avail);
        if (want > 4096) want = 4096;
        std::byte buf[4096];
        DWORD got = 0;
        if (!ReadFile(p->read_pipe, buf, (DWORD)want, &got, nullptr) || got == 0) {
            on_eof();
            return;
        }
        ring.write(buf, got);
        p->bytes_written += got;
        update_position();
        avail = avail > got ? avail - got : 0;
        if (state_.load(std::memory_order_acquire) == PlaybackState::buffering &&
            ring.readable() > 32 * 1024)
            state_.store(PlaybackState::playing, std::memory_order_release);
    }
    update_position();
}

std::size_t PlexSource::track_count() const noexcept {
    std::scoped_lock lk{mu_};
    return queue_.size();
}

std::vector<PlexLibrary> PlexSource::libraries_snapshot() const {
    std::scoped_lock lk{mu_};
    return libraries_;
}

std::vector<PlexPlaylist> PlexSource::playlists_snapshot() const {
    std::scoped_lock lk{mu_};
    return playlists_;
}

std::vector<PlexArtist> PlexSource::artists_snapshot() const {
    std::scoped_lock lk{mu_};
    return artists_;
}

std::vector<PlexAlbum> PlexSource::albums_snapshot() const {
    std::scoped_lock lk{mu_};
    return albums_;
}

std::vector<PlexTrack> PlexSource::queue_snapshot() const {
    std::scoped_lock lk{mu_};
    return queue_;
}

} // namespace fh6::sources
