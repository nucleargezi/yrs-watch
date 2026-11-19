#include "providers.hpp"

#include <glib.h>
#include <json-glib/json-glib.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <spawn.h>
#include <string_view>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <tuple>
#include <unistd.h>
#include <utility>

namespace yrswatch {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Failure { QuotaError code; std::string message; };
[[noreturn]] void fail(QuotaError code, std::string message) {
    throw Failure{code, std::move(message)};
}
[[noreturn]] void invalid_response() {
    fail(QuotaError::Unavailable, "额度响应格式无法识别");
}

struct Fd {
    explicit Fd(int fd = -1) : value(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int value;
    ~Fd() { close_fd(); }
    void close_fd() {
        if (value >= 0) close(std::exchange(value, -1));
    }
};

std::string find_cli(const char* name) {
    if (auto* from_path = g_find_program_in_path(name)) {
        std::string path(from_path);
        g_free(from_path);
        return path;
    }
    const char* home = g_get_home_dir();
    for (auto dir : {".local/bin", ".codex/packages/standalone/current/bin",
                     ".antigravity/antigravity-cli/bin"}) {
        std::string path = std::string(home) + "/" + dir + "/" + name;
        if (access(path.c_str(), X_OK) == 0) return path;
    }
    fail(QuotaError::Missing, std::string("未找到 ") + name + " 命令");
}

bool suggests_login(const std::string& message) {
    auto folded = std::unique_ptr<char, decltype(&g_free)>(
        g_ascii_strdown(message.c_str(), -1), g_free);
    const std::string_view lower(folded.get());
    for (auto text : {"not logged in", "not authenticated", "authentication required",
                      "unauthenticated", "unauthorized", "please sign in", "please log in",
                      "please login", "requires chatgpt", "sign in with chatgpt",
                      "not available for api key", "not signed in", "401 unauthorized",
                      "paste the authorization code", "open the following url"}) {
        if (lower.find(text) != std::string_view::npos) return true;
    }
    return false;
}

[[noreturn]] void cli_failure(const std::string& text) {
    if (suggests_login(text)) fail(QuotaError::NeedsLogin, "请先在对应客户端登录订阅账户");
    fail(QuotaError::Unavailable, "客户端暂时无法查询额度");
}

// Both ends of stdin/stdout share a socketpair. MSG_NOSIGNAL keeps a CLI
// exiting during the handshake from changing signal behavior in the tray.
class Child {
public:
    Child(Provider provider, const std::string& path, std::vector<const char*> args,
          std::stop_token stop, Clock::time_point deadline)
        : stop_(stop), deadline_(deadline) {
        if (stop_.stop_requested()) fail(QuotaError::Cancelled, "已取消");
        int pair[2], errors[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0)
            fail(QuotaError::Unavailable, "无法建立客户端连接");
        channel_.value = pair[0];
        Fd peer{pair[1]};
        if (pipe2(errors, O_CLOEXEC) != 0)
            fail(QuotaError::Unavailable, "无法建立客户端连接");
        errors_.value = errors[0];
        Fd error_writer{errors[1]};
        wake_.value = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (wake_.value < 0) fail(QuotaError::Unavailable, "无法监听客户端");

        std::vector<char*> argv{const_cast<char*>(path.c_str())};
        for (auto arg : args) argv.push_back(const_cast<char*>(arg));
        argv.push_back(nullptr);

        posix_spawn_file_actions_t files;
        posix_spawnattr_t attr;
        if (posix_spawn_file_actions_init(&files) != 0)
            fail(QuotaError::Unavailable, "无法启动客户端");
        if (posix_spawnattr_init(&attr) != 0) {
            posix_spawn_file_actions_destroy(&files);
            fail(QuotaError::Unavailable, "无法启动客户端");
        }
        // A quota lookup does not need project configuration or the tray's
        // file descriptors. The new process group belongs only to this fetch.
        int error = posix_spawn_file_actions_adddup2(&files, peer.value, STDIN_FILENO);
        error |= posix_spawn_file_actions_adddup2(&files, peer.value, STDOUT_FILENO);
        error |= posix_spawn_file_actions_adddup2(&files, error_writer.value, STDERR_FILENO);
        error |= posix_spawn_file_actions_addclosefrom_np(&files, 3);
        error |= posix_spawn_file_actions_addchdir_np(&files, "/");
        error |= posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        error |= posix_spawnattr_setpgroup(&attr, 0);
        gchar** env = g_get_environ();
        env = g_environ_setenv(env, "BROWSER", "/bin/false", TRUE);
        // A periodic quota lookup should not start the CLI's background updater.
        if (provider == Provider::Antigravity) {
            env = g_environ_setenv(env, "AGY_CLI_DISABLE_AUTO_UPDATE", "true", TRUE);
            // These are soft defaults for the Go runtime, not process RSS caps.
            env = g_environ_setenv(env, "GOMAXPROCS", "2", FALSE);
            env = g_environ_setenv(env, "GOMEMLIMIT", "96MiB", FALSE);
        }
        if (!error) error = posix_spawn(&pid_, path.c_str(), &files, &attr, argv.data(), env);
        g_strfreev(env);
        posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&files);
        if (error) fail(QuotaError::Unavailable, "无法启动客户端");
        pidfd_.value = static_cast<int>(syscall(SYS_pidfd_open, pid_, 0));
    }

