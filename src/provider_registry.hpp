#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace yrswatch {

// Adding a source: append an enumerator and a matching provider_table entry,
// then implement its fetch function (see providers.cpp). The compiler flags
// every switch that still needs a case.
enum class Provider { Codex, Antigravity, Claude };

enum class SourceKind {
    Cli,      // Spawns an official client. Heavy, so at most one runs at a time.
    Snapshot, // Reads a file written by another tool. Cheap and never queued.
};

struct ProviderInfo {
    Provider id;
    std::string_view key;   // Stable identifier: cache section and notification key prefix.
    std::string_view title; // Menu heading and notification title.
    SourceKind kind;
    bool hide_without_data; // Leave the menu block out until a reading exists.
};

inline constexpr std::array provider_table{
    ProviderInfo{Provider::Codex, "codex", "Codex", SourceKind::Cli, false},
    ProviderInfo{Provider::Antigravity, "antigravity", "Antigravity", SourceKind::Cli, false},
    // Only Claude.ai subscription logins expose usage windows, so stay hidden until one arrives.
    ProviderInfo{Provider::Claude, "claude", "Claude", SourceKind::Snapshot, true},
};
inline constexpr std::size_t provider_count = provider_table.size();

constexpr std::size_t index(Provider provider) { return static_cast<std::size_t>(provider); }
constexpr const ProviderInfo& info(Provider provider) { return provider_table[index(provider)]; }

// info() indexes the table directly, so its order must follow the enumeration.
static_assert([] {
    for (std::size_t i = 0; i < provider_count; ++i)
        if (index(provider_table[i].id) != i) return false;
    return true;
}(), "provider_table must list providers in enumeration order");

} // namespace yrswatch
