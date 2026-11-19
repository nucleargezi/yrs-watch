#pragma once

// Snapshot files are written by companion tools and read by Snapshot sources.

#include <glib.h>

#include <cstddef>
#include <filesystem>

namespace yrswatch {

inline constexpr std::size_t max_snapshot_size = 64 * 1024;

inline std::filesystem::path snapshot_dir() {
    return std::filesystem::path(g_get_user_cache_dir()) / "yrs-watch";
}

// Written by yrs-watch-statusline from Claude Code's statusline input:
// {"version":1,"observed_at":<unix>,"windows":{"five_hour":{"used_percentage":26,"resets_at":<unix>},...}}
inline std::filesystem::path claude_snapshot_path() {
    return snapshot_dir() / "claude-statusline.json";
}

} // namespace yrswatch
