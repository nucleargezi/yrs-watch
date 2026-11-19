#include "desktop.hpp"

#include <glib.h>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
namespace yrswatch {
namespace {
constexpr const char* group = "Desktop Entry";
constexpr const char* filename = "io.github.yorisou.YrsWatch.desktop";
fs::path autostart_path() { return fs::path(g_get_user_config_dir()) / "autostart" / filename; }
using KeyFile = std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)>;

// The Exec value has desktop-entry argument quoting in addition to key-file escaping.
std::string quoted_exec(const std::string& path) {
    std::string result = "\"";
    for (char c : path) {
        if (c == '%') result += '%';
        if (c == '\\' || c == '"' || c == '$' || c == '`') result += '\\';
        result += c;
    }
    return result + "\"";
}
}

bool autostart_enabled() {
    KeyFile file(g_key_file_new(), &g_key_file_unref);
    if (!g_key_file_load_from_file(file.get(), autostart_path().c_str(), G_KEY_FILE_NONE, nullptr))
        return false;
    if (g_key_file_get_boolean(file.get(), group, "Hidden", nullptr)) return false;
    return !g_key_file_has_key(file.get(), group, "X-GNOME-Autostart-enabled", nullptr) ||
           g_key_file_get_boolean(file.get(), group, "X-GNOME-Autostart-enabled", nullptr);
}

bool set_autostart(bool enabled) {
    const auto path = autostart_path();
    if (g_mkdir_with_parents(path.parent_path().c_str(), 0700) < 0) return false;
    std::error_code error;
    auto exe = fs::read_symlink("/proc/self/exe", error);
    if (error) return false;
    // After an in-place reinstall, /proc/self/exe may carry a " (deleted)" suffix.
    constexpr std::string_view deleted_suffix = " (deleted)";
    auto exe_str = exe.string();
    if (exe_str.ends_with(deleted_suffix) && !fs::exists(exe, error)) {
        fs::path stripped(exe_str.substr(0, exe_str.size() - deleted_suffix.size()));
        error.clear();
        if (!fs::exists(stripped, error) || error) return false;
        exe = std::move(stripped);
    }
    KeyFile file(g_key_file_new(), &g_key_file_unref);
    g_key_file_set_string(file.get(), group, "Type", "Application");
    g_key_file_set_string(file.get(), group, "Name", "yrs-watch");
    g_key_file_set_string(file.get(), group, "Comment", "AI quotas and system usage in the top bar");
    g_key_file_set_locale_string(file.get(), group, "Comment", "zh_CN", "在顶栏查看 AI 额度和系统用量");
    g_key_file_set_string(file.get(), group, "Exec", quoted_exec(exe.string()).c_str());
    g_key_file_set_string(file.get(), group, "Icon", "power-profile-performance-symbolic");
    g_key_file_set_boolean(file.get(), group, "Terminal", false);
    g_key_file_set_string(file.get(), group, "Categories", "System;Monitor;");
    g_key_file_set_boolean(file.get(), group, "StartupNotify", false);
    g_key_file_set_boolean(file.get(), group, "Hidden", !enabled);
    gsize length = 0;
    gchar* contents = g_key_file_to_data(file.get(), &length, nullptr);
    const bool ok = g_file_set_contents_full(path.c_str(), contents, length,
        G_FILE_SET_CONTENTS_CONSISTENT, 0600, nullptr);
    g_free(contents);
    return ok;
}

} // namespace yrswatch