    ~Child() {
        if (pid_ < 0) return;
        finish_input();
        // Keep the child unreaped until we have retired the entire group:
        // its PID cannot be recycled as another application's group ID.
        if (!stop_.stop_requested() && pidfd_.value >= 0) {
            pollfd exited{pidfd_.value, POLLIN, 0};
            poll(&exited, 1, 200);
        }
        kill(-pid_, SIGKILL);
        int status = 0;
        while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
    }
    Child(const Child&) = delete;
    void finish_input() { shutdown(channel_.value, SHUT_WR); }
    const std::string& diagnostics() const { return diagnostics_; }

    void send_message(std::string_view message) {
        while (!message.empty()) {
            const auto count = send(channel_.value, message.data(), message.size(), MSG_NOSIGNAL);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) cli_failure(diagnostics_);
            message.remove_prefix(static_cast<size_t>(count));
        }
    }

    std::string output(bool line = false) {
        std::stop_callback wake_on_stop(stop_, [this] {
            const uint64_t one = 1;
            while (write(wake_.value, &one, sizeof(one)) < 0 && errno == EINTR) {}
        });
        std::optional<Clock::time_point> drain_until;
        for (;;) {
            if (stop_.stop_requested()) fail(QuotaError::Cancelled, "已取消");
            if (line) {
                const auto end = buffer_.find('\n');
                if (end != std::string::npos) {
                    std::string reply = buffer_.substr(0, end);
                    buffer_.erase(0, end + 1);
                    return reply;
                }
            }
            if (eof_) {
                if (!line && !buffer_.empty()) return std::exchange(buffer_, {});
                if (errors_.value < 0) cli_failure(diagnostics_);
                // stdout 已关闭但 stderr 可能仍在写入诊断信息, 有界排空避免丢掉登录提示
                if (!drain_until) drain_until = std::min(deadline_, Clock::now() + 200ms);
            }
            const auto limit = drain_until ? *drain_until : deadline_;
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                limit - Clock::now()).count();
            if (ms <= 0) {
                if (drain_until || suggests_login(diagnostics_)) cli_failure(diagnostics_);
                fail(QuotaError::Timeout, "查询超时");
            }
            pollfd fds[] = {{eof_ ? -1 : channel_.value, POLLIN, 0}, {errors_.value, POLLIN, 0},
                            {wake_.value, POLLIN, 0}};
            int ready = poll(fds, 3, static_cast<int>(ms));
            if (ready == 0 || (ready < 0 && errno == EINTR)) continue;
            if (ready < 0) cli_failure(diagnostics_);
            if (fds[1].revents) {
                char bytes[4096];
                const auto count = read(errors_.value, bytes, sizeof(bytes));
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) errors_.close_fd();
                else if (diagnostics_.size() < 32768) diagnostics_.append(bytes, count);
            }
            if (fds[0].revents) {
                char bytes[8192];
                const auto count = read(channel_.value, bytes, sizeof(bytes));
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) eof_ = true;
                else {
                    total_ += static_cast<size_t>(count);
                    if (total_ > 2 * 1024 * 1024) invalid_response();
                    buffer_.append(bytes, count);
                }
            }
        }
    }
private:
    std::stop_token stop_;
    Clock::time_point deadline_;
    Fd channel_, errors_, wake_, pidfd_;
    pid_t pid_ = -1;
    bool eof_ = false;
    size_t total_ = 0;
    std::string buffer_, diagnostics_;
};

