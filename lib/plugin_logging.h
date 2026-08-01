#pragma once

#include "plugin_interfaces.h"

#include <string>
#include <utility>
#include <format>

bool InitializePluginLogging(HMODULE northstarModule, const PluginNorthstarData* initData);
void PluginLogMessageAsync(PluginLogLevel level, const std::string& message);
void PluginLogMessageSync(PluginLogLevel level, const std::string& message);
void FlushPluginLogs();

template<typename... Args>
void PluginLogInfo(std::format_string<Args...> format, Args&&... arguments)
{
	PluginLogMessageSync(PluginLogLevel::INFO, std::format(format, std::forward<Args>(arguments)...));
}

template<typename... Args>
void PluginLogWarn(std::format_string<Args...> format, Args&&... arguments)
{
	PluginLogMessageSync(PluginLogLevel::WARN, std::format(format, std::forward<Args>(arguments)...));
}

template<typename... Args>
void PluginLogError(std::format_string<Args...> format, Args&&... arguments)
{
	PluginLogMessageSync(PluginLogLevel::ERR, std::format(format, std::forward<Args>(arguments)...));
}

template<typename... Args>
void PluginLogInfoAsync(std::format_string<Args...> format, Args&&... arguments)
{
	PluginLogMessageAsync(PluginLogLevel::INFO, std::format(format, std::forward<Args>(arguments)...));
}

template<typename... Args>
void PluginLogWarnAsync(std::format_string<Args...> format, Args&&... arguments)
{
	PluginLogMessageAsync(PluginLogLevel::WARN, std::format(format, std::forward<Args>(arguments)...));
}

template<typename... Args>
void PluginLogErrorAsync(std::format_string<Args...> format, Args&&... arguments)
{
	PluginLogMessageAsync(PluginLogLevel::ERR, std::format(format, std::forward<Args>(arguments)...));
}
