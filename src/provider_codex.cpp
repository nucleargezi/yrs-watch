// Codex: JSON-RPC over `codex app-server --listen stdio://`, method account/rateLimits/read.

#include "child_process.hpp"
#include "json_util.hpp"
#include "provider_impl.hpp"

#include "version.hpp"

#include <cmath>

namespace yrswatch::detail {
namespace {

using namespace std::chrono_literals;

std::string duration_label(std::optional<double> mins, bool primary) {
    if (!mins || !std::isfinite(*mins) || *mins < 1 || *mins > 52596000) return primary ? "主窗口" : "次窗口";
    auto minutes = static_cast<int>(*mins);
    if (minutes == 10080) return "每周";
    if (minutes % 1440 == 0) return std::to_string(minutes / 1440) + " 天";
    if (minutes % 60 == 0) return std::to_string(minutes / 60) + " 小时";
    return std::to_string(minutes) + " 分钟";
}

void codex_bucket(QuotaSample& sample, std::string id, JsonObject* bucket) {
    if (!bucket) return;
    if (id.empty()) id = "codex";
    auto group = str(get(bucket, "limitName"));
    if (group.empty()) group = id == "codex" ? "Codex" : id;
    for (const char* key : {"primary", "secondary"}) {
        auto* window = object(get(bucket, key));
        if (!window) continue;
        QuotaRow row;
        row.group = group;
        row.window = duration_label(number(get(window, "windowDurationMins")), key[0] == 'p');
        row.label = group + " · " + row.window;
        row.id = id + ":" + key;
        auto used = number(get(window, "usedPercent"));
        if (used && *used >= 0 && *used <= 100) row.remaining = (100 - *used) / 100;
        if (auto* reset = get(window, "resetsAt");
            reset && json_node_get_value_type(reset) == G_TYPE_INT64 && json_node_get_int(reset) > 0)
            row.reset_at = json_node_get_int(reset);
        sample.rows.push_back(std::move(row));
    }
}

Json rpc_reply(Child& child, int expected) {
    for (;;) {
        auto reply = parse(child.output(true), child.diagnostics());
        auto* body = root_object(reply);
        if (number(get(body, "id")) != expected) continue; // Asynchronous notification.
        if (auto* error = object(get(body, "error"))) cli_failure(str(get(error, "message")));
        if (!object(get(body, "result"))) invalid_response();
        return reply;
    }
}

} // namespace

QuotaSample fetch_codex(std::stop_token stop) {
    const auto path = find_cli("codex");
    Child child(path, {"app-server", "--listen", "stdio://"}, stop, Clock::now() + 20s);
    const std::string initialize =
        std::string(R"({"id":1,"method":"initialize","params":{"clientInfo":{"name":"yrs_watch","title":"Yrs Watch","version":")") +
        version + "\"}}}\n";
    child.send_message(initialize);
    rpc_reply(child, 1);
    child.send_message(R"({"method":"initialized"})" "\n" R"({"id":2,"method":"account/rateLimits/read"})" "\n");
    auto reply = rpc_reply(child, 2);
    child.finish_input();
    auto* result = object(get(root_object(reply), "result"));
    QuotaSample sample;
    auto account = str(get(result, "accountId"));
    if (!account.empty()) {
        // Only a digest is kept, so the cache never holds the raw account identifier.
        gchar* hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, account.c_str(), -1);
        sample.account = hash;
        g_free(hash);
    }
    if (auto* buckets = object(get(result, "rateLimitsByLimitId"))) {
        JsonObjectIter iter;
        json_object_iter_init(&iter, buckets);
        const gchar* id;
        JsonNode* bucket;
        while (json_object_iter_next(&iter, &id, &bucket)) codex_bucket(sample, id, object(bucket));
    }
    if (sample.rows.empty()) {
        auto* bucket = object(get(result, "rateLimits"));
        codex_bucket(sample, str(get(bucket, "limitId")), bucket);
    }
    return sample;
}

} // namespace yrswatch::detail
