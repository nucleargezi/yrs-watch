#include "core.hpp"

#include <glib.h>
#include <glib/gstdio.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace yrswatch {

Epoch now_epoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool stale(const QuotaSample& sample, Epoch now, Epoch max_age) {
    return sample.observed_at <= 0 || sample.observed_at > now || now - sample.observed_at >= max_age;
}

bool expired(const QuotaRow& row, Epoch now) {
    return row.reset_at && now >= *row.reset_at;
}

namespace {

bool in_range(double value, double maximum) {
    return std::isfinite(value) && value >= 0 && value <= maximum;
}

void evaluate_row(const QuotaRow& row, Epoch observed, double threshold, AlertHistory& history,
                  std::vector<Alert>& alerts) {
    if (row.disabled) return;
    const auto value = row.remaining ? row.remaining : row.quantity;
    if (!value) return;

    const bool window_ended = history.window_end && observed >= *history.window_end;
    // A provider can grant a full refill before its previously advertised reset.
    const bool fully_refilled = row.remaining == 1.0;
    if (window_ended || fully_refilled) {
        history.low_sent = false;
        history.exhausted_sent = false;
        history.window_end.reset();
    }
    if (row.reset_at) history.window_end = row.reset_at;
    history.seen_at = observed;

    if (*value == 0) {
        if (!history.exhausted_sent) {
            alerts.push_back({row.id, row.label, Alert::Kind::Exhausted});
            history.exhausted_sent = true;
            history.awaiting_recovery = true;
        }
        // Moving from exhausted to a small positive amount should emit only recovery.
        history.low_sent = true;
    } else if (history.awaiting_recovery) {
        alerts.push_back({row.id, row.label, Alert::Kind::Recovered});
        history.awaiting_recovery = false;
        // A positive balance is the only boundary a windowless credit pool may expose.
        if (!history.window_end) history.exhausted_sent = false;
        if (row.remaining && *row.remaining <= threshold) history.low_sent = true;
    } else if (row.remaining && *row.remaining <= threshold && !history.low_sent) {
        alerts.push_back({row.id, row.label, Alert::Kind::Low});
        history.low_sent = true;
    }
}

} // namespace

std::vector<Alert> apply_sample(ProviderState& state, QuotaSample sample, double low_fraction, Epoch now) {
    std::vector<Alert> alerts;
    // An omitted account ID is unknown, not a different account.
    const bool same_account = state.last && state.last->provider == sample.provider &&
        (sample.account.empty() || state.last->account.empty() || state.last->account == sample.account);
    if (same_account && sample.observed_at < state.last->observed_at) {
        state.error.clear();
        return alerts;
    }
    if (same_account && sample.account.empty()) sample.account = state.last->account;
    sample.alerts = same_account ? state.last->alerts : decltype(sample.alerts){};
    // Retain deduplication across briefly omitted rows, without keeping obsolete pools indefinitely.
    constexpr Epoch retention = 90 * 24 * 3600;
    std::erase_if(sample.alerts, [&](const auto& entry) {
        return sample.observed_at > retention && entry.second.seen_at < sample.observed_at - retention;
    });
    for (auto& row : sample.rows) {
        if (row.remaining && !in_range(*row.remaining, 1)) row.remaining.reset();
        if (row.quantity && !in_range(*row.quantity, std::numeric_limits<double>::max())) row.quantity.reset();
        if (!row.id.empty() && !stale(sample, now) && !expired(row, now)) {
            evaluate_row(row, sample.observed_at, low_fraction, sample.alerts[row.id], alerts);
        }
    }
    state.last = std::move(sample);
    state.error.clear();
    return alerts;
}

