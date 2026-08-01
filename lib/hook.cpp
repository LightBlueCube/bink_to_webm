#include "hook.h"
#include "plugin_logging.h"

#include "MinHook.h"

#define MH_CHECK_FAILD(status)                                                                                                             \
	[&]() -> bool                                                                                                                          \
	{                                                                                                                                      \
		MH_STATUS _status = (status);                                                                                                      \
		if(_status != MH_OK)                                                                                                               \
		{                                                                                                                                  \
			PluginLogError("[{}:{}] MH_CHECK_FAILD: {}", __FILE__, __LINE__, MH_StatusToString(_status));                                  \
			return false;                                                                                                                  \
		}                                                                                                                                  \
		return true;                                                                                                                       \
	}()


namespace
{

std::string GetModuleBaseName(HMODULE module)
{
	char buf[MAX_PATH];
	uint32_t len = GetModuleFileNameA(module, buf, MAX_PATH);
	if(len == 0)
		return "";

	if(const char* base = strrchr(buf, '\\'))
		return static_cast<std::string>(++base);
	return static_cast<std::string>(buf);
}

} // namespace

namespace HookUtils
{

[[nodiscard]] bool InitMinHook()
{
	return MH_CHECK_FAILD(MH_Initialize());
}

[[nodiscard]] bool EnableMinHook(void* target)
{
	return MH_CHECK_FAILD(MH_EnableHook(target));
}

[[nodiscard]] bool UninitMinHook()
{
	return MH_CHECK_FAILD(MH_Uninitialize());
}


[[nodiscard]] bool ModuleExport(HMODULE module, const char* name, void** target)
{
	PluginLogInfo("Exporting {}({})", GetModuleBaseName(module), name);

	if(!(*target = (void*)GetProcAddress(module, name)))
	{
		PluginLogError("Module does not export {}({})", GetModuleBaseName(module), name);
		return false;
	}
	return true;
}

void ModuleExport(HMODULE module, uintptr_t rva, void** target)
{
	PluginLogInfo("Exporting {}+{:#x}", GetModuleBaseName(module), rva);
	*target = (void*)((uintptr_t)module + rva);
}

[[nodiscard]] bool AddHook(void* address, void* detour, void** original)
{
	return MH_CHECK_FAILD(MH_CreateHook(address, detour, original));
}

[[nodiscard]] bool HookExport(HMODULE module, const char* name, void* detour, void** original)
{
	void* target;
	if(!ModuleExport(module, name, &target))
		return false;

	PluginLogInfo("Hooking {}({}) [{:#x}]", GetModuleBaseName(module), name, (uintptr_t)target - (uintptr_t)module);
	return AddHook(target, detour, original);
}

[[nodiscard]] bool HookExport(HMODULE module, uintptr_t rva, void* detour, void** original)
{
	PluginLogInfo("Hooking {}+{:#x}", GetModuleBaseName(module), rva);
	return AddHook((void*)((uintptr_t)module + rva), detour, original);
}

}; // namespace HookUtils

#undef MH_CHECK_FAILD
