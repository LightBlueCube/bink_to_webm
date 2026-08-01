#include "lib/plugin_logging.h"
#include "lib/hook.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <windows.h>

namespace
{

struct Command
{
	int32_t argCount;
	uint8_t reserved[4];
	int64_t argv0Size;
	char argSBuffer[512];
	char argvBuffer[512];
	const char* args[64];
};
// clang-format off
static_assert(offsetof(Command, argCount) 		== 0x000);
static_assert(offsetof(Command, argv0Size) 		== 0x008);
static_assert(offsetof(Command, argSBuffer) 	== 0x010);
static_assert(offsetof(Command, argvBuffer) 	== 0x210);
static_assert(offsetof(Command, args) 			== 0x410);
static_assert(	sizeof(Command) 				== 0x610);
// clang-format on

// clang-format off
using CreateInterfaceFn 		= void* (*)			 (const char* name, int* returnCode);
using CommandCallbackFn 		= void 	(__fastcall*)(const Command& command);
using ConCommandConstructorFn 	= void* (__fastcall*)(void* command, const char* name, CommandCallbackFn callback,
													  const char* help, uint32_t flags, void* completionCallback);
using UnregisterConCommandFn 	= void 	(__fastcall*)(void* cvar, void* command);
using FindCommandBaseFn 		= void* (__fastcall*)(void* cvar, const char* name);
using VideoPanelCreateFn 		= bool 	(__fastcall*)(const char* fileName, const char* exitCommand, int x, int y, int width, int height,
													  int playbackMode, float fadeTime, bool noInterrupt, bool transition, bool unused, bool forcePlay);
using StopTransitionVideosFn 	= void 	(__fastcall*)();
// clang-format on

constexpr uintptr_t VIDEO_PANEL_CREATE_RVA = 0x3564f0;
constexpr uintptr_t STOP_TRANSITION_VIDEOS_RVA = 0x357970;
constexpr uintptr_t CON_COMMAND_CONSTRUCTOR_RVA = 0x415f60;
constexpr size_t CON_COMMAND_SIZE = 0x60;
constexpr size_t UNREGISTER_CON_COMMAND_VTABLE_INDEX = 10;
constexpr size_t FIND_COMMAND_BASE_VTABLE_INDEX = 14;

constexpr char COMMAND_NAME[] = "playvideoex";
constexpr char COMMAND_HELP[] = "Plays a video: <filename> <x> <y> <width> <height> <loop> [fade_in]";

alignas(void*) std::byte CommandStorage[CON_COMMAND_SIZE] = {};
void*** Cvar = nullptr;
void* ConCommand = nullptr;
VideoPanelCreateFn VideoPanelCreate = nullptr;
StopTransitionVideosFn StopTransitionVideos = nullptr;



bool ParseInt(const char* text, int& value)
{
	char* end = nullptr;
	value = static_cast<int>(std::strtol(text, &end, 10));
	return *end == '\0';
}

bool ParseFloat(const char* text, float& value)
{
	char* end = nullptr;
	value = std::strtof(text, &end);
	return *end == '\0' && !std::isnan(value);
}

void __fastcall PlayVideoEX(const Command& command)
{
	if(command.argCount < 7)
	{
		PluginLogWarn("Usage: {} <filename> <x> <y> <width> <height> <loop> [fade_in]", COMMAND_NAME);
		return;
	}

	int x, y, width, height, loop;
	if(!ParseInt(command.args[2], x) || !ParseInt(command.args[3], y) || !ParseInt(command.args[4], width) ||
	   !ParseInt(command.args[5], height) || !ParseInt(command.args[6], loop) || width <= 0 || height <= 0 || (loop != 0 && loop != 1))
	{
		PluginLogWarn("{}: requires integer x/y, positive width/height, and loop set to 0 or 1", COMMAND_NAME);
		return;
	}

	bool looping = static_cast<bool>(loop);
	float fadeTime = looping ? 0.1f : 0.0f;
	if(command.argCount >= 8)
	{
		if(looping)
		{
			PluginLogWarn("{}: looping video cannot have a fade_in", COMMAND_NAME);
			return;
		}
		if(!ParseFloat(command.args[7], fadeTime) || fadeTime <= 0.0f)
		{
			PluginLogWarn("{}: requires a positive fade_in", COMMAND_NAME);
			return;
		}
	}

	if(looping)
		StopTransitionVideos();
	if(!VideoPanelCreate(command.args[1], nullptr, x, y, width, height, looping ? 2 : 1, fadeTime, looping, looping, false, false))
		PluginLogWarn("{}: Unable to play video: {}", COMMAND_NAME, command.args[1]);
}

} // namespace


[[nodiscard]] bool InstallPlayVideoEXCommand(HMODULE client)
{
	HMODULE vstdlib = GetModuleHandleA("vstdlib.dll");
	if(!vstdlib)
	{
		PluginLogError("Cannot register {}: vstdlib.dll did not load", COMMAND_NAME);
		return false;
	}
	CreateInterfaceFn createInterface;
	if(!HookUtils::ModuleExport(vstdlib, "CreateInterface", (void**)&createInterface))
		return false;

	int returnCode;
	Cvar = (void***)createInterface("VEngineCvar007", &returnCode);
	if(returnCode != IFACE_OK || !Cvar)
	{
		PluginLogError("Cannot register {}: vstdlib.dll does not provide interface VEngineCvar007: return_code={}", COMMAND_NAME, returnCode);
		return false;
	}

	auto findCommandBase = (FindCommandBaseFn)HookUtils::VTableMethod(Cvar, FIND_COMMAND_BASE_VTABLE_INDEX);
	if(findCommandBase(Cvar, COMMAND_NAME))
	{
		PluginLogError("Cannot register {}: command already exists", COMMAND_NAME);
		return false;
	}
	HookUtils::ModuleExport(client, VIDEO_PANEL_CREATE_RVA, (void**)&VideoPanelCreate);
	HookUtils::ModuleExport(client, STOP_TRANSITION_VIDEOS_RVA, (void**)&StopTransitionVideos);

	HMODULE engine = GetModuleHandleA("engine.dll");
	if(!engine)
	{
		PluginLogError("Cannot register {}: engine.dll did not load", COMMAND_NAME);
		return false;
	}

	ConCommandConstructorFn conCommandConstructor = nullptr;
	HookUtils::ModuleExport(engine, CON_COMMAND_CONSTRUCTOR_RVA, (void**)&conCommandConstructor);
	ConCommand = conCommandConstructor(CommandStorage, COMMAND_NAME, PlayVideoEX, COMMAND_HELP, 0, nullptr);

	PluginLogInfo("Registered command: {}", COMMAND_NAME);
	return true;
}

void RemovePlayVideoEXCommand()
{
	if(!ConCommand)
		return;

	auto unregisterConCommand = (UnregisterConCommandFn)HookUtils::VTableMethod(Cvar, UNREGISTER_CON_COMMAND_VTABLE_INDEX);
	unregisterConCommand(Cvar, ConCommand);
	ConCommand = nullptr;
	Cvar = nullptr;
	VideoPanelCreate = nullptr;
	StopTransitionVideos = nullptr;
}