namespace {

constexpr int max_rows = 512;
constexpr std::size_t max_cache_size = 512 * 1024;

std::string provider_key(Provider provider) {
    return std::string(info(provider).key);
}

std::string get_text(GKeyFile* file, const char* group, const char* key) {
    g_autofree gchar* value = g_key_file_get_string(file, group, key, nullptr);
    return value ? value : "";
}

std::optional<Epoch> get_time(GKeyFile* file, const char* group, const char* key) {
    const Epoch value = g_key_file_get_int64(file, group, key, nullptr);
    return value > 0 ? std::optional<Epoch>(value) : std::nullopt;
}

std::optional<double> get_number(GKeyFile* file, const char* group, const char* key, double maximum) {
    g_autoptr(GError) error = nullptr;
    double value = g_key_file_get_double(file, group, key, &error);
    return !error && in_range(value, maximum) ? std::optional<double>(value) : std::nullopt;
}

void store_sample(GKeyFile* file, const QuotaSample& sample) {
    const auto key = provider_key(sample.provider);
    const char* section = key.c_str();
    g_key_file_set_int64(file, section, "observed", sample.observed_at);
    g_key_file_set_string(file, section, "account", sample.account.c_str());
    g_key_file_set_integer(file, section, "rows", static_cast<int>(sample.rows.size()));
    g_key_file_set_integer(file, section, "alerts", static_cast<int>(sample.alerts.size()));
    int index = 0;
    for (const auto& row : sample.rows) {
        auto name = std::string(section) + ".row." + std::to_string(index++);
        const char* group = name.c_str();
        g_key_file_set_string(file, group, "id", row.id.c_str());
        g_key_file_set_string(file, group, "label", row.label.c_str());
        g_key_file_set_string(file, group, "group", row.group.c_str());
        g_key_file_set_string(file, group, "window", row.window.c_str());
        g_key_file_set_string(file, group, "unit", row.unit.c_str());
        if (row.remaining) g_key_file_set_double(file, group, "remaining", *row.remaining);
        if (row.quantity) g_key_file_set_double(file, group, "quantity", *row.quantity);
        if (row.reset_at) g_key_file_set_int64(file, group, "reset", *row.reset_at);
        if (row.disabled) g_key_file_set_boolean(file, group, "disabled", true);
    }
    index = 0;
    for (const auto& [id, history] : sample.alerts) {
        auto name = std::string(section) + ".alert." + std::to_string(index++);
        const char* group = name.c_str();
        g_key_file_set_string(file, group, "id", id.c_str());
        g_key_file_set_int64(file, group, "seen", history.seen_at);
        if (history.window_end) g_key_file_set_int64(file, group, "end", *history.window_end);
        g_key_file_set_boolean(file, group, "low", history.low_sent);
        g_key_file_set_boolean(file, group, "exhausted", history.exhausted_sent);
        g_key_file_set_boolean(file, group, "recovery", history.awaiting_recovery);
    }
}

std::optional<QuotaSample> restore_sample(GKeyFile* file, Provider provider) {
    const auto key = provider_key(provider);
    const char* section = key.c_str();
    const auto observed = get_time(file, section, "observed");
    const int rows = g_key_file_get_integer(file, section, "rows", nullptr);
    const int alerts = g_key_file_get_integer(file, section, "alerts", nullptr);
    if (!observed || rows < 1 || rows > max_rows || alerts < 0 || alerts > max_rows) return {};
    QuotaSample sample;
    sample.provider = provider;
    sample.account = get_text(file, section, "account");
    sample.observed_at = *observed;
    for (int i = 0; i < rows; ++i) {
        auto name = std::string(section) + ".row." + std::to_string(i);
        const char* group = name.c_str();
        QuotaRow row;
        row.id = get_text(file, group, "id");
        row.label = get_text(file, group, "label");
        if (row.id.empty() || row.label.empty()) return {};
        row.group = get_text(file, group, "group");
        row.window = get_text(file, group, "window");
        row.unit = get_text(file, group, "unit");
        row.remaining = get_number(file, group, "remaining", 1);
        row.quantity = get_number(file, group, "quantity", std::numeric_limits<double>::max());
        row.reset_at = get_time(file, group, "reset");
        constexpr std::string_view legacy_disabled = "（未开放）";
        row.disabled = g_key_file_get_boolean(file, group, "disabled", nullptr);
        if (!row.disabled && row.label.ends_with(legacy_disabled)) {
            row.disabled = true;
            row.label.resize(row.label.size() - legacy_disabled.size());
            if (row.label.empty()) row.label = row.id;
        }
        sample.rows.push_back(std::move(row));
    }
    for (int i = 0; i < alerts; ++i) {
        auto name = std::string(section) + ".alert." + std::to_string(i);
        const char* group = name.c_str();
        const auto id = get_text(file, group, "id");
        const auto seen = get_time(file, group, "seen");
        if (id.empty() || !seen) continue;
        auto& h = sample.alerts[id];
        h.seen_at = *seen;
        h.window_end = get_time(file, group, "end");
        h.low_sent = g_key_file_get_boolean(file, group, "low", nullptr);
        h.exhausted_sent = g_key_file_get_boolean(file, group, "exhausted", nullptr);
        h.awaiting_recovery = g_key_file_get_boolean(file, group, "recovery", nullptr);
    }
    return sample;
}

} // namespace

