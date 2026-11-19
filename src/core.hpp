#pragma once

#include "provider_registry.hpp"

#include <cstdint>
#include <filesystem>
#include <istream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace yrswatch {

using Epoch = std::int64_t;

struct QuotaRow {
    std::string id, label;
    std::optional<double> remaining; // Fraction in [0, 1].
    std::optional<double> quantity;
    std::string unit;
    std::optional<Epoch> reset_at;
    std::string group, window;
    bool disabled = false;
};

struct AlertHistory {
    std::optional<Epoch> window_end;
    Epoch seen_at = 0;
    bool low_sent = false;
    bool exhausted_sent = false;
    bool awaiting_recovery = false;
};

struct QuotaSample {
    Provider provider = Provider::Codex;
    std::string account; // Optional opaque account identifier, never a credential.
    Epoch observed_at = 0;
    std::vector<QuotaRow> rows;
    std::map<std::string, AlertHistory> alerts;
};

struct ProviderState {
    std::optional<QuotaSample> last;
    std::string error;
};

struct Alert {
    enum class Kind { Low, Exhausted, Recovered };
    std::string row_id, label;
    Kind kind;
};

Epoch now_epoch();
// Remaining fraction at or below which a quota window is reported as low.
inline constexpr double low_quota_fraction = 0.20;
bool stale(const QuotaSample& sample, Epoch now, Epoch max_age = 900);
bool expired(const QuotaRow& row, Epoch now);
std::vector<Alert> apply_sample(ProviderState& state, QuotaSample sample,
                                double low_fraction = low_quota_fraction, Epoch now = now_epoch());

// The parent directory is created with mode 0700. Each replacement is atomic, fsynced and 0600.
bool save_cache(const std::filesystem::path& path, const std::vector<QuotaSample>& samples);
std::vector<QuotaSample> load_cache(const std::filesystem::path& path);
// Atomically replaces path with contents: parent created 0700, file fsynced and 0600.
bool write_private_file(const std::filesystem::path& path, std::string_view contents);
// Reads a regular file owned by the current user and inaccessible to others, without following symlinks.
std::optional<std::string> read_private_file(const std::filesystem::path& path, std::size_t max_size);

struct Memory { std::uint64_t used, total, available; }; // Bytes.
struct CpuTicks { std::uint64_t active, total; };
std::optional<Memory> parse_memory(std::istream& input);
std::optional<CpuTicks> parse_cpu(std::istream& input);
std::optional<Memory> read_memory();
std::optional<CpuTicks> read_cpu();
std::optional<double> cpu_fraction(CpuTicks before, CpuTicks after);

} // namespace yrswatch
