#include "file_redirect.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>

#include "log.h"

namespace rmre5::fileredirect {
namespace {

using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using GetAttributesAFn = DWORD(WINAPI*)(LPCSTR);
using GetAttributesWFn = DWORD(WINAPI*)(LPCWSTR);
using GetAttributesExAFn = BOOL(WINAPI*)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using GetAttributesExWFn = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
CreateFileAFn g_origA = nullptr;
CreateFileWFn g_origW = nullptr;
GetAttributesAFn g_origAttrA = nullptr;
GetAttributesWFn g_origAttrW = nullptr;
GetAttributesExAFn g_origAttrExA = nullptr;
GetAttributesExWFn g_origAttrExW = nullptr;

std::mutex g_mutex;
std::shared_ptr<const std::vector<Redirect>> g_redirects = std::make_shared<std::vector<Redirect>>();
std::atomic<bool> g_logOpens{false};
std::atomic<int> g_logged{0};
thread_local bool t_inside = false;  // our own CreateFile calls (the log) pass straight through

// The last archives the game opened (any thread), newest last: a fatal "inflate" error names no
// file, so the crash log lists these and checks them (crash.cpp).
constexpr std::size_t kRecentArchives = 16;
std::mutex g_recentMutex;
std::wstring g_recent[kRecentArchives];
std::size_t g_recentCount = 0;

void NoteOpened(LPCWSTR path) {
    if (path == nullptr) return;
    const std::size_t n = wcslen(path);
    if (n < 4 || _wcsicmp(path + n - 4, L".arc") != 0) return;
    std::lock_guard lock(g_recentMutex);
    g_recent[g_recentCount % kRecentArchives] = path;
    ++g_recentCount;
}

// The music streams the game asked for (music.cpp learns the Mercenaries theme from them).
std::mutex g_streamMutex;
std::vector<std::wstring> g_streams;

void NoteStream(const std::wstring& path) {
    const std::size_t n = path.size();
    if (n < 5 || _wcsicmp(path.c_str() + n - 5, L".sngw") != 0) return;
    std::lock_guard lock(g_streamMutex);
    if (g_streams.size() < 32) g_streams.push_back(path);
}

std::shared_ptr<const std::vector<Redirect>> Current() {
    std::lock_guard lock(g_mutex);
    return g_redirects;
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = c == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::string Narrow(const std::wstring& w) {
    std::string out;
    for (wchar_t c : w) out += c < 0x80 ? static_cast<char>(c) : '?';
    return out;
}

DWORD TargetAttributes(const std::wstring& target) { return g_origAttrW ? g_origAttrW(target.c_str()) : GetFileAttributesW(target.c_str()); }

// The file to open instead of `path`, or empty. Reads only (a write never goes to a mod's file).
std::wstring Lookup(const std::wstring& path, DWORD access) {
    if (t_inside || path.empty()) return {};
    const std::wstring lower = Lower(path);
    const bool archive = lower.size() > 4 && lower.compare(lower.size() - 4, 4, L".arc") == 0;
    if (archive && g_logOpens.load() && g_logged.fetch_add(1) < 400) {
        t_inside = true;
        RM_LOGI("files: the game opens %s", Narrow(path).c_str());
        t_inside = false;
    }
    if ((access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0) return {};
    const auto redirects = Current();
    for (const Redirect& r : *redirects) {
        if (r.tail.empty() || lower.size() < r.tail.size()) continue;
        if (r.tail.back() == L'\\') {
            // A folder: a path inside it reads the same path inside the mod's folder.
            const auto at = lower.find(L"\\" + r.tail);
            if (at == std::wstring::npos) continue;
            const std::wstring rest = path.substr(at + 1 + r.tail.size());
            if (rest.empty()) continue;
            const std::wstring target = r.target + L"\\" + rest;
            const DWORD attributes = TargetAttributes(target);
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 20) {
                t_inside = true;
                RM_LOGI("files: %s read from %s", Narrow(path).c_str(), Narrow(target).c_str());
                t_inside = false;
            }
            return target;
        }
        if (lower.compare(lower.size() - r.tail.size(), r.tail.size(), r.tail) != 0) continue;
        // The tail must start at a folder boundary ("archive\x.arc" never matches "barchive\x.arc").
        if (lower.size() > r.tail.size() && r.tail.front() != L'\\' && lower[lower.size() - r.tail.size() - 1] != L'\\') continue;
        // A missing mod file would end the game ("Failed open file"): the game's own is read then.
        const DWORD attributes = TargetAttributes(r.target);
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            t_inside = true;
            RM_LOGW("files: %s is missing; the game's own %s is read", Narrow(r.target).c_str(), Narrow(path).c_str());
            t_inside = false;
            return {};
        }
        t_inside = true;
        RM_LOGI("files: %s read from %s", Narrow(path).c_str(), Narrow(r.target).c_str());
        t_inside = false;
        return r.target;
    }
    return {};
}

HANDLE WINAPI CreateFileWDetour(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags,
                                HANDLE templ) {
    if (name != nullptr) {
        if (!t_inside) NoteStream(name);
        const std::wstring target = Lookup(name, access);
        if (!target.empty()) {
            NoteOpened(target.c_str());
            return g_origW(target.c_str(), access, share, sa, disposition, flags, templ);
        }
        if (!t_inside) NoteOpened(name);
    }
    return g_origW(name, access, share, sa, disposition, flags, templ);
}

HANDLE WINAPI CreateFileADetour(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags,
                                HANDLE templ) {
    if (name != nullptr) {
        const int n = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0);
        if (n > 0) {
            std::wstring wide(static_cast<std::size_t>(n), L'\0');
            MultiByteToWideChar(CP_ACP, 0, name, -1, wide.data(), n);
            wide.resize(static_cast<std::size_t>(n - 1));
            if (!t_inside) NoteStream(wide);
            const std::wstring target = Lookup(wide, access);
            if (!target.empty()) {
                NoteOpened(target.c_str());
                return g_origW(target.c_str(), access, share, sa, disposition, flags, templ);
            }
            if (!t_inside) NoteOpened(wide.c_str());
        }
    }
    return g_origA(name, access, share, sa, disposition, flags, templ);
}

std::wstring Wide(LPCSTR name) {
    const int n = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, name, -1, wide.data(), n);
    wide.resize(static_cast<std::size_t>(n - 1));
    return wide;
}

