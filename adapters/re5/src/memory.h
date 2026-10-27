#pragma once

// Fault-tolerant access to game memory. Engine objects can be freed between a
// pointer read and its use (stage unload, enemy despawn), so every pointer
// chase goes through these helpers instead of raw dereferences.

#include <cstddef>
#include <cstdint>
#include <string>

namespace rmre5 {

bool SafeCopy(void* dst, std::uintptr_t src, std::size_t size);
bool SafeWrite(std::uintptr_t dst, const void* src, std::size_t size);

template <class T>
bool SafeRead(std::uintptr_t address, T& out) {
    return address >= 0x10000 && SafeCopy(&out, address, sizeof(T));
}

template <class T>
T ReadOr(std::uintptr_t address, T fallback) {
    T value{};
    return SafeRead(address, value) ? value : fallback;
}

template <class T>
bool SafeStore(std::uintptr_t address, const T& value) {
    return address >= 0x10000 && SafeWrite(address, &value, sizeof(T));
}

// Reads a NUL-terminated ASCII string of at most maxLength characters.
std::string ReadCString(std::uintptr_t address, std::size_t maxLength = 96);

// Parses "AA BB ?? CC" and compares it with the bytes at address.
bool BytesMatch(std::uintptr_t address, const char* hexPattern, std::string* actualHex = nullptr);

}  // namespace rmre5
