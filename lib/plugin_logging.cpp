#include "plugin_logging.h"
#include "hook.h"

#include <mutex>
#include <string>
#include <vector>

namespace
{

struct PendingPluginLog
{
	PluginLogLevel level;
	std::string message;
};

HMODULE PluginHandle = nullptr;
IPluginSys* PluginSystem = nullptr;

std::mutex PluginLogMutex;
std::vector<PendingPluginLog> PendingPluginLogs;

} // namespace

[[nodiscard]] bool InitializePluginLogging(HMODULE northstar, const PluginNorthstarData* initData)
{
	PluginHandle = initData->pluginHandle;

	using NSCreateInterfaceFn = void* (*)(const char* name, InterfaceStatus* returnCode);
	auto createInterface = (NSCreateInterfaceFn)(void*)GetProcAddress(northstar, "CreateInterface");
	if(!createInterface)
		return false;

	PluginSystem = static_cast<IPluginSys*>(createInterface("NSSys001", nullptr));
	return true;
}

void PluginLogMessageAsync(PluginLogLevel level, const std::string& message)
{
	std::lock_guard lock(PluginLogMutex);
	PendingPluginLogs.push_back({level, message});
}

void PluginLogMessageSync(PluginLogLevel level, const std::string& message)
{
	PluginSystem->Log(PluginHandle, level, (char*)message.c_str());
}

void FlushPluginLogs()
{
	std::vector<PendingPluginLog> logs;
	{
		std::lock_guard lock(PluginLogMutex);
		if(PendingPluginLogs.empty())
			return;

		logs.swap(PendingPluginLogs);
	}

	for(auto& log : logs)
		PluginLogMessageSync(log.level, log.message);
}
