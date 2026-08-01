#pragma once

#include <filesystem>

namespace WebmAssets
{

void Scan(const std::filesystem::path& root);
void Clear();
const std::filesystem::path* Resolve(const char* requestedPath);

} // namespace WebmAssets
