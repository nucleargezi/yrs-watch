#include "child_process.hpp"

#include <glib.h>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>

namespace yrswatch::detail {

using namespace std::chrono_literals;

void fail(QuotaError code, std::string message) {
    throw Failure{code, std::move(message)};
}

void invalid_response() {
    fail(QuotaError::Unavailable, "额度响应格式无法识别");
}

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

void cli_failure(const std::string& text) {
    if (suggests_login(text)) fail(QuotaError::NeedsLogin, "请先在对应客户端登录订阅账户");
    fail(QuotaError::Offline, {});
}

Child::Child(const std::string& path, std::vector<const char*> args, std::stop_token stop,
             Clock::time_point deadline, const std::vector<EnvVar>& extra_env)
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
    // Never let a login flow open a browser from a background lookup.
    env = g_environ_setenv(env, "BROWSER", "/bin/false", TRUE);
    for (const auto& var : extra_env) env = g_environ_setenv(env, var.name, var.value, var.overwrite);
    if (!error) error = posix_spawn(&pid_, path.c_str(), &files, &attr, argv.data(), env);
    g_strfreev(env);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&files);
    if (error) fail(QuotaError::Unavailable, "无法启动客户端");
    pidfd_.value = static_cast<int>(syscall(SYS_pidfd_open, pid_, 0));
}

Child::~Child() {
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

void Child::finish_input() { shutdown(channel_.value, SHUT_WR); }

void Child::send_message(std::string_view message) {
    while (!message.empty()) {
        const auto count = send(channel_.value, message.data(), message.size(), MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) cli_failure(diagnostics_);
        message.remove_prefix(static_cast<size_t>(count));
    }
}

std::string Child::output(bool line) {
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
            // stdout is closed, but stderr may still carry a login hint. Drain it briefly.
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
            else if (diagnostics_.size() < 32768) diagnostics_.append(bytes, static_cast<size_t>(count));
        }
        if (fds[0].revents) {
            char bytes[8192];
            const auto count = read(channel_.value, bytes, sizeof(bytes));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) eof_ = true;
            else {
                total_ += static_cast<size_t>(count);
                if (total_ > 2 * 1024 * 1024) invalid_response();
                buffer_.append(bytes, static_cast<size_t>(count));
            }
        }
    }
}

} // namespace yrswatch::detail