DWORD WINAPI GetAttributesWDetour(LPCWSTR name) {
    if (name != nullptr) {
        const std::wstring target = Lookup(name, GENERIC_READ);
        if (!target.empty()) return g_origAttrW(target.c_str());
    }
    return g_origAttrW(name);
}

DWORD WINAPI GetAttributesADetour(LPCSTR name) {
    if (name != nullptr) {
        const std::wstring target = Lookup(Wide(name), GENERIC_READ);
        if (!target.empty()) return g_origAttrW(target.c_str());
    }
    return g_origAttrA(name);
}

BOOL WINAPI GetAttributesExWDetour(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    if (name != nullptr) {
        const std::wstring target = Lookup(name, GENERIC_READ);
        if (!target.empty()) return g_origAttrExW(target.c_str(), level, info);
    }
    return g_origAttrExW(name, level, info);
}

BOOL WINAPI GetAttributesExADetour(LPCSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    if (name != nullptr) {
        const std::wstring target = Lookup(Wide(name), GENERIC_READ);
        if (!target.empty()) return g_origAttrExW(target.c_str(), level, info);
    }
    return g_origAttrExA(name, level, info);
}

bool Queue(LPCWSTR module, LPCSTR proc, void* detour, void** original) {
    void* target = nullptr;
    MH_STATUS status = MH_CreateHookApiEx(module, proc, detour, original, &target);
    if (status == MH_OK) status = MH_QueueEnableHook(target);
    if (status != MH_OK) {
        RM_LOGE("files: hook on %s failed: %s", proc, MH_StatusToString(status));
        return false;
    }
    return true;
}

}  // namespace

bool Install() {
    const bool a = Queue(L"kernel32", "CreateFileA", reinterpret_cast<void*>(&CreateFileADetour), reinterpret_cast<void**>(&g_origA));
    const bool w = Queue(L"kernel32", "CreateFileW", reinterpret_cast<void*>(&CreateFileWDetour), reinterpret_cast<void**>(&g_origW));
    const bool aa = Queue(L"kernel32", "GetFileAttributesA", reinterpret_cast<void*>(&GetAttributesADetour), reinterpret_cast<void**>(&g_origAttrA));
    const bool aw = Queue(L"kernel32", "GetFileAttributesW", reinterpret_cast<void*>(&GetAttributesWDetour), reinterpret_cast<void**>(&g_origAttrW));
    const bool xa = Queue(L"kernel32", "GetFileAttributesExA", reinterpret_cast<void*>(&GetAttributesExADetour),
                          reinterpret_cast<void**>(&g_origAttrExA));
    const bool xw = Queue(L"kernel32", "GetFileAttributesExW", reinterpret_cast<void*>(&GetAttributesExWDetour),
                          reinterpret_cast<void**>(&g_origAttrExW));
    return a && w && aa && aw && xa && xw;
}

void SetRedirects(std::vector<Redirect> redirects) {
    for (Redirect& r : redirects) r.tail = NormaliseTail(r.tail);
    auto next = std::make_shared<const std::vector<Redirect>>(std::move(redirects));
    std::lock_guard lock(g_mutex);
    g_redirects = std::move(next);
}

std::vector<Redirect> Redirects() { return *Current(); }

void SetLogOpens(bool on) {
    g_logOpens = on;
    g_logged = 0;
}

std::wstring NormaliseTail(const std::wstring& tail) { return Lower(tail); }

std::vector<std::wstring> TakeStreamOpens() {
    std::lock_guard lock(g_streamMutex);
    std::vector<std::wstring> out;
    out.swap(g_streams);
    return out;
}

std::vector<std::wstring> RecentArchives() {
    std::lock_guard lock(g_recentMutex);
    std::vector<std::wstring> out;
    const std::size_t n = std::min(g_recentCount, kRecentArchives);
    for (std::size_t i = 0; i < n; ++i) out.push_back(g_recent[(g_recentCount - 1 - i) % kRecentArchives]);
    return out;
}

}  // namespace rmre5::fileredirect