bool save_cache(const std::filesystem::path& path, const std::vector<QuotaSample>& samples) {
    g_autoptr(GKeyFile) file = g_key_file_new();
    g_key_file_set_integer(file, "cache", "version", 1);
    for (const auto& sample : samples) {
        if (sample.rows.size() > max_rows || sample.alerts.size() > max_rows) return false;
        store_sample(file, sample);
    }
    gsize length = 0;
    g_autofree gchar* data = g_key_file_to_data(file, &length, nullptr);
    if (!data || length > max_cache_size) return false;
    const auto parent = path.parent_path();
    if (!parent.empty() && g_mkdir_with_parents(parent.c_str(), 0700) != 0) return false;
    // Not g_file_set_contents_full(): recent GLib keeps the mode of the file it
    // replaces, and the cache must always end up 0600.
    auto temp = path.string() + ".XXXXXX";
    const int fd = g_mkstemp_full(temp.data(), O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    std::size_t done = 0;
    while (done < length) {
        const ssize_t n = write(fd, data + done, length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += static_cast<std::size_t>(n);
    }
    // Flush before the rename so a power loss cannot leave an empty cache behind.
    const bool synced = done == length && fsync(fd) == 0;
    const bool closed = close(fd) == 0;
    if (synced && closed && rename(temp.c_str(), path.c_str()) == 0) return true;
    unlink(temp.c_str());
    return false;
}

std::optional<std::string> read_private_file(const std::filesystem::path& path, std::size_t max_size) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return {};
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & 0077) != 0 || info.st_size <= 0 || info.st_size > static_cast<off_t>(max_size)) {
        close(fd);
        return {};
    }
    std::string data(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = read(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += static_cast<std::size_t>(n);
    }
    close(fd);
    if (done != data.size()) return {};
    return data;
}

std::vector<QuotaSample> load_cache(const std::filesystem::path& path) {
    const auto data = read_private_file(path, max_cache_size);
    if (!data) return {};
    g_autoptr(GKeyFile) file = g_key_file_new();
    if (!g_key_file_load_from_data(file, data->data(), data->size(), G_KEY_FILE_NONE, nullptr) ||
        g_key_file_get_integer(file, "cache", "version", nullptr) != 1) return {};
    std::vector<QuotaSample> samples;
    for (const auto& entry : provider_table) {
        if (auto sample = restore_sample(file, entry.id)) samples.push_back(std::move(*sample));
    }
    return samples;
}

std::optional<Memory> parse_memory(std::istream& input) {
    std::optional<std::uint64_t> total, available;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.starts_with("MemTotal:") && !line.starts_with("MemAvailable:")) continue;
        std::istringstream fields(line);
        std::string name, unit;
        std::uint64_t kib;
        if (!(fields >> name >> kib >> unit) || unit != "kB" ||
            kib > std::numeric_limits<std::uint64_t>::max() / 1024) return {};
        (name == "MemTotal:" ? total : available) = kib * 1024;
        if (total && available) break;
    }
    if (!total || !available || *total == 0 || *available > *total) return {};
    return Memory{*total - *available, *total, *available};
}

std::optional<CpuTicks> parse_cpu(std::istream& input) {
    std::string line;
    if (!std::getline(input, line)) return {};
    std::istringstream fields(line);
    std::string name;
    if (!(fields >> name) || name != "cpu") return {};
    // Guest ticks are already included in user and nice. Linux reports idle and iowait separately.
    std::array<std::uint64_t, 8> counters{};
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < counters.size(); ++i) {
        if (!(fields >> counters[i])) {
            if (!fields.eof() || i < 4) return {};
            break;
        }
        if (counters[i] > std::numeric_limits<std::uint64_t>::max() - total) return {};
        total += counters[i];
    }
    return CpuTicks{total - counters[3] - counters[4], total};
}

std::optional<Memory> read_memory() {
    std::ifstream input("/proc/meminfo");
    return parse_memory(input);
}

std::optional<CpuTicks> read_cpu() {
    std::ifstream input("/proc/stat");
    return parse_cpu(input);
}

std::optional<double> cpu_fraction(CpuTicks before, CpuTicks after) {
    if (after.total <= before.total || after.active < before.active) return {};
    const auto active = after.active - before.active;
    const auto total = after.total - before.total;
    if (active > total) return {};
    return static_cast<double>(active) / static_cast<double>(total);
}

} // namespace yrswatch
