#include "providers.hpp"

#include <cassert>
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
    assert(chmod(path.c_str(), 0700) == 0);
}

void stopped(pid_t pid) {
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (std::getline(stat, line)) {
        auto end = line.rfind(')');
        assert(end != std::string::npos && (line[end + 2] == 'Z' || line[end + 2] == 'X'));
    }
}
} // namespace

int main() {
    char pattern[] = "/tmp/yrs-watch-provider-test-XXXXXX";
    assert(mkdtemp(pattern));
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
    assert(codex.sample && codex.sample->rows.size() == 2);
    assert(!codex.sample->account.empty() && codex.sample->account != "opaque-example");
    auto& primary = codex.sample->rows[0];
    assert(primary.id == "codex:primary" && primary.remaining == 0.75);
    assert(primary.reset_at == 2000000000 && primary.window == "5 小时");
    assert(codex.sample->rows[1].remaining == 0 && !codex.sample->rows[1].reset_at);

    script(dir / "agy", R"sh(
[ "$AGY_CLI_DISABLE_AUTO_UPDATE" = true ] || exit 9
[ "$GOMAXPROCS" = 2 ] && [ "$GOMEMLIMIT" = 128MiB ] || exit 9
if [ "$1" = "--version" ]; then printf x >> "$VERSION_PROBES"; echo 1.5.9; exit; fi
echo '{"status":"SUCCESS","num_turns":0,"command":{"name":"usage","data":{"groups":[{"name":"Gemini","buckets":[{"id":"g-week","window":"weekly","remaining_fraction":0,"reset_time":"2026-11-01T08:00:00+08:00"},{"id":"g-5h","window":"5h","disabled":true}]}]}}}'
)sh");
    auto agy = fetch_quota(Provider::Antigravity);
    assert(agy.sample && agy.sample->rows.size() == 2);
    assert(agy.sample->rows[0].remaining == 0.0);
    assert(agy.sample->rows[0].reset_at == 1793491200);
    assert(!agy.sample->rows[1].remaining && !agy.sample->rows[1].reset_at);
    assert(agy.sample->rows[1].disabled && agy.sample->rows[1].label.find("未开放") == std::string::npos);
    assert(fetch_quota(Provider::Antigravity).sample);
    assert(std::filesystem::file_size(dir / "version-probes") == 1);

    std::filesystem::remove(dir / "agy");
    script(dir / "agy", R"sh(
if [ "$1" = "--version" ]; then echo 1.1.10; exit; fi
echo called > "$PIDS"
)sh");
    assert(fetch_quota(Provider::Antigravity).error == QuotaError::Unavailable);
    assert(!std::filesystem::exists(dir / "pids"));

    // 验证带前缀版本号解析以及 Go 零值时间过滤
    std::filesystem::remove(dir / "agy");
    script(dir / "agy", R"sh(
if [ "$1" = "--version" ]; then echo "agy v1.6.0-beta.1"; exit; fi
echo '{"status":"SUCCESS","command":{"name":"usage","data":{"groups":[{"name":"Gemini","buckets":[{"id":"g-zero","window":"daily","remaining_fraction":0.5,"reset_time":"0001-01-01T00:00:00Z"}]}]}}}'
)sh");
    auto prefixed = fetch_quota(Provider::Antigravity);
    assert(prefixed.sample && prefixed.sample->rows.size() == 1 && !prefixed.sample->rows[0].reset_at);

    // 验证 Codex rateLimits 回退分支
    script(dir / "codex", R"sh(
read -r init
echo '{"id":1,"result":{}}'
read -r ready
read -r query
echo '{"id":2,"result":{"rateLimits":{"limitId":"fallback","limitName":"Fallback","primary":{"usedPercent":40,"windowDurationMins":300,"resetsAt":2000000000}}}}'
)sh");
    auto fallback = fetch_quota(Provider::Codex);
    assert(fallback.sample && fallback.sample->rows.size() == 1 && fallback.sample->rows[0].id == "fallback:primary");
    assert(fallback.sample->rows[0].remaining == 0.6);

    // 验证 stdout 立即关闭后仍能读取延迟到达的 stderr 登录诊断
    script(dir / "codex", R"sh(
exec 1>&-
/bin/sleep 0.03
echo "authentication required" >&2
exit 1
)sh");
    auto delayed_err = fetch_quota(Provider::Codex);
    assert(delayed_err.error == QuotaError::NeedsLogin && !delayed_err.sample);

    // 验证超过 2 MiB 输出被截断并报错
    script(dir / "codex", R"sh(
/bin/dd if=/dev/zero bs=65536 count=34 2>/dev/null
)sh");
    auto oversized = fetch_quota(Provider::Codex);
    assert(oversized.error == QuotaError::Unavailable && !oversized.sample);

    script(dir / "codex", R"sh(
read -r req
echo '{"id":1,"result":{}}'
read -r ready
read -r query
echo '{"id":2,"error":{"code":-32000,"message":"not logged in: private@example.invalid"}}'
)sh");
    auto unauth = fetch_quota(Provider::Codex);
    assert(unauth.error == QuotaError::NeedsLogin && !unauth.sample);
    assert(unauth.message.find("private") == std::string::npos);

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
    assert(child > 0 && grandchild > 0);
    auto start = std::chrono::steady_clock::now();
    worker.request_stop();
    worker.join();
    assert(cancelled.error == QuotaError::Cancelled);
    assert(std::chrono::steady_clock::now() - start < 2s);
    stopped(child);
    // SIGKILL delivery to an orphan may finish just after waitpid on the leader.
    std::this_thread::sleep_for(30ms);
    stopped(grandchild);

    // 验证找不到可执行文件时返回 Missing (需同时屏蔽 PATH 和 HOME 回退目录)
    setenv("PATH", empty_home.c_str(), 1);
    assert(fetch_quota(Provider::Codex).error == QuotaError::Missing);
    assert(fetch_quota(Provider::Antigravity).error == QuotaError::Missing);

    std::filesystem::remove_all(dir);
}
