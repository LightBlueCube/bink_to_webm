#pragma once

#include <minwindef.h>

[[nodiscard]] bool EnableWebmHooks();
[[nodiscard]] bool RegisterWebmHooks_engine(HMODULE engine);
[[nodiscard]] bool RegisterWebmHooks_client(HMODULE client);
[[nodiscard]] bool RegisterWebmHooks_bink2w64(HMODULE bink);
[[nodiscard]] bool PrepareRemoveWebmHooks();