using Json = std::unique_ptr<JsonParser, decltype(&g_object_unref)>;

Json parse(const std::string& data, const std::string& diagnostics = {}) {
    Json parser(json_parser_new(), g_object_unref);
    if (!json_parser_load_from_data(parser.get(), data.data(), data.size(), nullptr)) {
        if (suggests_login(data)) cli_failure(data);
        if (suggests_login(diagnostics)) cli_failure(diagnostics);
        invalid_response();
    }
    return parser;
}

JsonObject* object(JsonNode* n) {
    return n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : nullptr;
}
JsonNode* get(JsonObject* obj, const char* key) {
    return obj ? json_object_get_member(obj, key) : nullptr;
}
std::string str(JsonNode* n) {
    return n && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : "";
}
std::optional<double> number(JsonNode* n) {
    if (!n) return {};
    if (json_node_get_value_type(n) == G_TYPE_INT64) return json_node_get_int(n);
    if (json_node_get_value_type(n) == G_TYPE_DOUBLE) return json_node_get_double(n);
    return {};
}
JsonArray* array(JsonNode* n) {
    return n && JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n) : nullptr;
}
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
        auto* body = object(json_parser_get_root(reply.get()));
        if (number(get(body, "id")) != expected) continue; // Asynchronous notification.
        if (auto* error = object(get(body, "error"))) cli_failure(str(get(error, "message")));
        if (!object(get(body, "result"))) invalid_response();
        return reply;
    }
}

QuotaSample fetch_codex(Child& child) {
    child.send_message(R"({"id":1,"method":"initialize","params":{"clientInfo":{"name":"yrs_watch","title":"Yrs Watch","version":"0.1"}}})" "\n");
    rpc_reply(child, 1);
    child.send_message(R"({"method":"initialized"})" "\n" R"({"id":2,"method":"account/rateLimits/read"})" "\n");
    auto reply = rpc_reply(child, 2);
    child.finish_input();
    auto* body = object(json_parser_get_root(reply.get()));
    auto* result = object(get(body, "result"));
    QuotaSample sample;
    sample.provider = Provider::Codex;
    auto account = str(get(result, "accountId"));
    if (!account.empty()) {
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

QuotaSample antigravity_data(const std::string& output, const std::string& diagnostics = {}) {
    auto reply = parse(output, diagnostics);
    auto* envelope = object(json_parser_get_root(reply.get()));
    if (str(get(envelope, "status")) != "SUCCESS")
        cli_failure(str(get(envelope, "error")) + " " + str(get(envelope, "response")) + " " + diagnostics);
    auto* command = object(get(envelope, "command"));
    auto* data = object(get(command, "data"));
    auto* groups = array(get(data, "groups"));
    if (!groups) invalid_response();
    QuotaSample sample;
    sample.provider = Provider::Antigravity;
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
    Child version(Provider::Antigravity, path, {"--version"}, stop, deadline);
    version.finish_input();
    if (!has_read_only_usage(version.output()))
        fail(QuotaError::Unavailable, "请更新 Antigravity CLI 以读取额度");
    std::lock_guard guard(lock);
    verified = revision;
}

} // namespace

QuotaResult fetch_quota(Provider provider, std::stop_token stop) {
    try {
        if (stop.stop_requested()) fail(QuotaError::Cancelled, "已取消");
        const bool codex = provider == Provider::Codex;
        const auto path = find_cli(codex ? "codex" : "agy");
        auto deadline = Clock::now() + (codex ? 20s : 45s);
        QuotaSample sample;
        if (codex) {
            Child child(provider, path, {"app-server", "--listen", "stdio://"}, stop, deadline);
            sample = fetch_codex(child);
        } else {
            check_agy_version(path, stop, deadline);
            Child child(provider, path, {"--settings", R"({"disableSlashCommands":false})",
                               "--output-format", "json", "-p", "/usage"}, stop, deadline);
            child.finish_input();
            sample = antigravity_data(child.output(), child.diagnostics());
        }
        if (sample.rows.empty()) fail(QuotaError::Unavailable, "客户端未返回额度窗口");
        sample.observed_at = now_epoch();
        return {std::move(sample), QuotaError::None, {}};
    } catch (const Failure& f) {
        return {{}, f.code, f.message};
    }
}

} // namespace yrswatch
