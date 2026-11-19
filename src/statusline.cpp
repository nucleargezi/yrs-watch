// yrs-watch-statusline: a Claude Code statusLine command.
//
// Claude Code pipes a JSON description of the session to the statusline
// command. This helper keeps only the subscription usage windows
// (rate_limits.*.used_percentage / resets_at) in a private snapshot file for
// yrs-watch, then prints a status line, or hands the untouched input to the
// user's previous statusline command with --chain.

#include "core.hpp"
#include "snapshot.hpp"

#include <json-glib/json-glib.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace yrswatch;

namespace {

constexpr std::size_t max_input = 1024 * 1024;
// Rewrite an unchanged snapshot at most this often, to refresh its timestamp.
constexpr Epoch refresh_interval = 60;

using Parser = std::unique_ptr<JsonParser, decltype(&g_object_unref)>;

struct Window {
    std::string key;
    double used;
    std::optional<Epoch> resets_at;
    bool operator==(const Window&) const = default;
};

JsonObject* member_object(JsonObject* object, const char* key) {
    JsonNode* node = object ? json_object_get_member(object, key) : nullptr;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : nullptr;
}

std::optional<double> member_number(JsonObject* object, const char* key) {
    JsonNode* node = object ? json_object_get_member(object, key) : nullptr;
    if (!node || !JSON_NODE_HOLDS_VALUE(node)) return {};
    const GType type = json_node_get_value_type(node);
    if (type == G_TYPE_INT64) return static_cast<double>(json_node_get_int(node));
    if (type == G_TYPE_DOUBLE) return json_node_get_double(node);
    return {};
}

std::string member_string(JsonObject* object, const char* key) {
    JsonNode* node = object ? json_object_get_member(object, key) : nullptr;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
        ? json_node_get_string(node) : "";
}

// Every member of `limits` that reports a usage percentage, sorted by key.
std::vector<Window> windows_of(JsonObject* limits) {
    std::vector<Window> windows;
    if (!limits) return windows;
    JsonObjectIter iter;
    json_object_iter_init(&iter, limits);
    const gchar* key;
    JsonNode* node;
    while (json_object_iter_next(&iter, &key, &node)) {
        auto* window = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : nullptr;
        const auto used = member_number(window, "used_percentage");
        if (!used || !std::isfinite(*used) || *used < 0 || *used > 100) continue;
        std::optional<Epoch> reset;
        if (const auto at = member_number(window, "resets_at"); at && std::isfinite(*at) && *at > 0)
            reset = static_cast<Epoch>(*at);
        windows.push_back({key, *used, reset});
    }
    std::ranges::sort(windows, {}, &Window::key);
    return windows;
}

Parser parse(const std::string& text) {
    Parser parser(json_parser_new(), g_object_unref);
    if (!json_parser_load_from_data(parser.get(), text.data(), static_cast<gssize>(text.size()), nullptr))
        return Parser(nullptr, g_object_unref);
    return parser;
}

JsonObject* root(const Parser& parser) {
    JsonNode* node = parser ? json_parser_get_root(parser.get()) : nullptr;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : nullptr;
}

std::string snapshot_json(const std::vector<Window>& windows, Epoch observed) {
    g_autoptr(JsonBuilder) builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "version");
    json_builder_add_int_value(builder, 1);
    json_builder_set_member_name(builder, "observed_at");
    json_builder_add_int_value(builder, observed);
    json_builder_set_member_name(builder, "windows");
    json_builder_begin_object(builder);
    for (const auto& window : windows) {
        json_builder_set_member_name(builder, window.key.c_str());
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "used_percentage");
        json_builder_add_double_value(builder, window.used);
        if (window.resets_at) {
            json_builder_set_member_name(builder, "resets_at");
            json_builder_add_int_value(builder, *window.resets_at);
        }
        json_builder_end_object(builder);
    }
    json_builder_end_object(builder);
    json_builder_end_object(builder);
    g_autoptr(JsonNode) node = json_builder_get_root(builder);
    g_autoptr(JsonGenerator) generator = json_generator_new();
    json_generator_set_root(generator, node);
    g_autofree gchar* text = json_generator_to_data(generator, nullptr);
    return std::string(text) + "\n";
}

void store(const std::vector<Window>& windows) {
    const auto path = claude_snapshot_path();
    const Epoch now = now_epoch();
    if (const auto previous = read_private_file(path, max_snapshot_size)) {
        const auto parsed = parse(*previous);
        auto* object = root(parsed);
        const auto observed = member_number(object, "observed_at");
        const Epoch at = observed ? static_cast<Epoch>(*observed) : 0;
        if (at > 0 && now >= at && now - at < refresh_interval &&
            windows_of(member_object(object, "windows")) == windows) return;
    }
    write_private_file(path, snapshot_json(windows, now));
}

std::string percent(double value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.0f%%", value);
    return text;
}

std::string status_line(JsonObject* input, const std::vector<Window>& windows) {
    std::string line = member_string(member_object(input, "model"), "display_name");
    for (const auto& window : windows) {
        const std::string name = window.key == "five_hour" ? "5h" : window.key == "seven_day" ? "7d" : window.key;
        if (!line.empty()) line += " · ";
        line += name + " " + percent(100 - window.used);
    }
    return line;
}

// Runs the previous statusline command with the original input; its output goes straight to ours.
int chain(const char* command, const std::string& input) {
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) return 1;
    const pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        dup2(pipe_fds[0], STDIN_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        execl("/bin/sh", "sh", "-c", command, static_cast<char*>(nullptr));
        _exit(127);
    }
    close(pipe_fds[0]);
    std::signal(SIGPIPE, SIG_IGN); // The command may ignore its input.
    std::size_t done = 0;
    while (done < input.size()) {
        const ssize_t n = write(pipe_fds[1], input.data() + done, input.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += static_cast<std::size_t>(n);
    }
    close(pipe_fds[1]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

} // namespace

int main(int argc, char** argv) {
    const char* chained = nullptr;
    if (argc == 3 && std::string(argv[1]) == "--chain") chained = argv[2];
    else if (argc != 1) {
        std::cerr << "usage: yrs-watch-statusline [--chain COMMAND]\n";
        return 2;
    }

    std::string input;
    char buffer[8192];
    while (input.size() <= max_input) {
        const auto n = std::fread(buffer, 1, sizeof(buffer), stdin);
        if (n == 0) break;
        input.append(buffer, n);
    }
    if (input.size() > max_input) input.clear();

    const auto parsed = parse(input);
    auto* object = root(parsed);
    const auto windows = windows_of(member_object(object, "rate_limits"));
    // Without rate limits (API key or gateway logins) there is nothing to record.
    if (!windows.empty()) store(windows);

    if (chained) return chain(chained, input);
    std::cout << status_line(object, windows) << '\n';
    return 0;
}
