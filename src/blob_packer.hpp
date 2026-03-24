#pragma once
#include "mod_loader.hpp"
#include <string>

namespace blob_packer {
std::wstring synthesize_upk(const UPKBlobManifest &manifest);
} // namespace blob_packer
