#pragma once

#include <string>

namespace shuru {

// Applies a protected DACL granting full control only to the current user.
// Existing parent directories are hardened as well. Returns false without
// blocking input when security APIs or the filesystem reject the operation.
bool EnsureCurrentUserOnlyPath(const std::wstring& path, bool is_directory);

}  // namespace shuru
