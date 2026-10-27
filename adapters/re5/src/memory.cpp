#include "memory.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "crash.h"

namespace rmre5 {

namespace {

bool GuardedCopy(void* dst, const void* src, std::size_t size) {
    __try {
        std::memcpy(dst, src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool SafeCopy(void* dst, std::uintptr_t src, std::size_t size) {
    SetGuardedAccess(true);
    const bool ok = GuardedCopy(dst, reinterpret_cast<const void*>(src), size);
    SetGuardedAccess(false);
    return ok;
}

bool SafeWrite(std::uintptr_t dst, const void* src, std::size_t size) {
    SetGuardedAccess(true);
    const bool ok = GuardedCopy(reinterpret_cast<void*>(dst), src, size);
    SetGuardedAccess(false);
    return ok;
}

std::string ReadCString(std::uintptr_t address, std::size_t maxLength) {
    std::string out;
    for (std::size_t i = 0; i < maxLength; ++i) {
        char c = 0;
        if (!SafeRead(address + i, c) || c == '\0') {
            break;
        }
        if (c < 0x20 || c > 0x7E) {
            return {};
        }
        out.push_back(c);
    }
    return out;
}

bool BytesMatch(std::uintptr_t address, const char* hexPattern, std::string* actualHex) {
    std::vector<int> expected;
    for (const char* p = hexPattern; *p;) {
        while (*p == ' ') {
            ++p;
        }
        if (!*p) {
            break;
        }
        if (p[0] == '?') {
            expected.push_back(-1);
            p += (p[1] == '?') ? 2 : 1;
            continue;
        }
        unsigned value = 0;
        std::sscanf(p, "%2x", &value);
        expected.push_back(static_cast<int>(value));
        p += 2;
    }
    std::vector<unsigned char> actual(expected.size());
    if (!SafeCopy(actual.data(), address, actual.size())) {
        if (actualHex) {
            *actualHex = "<unreadable>";
        }
        return false;
    }
    bool match = true;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (expected[i] >= 0 && expected[i] != actual[i]) {
            match = false;
        }
    }
    if (actualHex) {
        actualHex->clear();
        char byte[4];
        for (unsigned char b : actual) {
            std::snprintf(byte, sizeof(byte), "%02X ", b);
            *actualHex += byte;
        }
    }
    return match;
}

}  // namespace rmre5
