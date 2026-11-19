#include "providers.hpp"

#include "check.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <thread>

using namespace yrswatch;
using namespace std::chrono_literals;

namespace {
void script(const std::filesystem::path& path, std::string_view body) {
    std::ofstream file(path);
    file << "#!/bin/sh\n" << body;
    file.close();
    CHECK(chmod(path.c_str(), 0700) == 0);
}

void stopped(pid_t pid) {
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (std::getline(stat, line)) {
        auto end = line.rfind(')');
        CHECK(end != std::string::npos && (line[end + 2] == 'Z' || line[end + 2] == 'X'));
    }
}
} // namespace

int main() {
    char pattern[] = "/tmp/yrs-watch-provider-test-XXXXXX";
    CHECK(mkdtemp(pattern));
    std::filesystem::path dir(pattern);
    std::filesystem::path empty_home = dir / "empty-home";
    std::filesystem::create_directory(empty_home);
    setenv("HOME", empty_home.c_str(), 1);
    setenv("PATH", pattern, 1);
    setenv("PIDS", (dir / "pids").c_str(), 1);
    setenv("VERSION_PROBES", (dir / "version-probes").c_str(), 1);
    unsetenv("AGY_CLI_DISABLE_AUTO_UPDATE");
    unsetenv("GOMAXPROCS");
    setenv("GOMEMLIMIT", "128MiB", 1);

    script(dir / "codex", R"sh(
[ -z "$AGY_CLI_DISABLE_AUTO_UPDATE" ] || exit 9
[ -z "$GOMAXPROCS" ] || exit 9
read -r init
case "$init" in *initialize*) ;; *) exit 9;; esac
echo '{"id":1,"result":{}}'
read -r ready
read -r query
case "$ready/$query" in *initialized*account/rateLimits/read*) ;; *) exit 9;; esac
echo '{"method":"another/notification"}'
echo '{"id":2,"result":{"accountId":"opaque-example","rateLimits":{"primary":{"usedPercent":99}},"rateLimitsByLimitId":{"codex":{"primary":{"usedPercent":25,"windowDurationMins":300,"resetsAt":2000000000},"secondary":{"usedPercent":100,"windowDurationMins":10080,"resetsAt":null}}}}}'
)sh");
    auto codex = fetch_quota(Provider::Codex);
    CHECK(codex.sample && codex.sample->rows.size() == 2);
    CHECK(!codex.sample->account.empty() && codex.sample->account != "opaque-example");
    auto& primary = codex.sample->rows[0];
    CHECK(primary.id == "codex:primary" && primary.remaining == 0.75);
    CHECK(primary.reset_at == 2000000000 && primary.window == "5 小时");
    CHECK(codex.sample->rows[1].remaining == 0 && !codex.sample->rows[1].reset_at);

    script(dir / "agy", R"sh(
[ "$AGY_CLI_DISABLE_AUTO_UPDATE" = true ] || exit 9
[ "$GOMAXPROCS" = 2 ] && [ "$GOMEMLIMIT" = 128MiB ] || exit 9
if [ "$1" = "--version" ]; then printf x >> "$VERSION_PROBES"; echo 1.5.9; exit; fi
echo '{"status":"SUCCESS","num_turns":0,"command":{"name":"usage","data":{"groups":[{"name":"Gemini","buckets":[{"id":"g-week","window":"weekly","remaining_fraction":0,"reset_time":"2026-11-01T08:00:00+08:00"},{"id":"g-5h","window":"5h","disabled":true}]}]}}}'
)sh");
    auto agy = fetch_quota(Provider::Antigravity);
    CHECK(agy.sample && agy.sample->rows.size() == 2);
    CHECK(agy.sample->rows[0].remaining == 0.0);
    CHECK(agy.sample->rows[0].reset_at == 1793491200);
    CHECK(!agy.sample->rows[1].remaining && !agy.sample->rows[1].reset_at);
    CHECK(agy.sample->rows[1].disabled && agy.sample->rows[1].label.find("未开放") == std::string::npos);
    CHECK(fetch_quota(Provider::Antigravity).sample);
    CHECK(std::filesystem::file_size(dir / "version-probes") == 1);

    std::filesystem::remove(dir / "agy");
    script(dir / "agy", R"sh(
if [ "$1" = "--version" ]; then echo 1.1.10; exit; fi
echo called > "$PIDS"
)sh");
    CHECK(fetch_quota(Provider::Antigravity).error == QuotaError::Unavailable);
    CHECK(!std::filesystem::exists(dir / "pids"));

    // Prefixed version strings parse, and Go zero timestamps are ignored.
    std::filesystem::remove(dir / "agy");
    script(dir / "agy", R"sh(
if [ "$1" = "--version" ]; then echo "agy v1.6.0-beta.1"; exit; fi
echo '{"status":"SUCCESS","command":{"name":"usage","data":{"groups":[{"name":"Gemini","buckets":[{"id":"g-zero","window":"daily","remaining_fraction":0.5,"reset_time":"0001-01-01T00:00:00Z"}]}]}}}'
)sh");
    auto prefixed = fetch_quota(Provider::Antigravity);
    CHECK(prefixed.sample && prefixed.sample->rows.size() == 1 && !prefixed.sample->rows[0].reset_at);

    // Codex falls back to rateLimits when no per-limit buckets exist.
    script(dir / "codex", R"sh(
read -r init
echo '{"id":1,"result":{}}'
read -r ready
read -r query
echo '{"id":2,"result":{"rateLimits":{"limitId":"fallback","limitName":"Fallback","primary":{"usedPercent":40,"windowDurationMins":300,"resetsAt":2000000000}}}}'
)sh");
    auto fallback = fetch_quota(Provider::Codex);
    CHECK(fallback.sample && fallback.sample->rows.size() == 1 && fallback.sample->rows[0].id == "fallback:primary");
    CHECK(fallback.sample->rows[0].remaining == 0.6);

    // A login hint arriving on stderr after stdout closed is still classified.
    script(dir / "codex", R"sh(
exec 1>&-
/bin/sleep 0.03
echo "authentication required" >&2
exit 1
)sh");
    auto delayed_err = fetch_quota(Provider::Codex);
    CHECK(delayed_err.error == QuotaError::NeedsLogin && !delayed_err.sample);

    // Output above 2 MiB is rejected.
    script(dir / "codex", R"sh(
/bin/dd if=/dev/zero bs=65536 count=34 2>/dev/null
)sh");
    auto oversized = fetch_quota(Provider::Codex);
    CHECK(oversized.error == QuotaError::Unavailable && !oversized.sample);

    script(dir / "codex", R"sh(
read -r req
echo '{"id":1,"result":{}}'
read -r ready
read -r query
echo '{"id":2,"error":{"code":-32000,"message":"not logged in: private@example.invalid"}}'
)sh");
    auto unauth = fetch_quota(Provider::Codex);
    CHECK(unauth.error == QuotaError::NeedsLogin && !unauth.sample);
    CHECK(unauth.message.find("private") == std::string::npos);

    // A failed lookup that is not about login is a neutral offline state.
    script(dir / "codex", R"sh(
read -r req
echo '{"id":1,"result":{}}'
read -r ready
read -r query
echo '{"id":2,"error":{"code":-32000,"message":"network unavailable"}}'
)sh");
    auto offline = fetch_quota(Provider::Codex);
    CHECK(offline.error == QuotaError::Offline && !offline.sample);

    // Notices printed before the JSON document are skipped.
    std::filesystem::remove(dir / "agy");
    script(dir / "agy", R"sh(
if [ "$1" = "--version" ]; then echo 1.6.0; exit; fi
echo 'Update available: 1.7.0'
echo '{ not json either'
echo '{"status":"SUCCESS","command":{"data":{"groups":[{"name":"Gemini","buckets":[{"id":"g-week","window":"weekly","remaining_fraction":0.5}]}]}}}'
)sh");
    auto noisy = fetch_quota(Provider::Antigravity);
    CHECK(noisy.sample && noisy.sample->rows.size() == 1 && noisy.sample->rows[0].remaining == 0.5);

    script(dir / "codex", R"sh(
/bin/sleep 60 &
echo "$$ $!" > "$PIDS"
wait
)sh");
    QuotaResult cancelled;
    std::jthread worker([&](std::stop_token stop){ cancelled = fetch_quota(Provider::Codex, stop); });
    pid_t child = -1, grandchild = -1;
    for (unsigned i = 0; i < 200 && grandchild <= 0; ++i) {
        std::this_thread::sleep_for(5ms);
        std::ifstream(dir / "pids") >> child >> grandchild;
    }
    CHECK(child > 0 && grandchild > 0);
    auto start = std::chrono::steady_clock::now();
    worker.request_stop();
    worker.join();
    CHECK(cancelled.error == QuotaError::Cancelled);
    CHECK(std::chrono::steady_clock::now() - start < 2s);
    stopped(child);
    // SIGKILL delivery to an orphan may finish just after waitpid on the leader.
    std::this_thread::sleep_for(30ms);
    stopped(grandchild);

    // A missing executable reports Missing (both PATH and HOME fallbacks are hidden).
    setenv("PATH", empty_home.c_str(), 1);
    CHECK(fetch_quota(Provider::Codex).error == QuotaError::Missing);
    CHECK(fetch_quota(Provider::Antigravity).error == QuotaError::Missing);

    std::filesystem::remove_all(dir);
    std::puts("provider tests passed");
}
