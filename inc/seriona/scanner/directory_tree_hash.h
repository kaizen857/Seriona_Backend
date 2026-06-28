#pragma once

#include "seriona/scanner/hash_utils.h"

#include <filesystem>

namespace seriona::scanner {

[[nodiscard]] DirectoryHashResult computeDirectoryTreeHash(const std::filesystem::path& rootPath,
                                                          const HashOptions& options = {});

}
