#pragma once

#include <minwindef.h>

namespace HookUtils
{

[[nodiscard]] bool InitMinHook();
[[nodiscard]] bool EnableMinHook(void* target);
[[nodiscard]] bool UninitMinHook();

[[nodiscard]] bool ModuleExport(HMODULE module, const char* name, void** target);
void ModuleExport(HMODULE module, uintptr_t rva, void** target);

[[nodiscard]] bool AddHook(void* address, void* detour, void** original);
[[nodiscard]] bool HookExport(HMODULE module, const char* name, void* detour, void** original);
[[nodiscard]] bool HookExport(HMODULE module, uintptr_t rva, void* detour, void** original);

[[nodiscard]] inline void* VTableMethod(void*** object, size_t index)
{
	return (*object)[index];
}

}; // namespace HookUtils
