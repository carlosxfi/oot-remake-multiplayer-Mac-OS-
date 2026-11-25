// SPDX-License-Identifier: GPL-3.0-or-later
// CoopLocalStart's OS part (coop_local_start.h): Windows only; the Linux kit has no local co-op launcher (none offered).
#include "coop_local_start.h"

#include <algorithm>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601   // (QueryFullProcessImageNameW)
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <cwchar>
#endif

namespace CoopLocalStart {

#ifdef _WIN32

namespace {

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool PortFreeHere(std::uint16_t port) {   // (WSAStartup done)
    const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    BOOL exclusive = TRUE;   // (a socket of another program on the port, even shared, makes the bind fail)
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
    const bool ok = ::bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0;
    ::closesocket(s);
    return ok;
}

}  // namespace

bool Offered(const std::optional<std::filesystem::path>& kit, const std::filesystem::path& game_dir) {
    if (!kit || PlayerOf(game_dir) != 1) return false;
    std::error_code ec;
    if (std::filesystem::exists(game_dir / "coop-player.ini", ec)) return false;   // (a Nucleus Co-op instance)
    return std::filesystem::is_regular_file(*kit / "scripts" / kScript, ec) &&
           std::filesystem::is_regular_file(*kit / "scripts" / "local-coop.ps1", ec);
}

std::vector<int> RunningPlayers(const std::filesystem::path& game_dir) {
    std::vector<int> out;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof e;
    for (BOOL more = Process32FirstW(snap, &e); more; more = Process32NextW(snap, &e)) {
        if (_wcsicmp(e.szExeFile, L"longshot.exe") != 0) continue;
        const HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
        if (p == nullptr) continue;
        wchar_t path[MAX_PATH * 2];
        DWORD n = static_cast<DWORD>(std::size(path));
        if (QueryFullProcessImageNameW(p, 0, path, &n)) {
            const std::filesystem::path exe(std::wstring(path, n));
            std::error_code ec;
            const auto dir = exe.parent_path();
            const int player = PlayerOf(dir);
            if (player >= 2 && std::filesystem::equivalent(dir.parent_path().parent_path(), game_dir, ec)) {
                out.push_back(player);
            }
        }
        CloseHandle(p);
    }
    CloseHandle(snap);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::uint16_t FreePort() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
    std::uint16_t found = 0;
    for (std::uint32_t p = kFirstPort; p < kFirstPort + 40u && found == 0; ++p) {
        if (PortFreeHere(static_cast<std::uint16_t>(p))) found = static_cast<std::uint16_t>(p);
    }
    WSACleanup();
    return found;
}

bool PortFree(std::uint16_t port) {
    WSADATA wsa;
    if (port == 0 || WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    const bool ok = PortFreeHere(port);
    WSACleanup();
    return ok;
}

std::filesystem::path ThisExe() {
    wchar_t path[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return n > 0 && n < std::size(path) ? std::filesystem::path(std::wstring(path, n)) : std::filesystem::path();
}

std::uint32_t ThisPid() {
    return static_cast<std::uint32_t>(GetCurrentProcessId());
}

bool Launch(const std::string& command_line) {
    std::wstring cmd = Wide(command_line);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    // (no window, and not in the game's console or job: the games it starts outlive the launcher)
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB, nullptr,
                                   nullptr, &si, &pi);
    if (!ok) {   // (a job that does not allow breaking away: started inside it)
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                            nullptr, nullptr, &si, &pi)) {
            return false;
        }
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

#else

bool Offered(const std::optional<std::filesystem::path>&, const std::filesystem::path&) {
    return false;
}
std::vector<int> RunningPlayers(const std::filesystem::path&) {
    return {};
}
std::uint16_t FreePort() {
    return 0;
}
bool PortFree(std::uint16_t) {
    return false;
}
std::filesystem::path ThisExe() {
    return {};
}
std::uint32_t ThisPid() {
    return 0;
}
bool Launch(const std::string&) {
    return false;
}

#endif

}  // namespace CoopLocalStart
