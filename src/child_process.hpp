#pragma once

// Shared plumbing for sources that query an official command-line client.

#include "providers.hpp"

#include <chrono>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

namespace yrswatch::detail {

using Clock = std::chrono::steady_clock;

// Thrown inside a fetch and converted to a QuotaResult by fetch_quota().
struct Failure { QuotaError code; std::string message; };
[[noreturn]] void fail(QuotaError code, std::string message);
[[noreturn]] void invalid_response();

// Heuristic: CLIs report a missing login in free-form English text. Extend the
// keyword list in child_process.cpp (and provider_test) when a client changes wording.
bool suggests_login(const std::string& message);
// NeedsLogin when the text looks like a login prompt, otherwise Offline.
[[noreturn]] void cli_failure(const std::string& text);

// PATH first, then the per-user install locations GNOME sessions often omit from PATH.
std::string find_cli(const char* name);

struct Fd {
    explicit Fd(int fd = -1) : value(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { close_fd(); }
    void close_fd() {
        if (value >= 0) close(std::exchange(value, -1));
    }
    int value;
};

struct EnvVar {
    const char* name;
    const char* value;
    bool overwrite = true; // false keeps a value the user already set.
};

// Both ends of stdin/stdout share a socketpair. MSG_NOSIGNAL keeps a CLI
// exiting during the handshake from changing signal behavior in the tray.
// The child runs in its own process group, which is killed as a whole on destruction.
class Child {
public:
    Child(const std::string& path, std::vector<const char*> args, std::stop_token stop,
          Clock::time_point deadline, const std::vector<EnvVar>& env = {});
    ~Child();
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    void finish_input();
    const std::string& diagnostics() const { return diagnostics_; }
    void send_message(std::string_view message);
    // With line = true, returns the next newline-terminated line; otherwise everything until EOF.
    std::string output(bool line = false);

private:
    std::stop_token stop_;
    Clock::time_point deadline_;
    Fd channel_, errors_, wake_, pidfd_;
    pid_t pid_ = -1;
    bool eof_ = false;
    std::size_t total_ = 0;
    std::string buffer_, diagnostics_;
};

} // namespace yrswatch::detail
