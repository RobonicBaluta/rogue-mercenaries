#pragma once

namespace rmre5 {

// Loads %SystemRoot%\SysWOW64\dinput8.dll and resolves its exports.
bool LoadRealDinput8();
void* RealDirectInput8Create();

}  // namespace rmre5
