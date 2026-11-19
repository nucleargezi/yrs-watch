// Antigravity: `agy -p /usage --output-format json`, guarded by a version check.

#include "child_process.hpp"
#include "json_util.hpp"
#include "provider_impl.hpp"

#include <array>
#include <charconv>
#include <mutex>
#include <optional>
#include <sys/stat.h>
#include <tuple>

namespace yrswatch::detail {
namespace {

using namespace std::chrono_literals;

// A periodic lookup should not start the CLI's background updater. The Go
// settings are soft defaults for the runtime, not process RSS caps.
const std::vector<EnvVar> agy_env{
    {"AGY_CLI_DISABLE_AUTO_UPDATE", "true", true},
    {"GOMAXPROCS", "2", false},
    {"GOMEMLIMIT", "96MiB", false},
};

QuotaSample antigravity_data(const std::string& output, const std::string& diagnostics = {}) {
    auto reply = parse(output, diagnostics);
    auto* envelope = root_object(reply);
    if (str(get(envelope, "status")) != "SUCCESS")
        cli_failure(str(get(envelope, "error")) + " " + str(get(envelope, "response")) + " " + diagnostics);
    auto* command = object(get(envelope, "command"));
    auto* data = object(get(command, "data"));
    auto* groups = array(get(data, "groups"));
    if (!groups) invalid_response();
    QuotaSample sample;
    for (guint g = 0; g < json_array_get_length(groups); ++g) {
        auto* group = object(json_array_get_element(groups, g));
        auto* buckets = array(get(group, "buckets"));
        if (!buckets) continue;
        for (guint i = 0; i < json_array_get_length(buckets); ++i) {
            auto* bucket = object(json_array_get_element(buckets, i));
            QuotaRow row;
            row.id = str(get(bucket, "id"));
            if (row.id.empty()) continue;
            row.group = str(get(group, "name"));
            row.window = str(get(bucket, "window"));
            if (row.window == "weekly") row.window = "每周";
            else if (row.window == "5h") row.window = "5 小时";
            row.label = row.group + (row.window.empty() ? "" : " · " + row.window);
            if (row.label.empty()) row.label = row.id;
            auto* disabled = get(bucket, "disabled");
            if (disabled && json_node_get_value_type(disabled) == G_TYPE_BOOLEAN &&
                json_node_get_boolean(disabled)) {
                row.disabled = true;
                sample.rows.push_back(std::move(row));
                continue;
            }
            auto fraction = number(get(bucket, "remaining_fraction"));
            if (fraction && *fraction >= 0 && *fraction <= 1) row.remaining = fraction;
            auto amount = number(get(bucket, "remaining_amount"));
            if (amount && *amount >= 0) {
                row.quantity = amount;
                row.unit = str(get(bucket, "unit"));
            }
            auto iso = str(get(bucket, "reset_time"));
            if (auto* time = g_date_time_new_from_iso8601(iso.c_str(), nullptr)) {
                if (const Epoch unix_time = g_date_time_to_unix(time); unix_time > 0) row.reset_at = unix_time;
                g_date_time_unref(time);
            }
            sample.rows.push_back(std::move(row));
        }
    }
    return sample;
}

bool has_read_only_usage(const std::string& version) {
    const char* const begin = version.data();
    const char* const end = begin + version.size();
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    for (const char* p = begin; p < end; ++p) {
        if (!is_digit(*p)) continue;
        if (p > begin && (p[-1] == '-' || p[-1] == '.' || is_digit(p[-1]))) continue;
        unsigned major = 0, minor = 0, patch = 0;
        auto [after_major, ec1] = std::from_chars(p, end, major);
        if (ec1 != std::errc{} || after_major >= end || *after_major != '.') continue;
        auto [after_minor, ec2] = std::from_chars(after_major + 1, end, minor);
        if (ec2 != std::errc{} || after_minor >= end || *after_minor != '.') continue;
        auto [after_patch, ec3] = std::from_chars(after_minor + 1, end, patch);
        if (ec3 != std::errc{}) continue;
        return std::array{major, minor, patch} >= std::array{1u, 1u, 11u};
    }
    return false;
}

void check_agy_version(const std::string& path, std::stop_token stop, Clock::time_point deadline) {
    // Pre-1.1.11 builds could dispatch /usage to the model. The positive
    // check belongs to an executable revision and is independent of account.
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) fail(QuotaError::Missing, "未找到 agy 命令");
    auto revision = std::tuple{st.st_dev, st.st_ino, st.st_size,
                               st.st_ctim.tv_sec, st.st_ctim.tv_nsec};
    static std::mutex lock;
    static std::optional<decltype(revision)> verified;
    {
        std::lock_guard guard(lock);
        if (verified == revision) return;
    }
    Child version(path, {"--version"}, stop, deadline, agy_env);
    version.finish_input();
    if (!has_read_only_usage(version.output()))
        fail(QuotaError::Unavailable, "请更新 Antigravity CLI 以读取额度");
    std::lock_guard guard(lock);
    verified = revision;
}

} // namespace

QuotaSample fetch_antigravity(std::stop_token stop) {
    const auto path = find_cli("agy");
    const auto deadline = Clock::now() + 45s;
    check_agy_version(path, stop, deadline);
    Child child(path, {"--settings", R"({"disableSlashCommands":false})",
                       "--output-format", "json", "-p", "/usage"}, stop, deadline, agy_env);
    child.finish_input();
    return antigravity_data(child.output(), child.diagnostics());
}

} // namespace yrswatch::detail
