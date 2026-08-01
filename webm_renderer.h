#pragma once

#include <cstdint>
#include <minwindef.h>

namespace WebmRenderer
{

[[nodiscard]] bool PrepareAlphaTexture(void* material, uint32_t width, uint32_t height);
void ReleaseAlphaTexture(void* material);
void UpdateAlphaTexture(void* material, const uint8_t* data, uint32_t pitch);

[[nodiscard]] bool RegisterHooks(HMODULE materialsystem);
[[nodiscard]] bool Ready();
void Shutdown();

} // namespace WebmRenderer
