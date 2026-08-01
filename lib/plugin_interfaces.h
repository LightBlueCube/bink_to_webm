#pragma once

#include <stdint.h>
#include <windows.h>

#define PLUGIN_ID_VERSION "PluginId001"

enum class PluginString : int
{
	NAME = 0,
	LOG_NAME = 1,
	DEPENDENCY_NAME = 2,
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
enum class PluginField : int
{
	CONTEXT = 0,
	COLOR = 1,
};
#pragma GCC diagnostic pop

namespace PluginContext
{

enum : uint64_t
{
	DEDICATED = 0x1,
	CLIENT = 0x2,
};

} // namespace PluginContext

class IPluginId
{
public:
	virtual const char* GetString(PluginString property) = 0;
	virtual int64_t GetField(PluginField property) = 0;
};

struct PluginNorthstarData
{
	HMODULE pluginHandle;
};

class IPluginCallbacks
{
public:
	virtual void Init(HMODULE northstar, const PluginNorthstarData* initData, bool reloaded) = 0;
	virtual void Finalize() = 0;
	virtual bool Unload() = 0;
	virtual void OnSqvmCreated(void* sqvm) = 0;
	virtual void OnSqvmDestroying(void* sqvm) = 0;
	virtual void OnLibraryLoaded(HMODULE module, const char* name) = 0;
	virtual void RunFrame() = 0;
};

enum class PluginLogLevel : int
{
	INFO = 0,
	WARN,
	ERR,
};

class IPluginSys
{
public:
	virtual void Log(HMODULE plugin, PluginLogLevel level, char* message) = 0;
	virtual void Unload(HMODULE plugin) = 0;
	virtual void Reload(HMODULE plugin) = 0;
};

enum InterfaceStatus
{
	IFACE_OK = 0,
	IFACE_FAILED = 1,
};

extern "C" __declspec(dllexport) void* CreateInterface(const char* name, InterfaceStatus* returnCode);
