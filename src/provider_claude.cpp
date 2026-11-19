// Claude Code: reads the snapshot that yrs-watch-statusline writes from the
// statusline input. Claude Code offers no read-only, non-interactive usage command.

#include "child_process.hpp"
#include "json_util.hpp"
#include "provider_impl.hpp"
#include "snapshot.hpp"

#include <algorithm>
#include <system_error>

namespace yrswatch::detail {
namespace {

int window_rank(const std::string& key) {
    if (key == "five_hour") return 0;
    if (key == "seven_day") return 1;
    return 2;
}

std::string window_label(const std::string& key) {
    if (key == "five_hour") return "5 小时";
    if (key == "seven_day") return "每周";
    return key;
}

} // namespace

QuotaSample fetch_claude(std::stop_token) {
    const auto path = claude_snapshot_path();
    std::error_code error;
    if (!std::filesystem::exists(path, error)) fail(QuotaError::Missing, "尚无 Claude Code 快照");
    const auto data = read_private_file(path, max_snapshot_size);
    if (!data) fail(QuotaError::Unavailable, "Claude 快照不可读 (需为本人所有且权限为 0600)");
    const auto json = parse(*data);
    auto* root = root_object(json);
    const auto version = number(get(root, "version"));
    const auto observed = number(get(root, "observed_at"));
    auto* windows = object(get(root, "windows"));
    if (version != 1.0 || !observed || *observed <= 0 || !windows) invalid_response();

    QuotaSample sample;
    // The snapshot time, not the read time: an idle Claude Code leaves an old reading.
    sample.observed_at = static_cast<Epoch>(*observed);
    JsonObjectIter iter;
    json_object_iter_init(&iter, windows);
    const gchar* name;
    JsonNode* node;
    while (json_object_iter_next(&iter, &name, &node)) {
        auto* window = object(node);
        const auto used = number(get(window, "used_percentage"));
        if (!used) continue;
        QuotaRow row;
        row.id = std::string("claude:") + name;
        row.group = "Claude";
        row.window = window_label(name);
        row.label = "Claude · " + row.window;
        if (*used >= 0 && *used <= 100) row.remaining = (100 - *used) / 100;
        if (const auto reset = number(get(window, "resets_at")); reset && *reset > 0)
            row.reset_at = static_cast<Epoch>(*reset);
        sample.rows.push_back(std::move(row));
    }
    // JSON object members carry no reliable order.
    std::ranges::sort(sample.rows, [](const QuotaRow& a, const QuotaRow& b) {
        const auto ka = a.id.substr(7), kb = b.id.substr(7);
        return std::pair(window_rank(ka), ka) < std::pair(window_rank(kb), kb);
    });
    return sample;
}

} // namespace yrswatch::detail
