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
CreateFileAFn g_origA = nullptr;
CreateFileWFn g_origW = nullptr;

std::mutex g_mutex;
std::shared_ptr<const std::vector<Redirect>> g_redirects = std::make_shared<std::vector<Redirect>>();
std::atomic<bool> g_logOpens{false};
std::atomic<int> g_logged{0};
thread_local bool t_inside = false;  // our own CreateFile calls (the log) pass straight through

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
        if (lower.compare(lower.size() - r.tail.size(), r.tail.size(), r.tail) != 0) continue;
        // The tail must start at a folder boundary ("archive\x.arc" never matches "barchive\x.arc").
        if (lower.size() > r.tail.size() && r.tail.front() != L'\\' && lower[lower.size() - r.tail.size() - 1] != L'\\') continue;
        // A missing mod file would end the game ("Failed open file"): the game's own is read then.
        const DWORD attributes = GetFileAttributesW(r.target.c_str());
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
        const std::wstring target = Lookup(name, access);
        if (!target.empty()) return g_origW(target.c_str(), access, share, sa, disposition, flags, templ);
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
            const std::wstring target = Lookup(wide, access);
            if (!target.empty()) return g_origW(target.c_str(), access, share, sa, disposition, flags, templ);
        }
    }
    return g_origA(name, access, share, sa, disposition, flags, templ);
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
    return a && w;
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

}  // namespace rmre5::fileredirect
