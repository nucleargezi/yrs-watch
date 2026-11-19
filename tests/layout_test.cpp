#include "quota_layout.hpp"

#include <gio/gio.h>

#include "check.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace yrswatch;
namespace fs = std::filesystem;

namespace {

fs::path helper_path() {
    std::error_code error;
    auto exe = fs::read_symlink("/proc/self/exe", error);
    CHECK(!error);
    auto helper = exe.parent_path() / "yrs-watch-font-metrics";
    CHECK(fs::exists(helper));
    return helper;
}

std::vector<std::pair<int, std::string>> run_helper(const fs::path& helper, const char* font_name,
                                                    const std::string& input) {
    g_autoptr(GError) error = nullptr;
    const auto flags = static_cast<GSubprocessFlags>(
        G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    g_autoptr(GSubprocess) proc = g_subprocess_new(flags, &error, helper.c_str(), font_name, "1", nullptr);
    CHECK(proc && !error);
    g_autofree char* out = nullptr;
    const bool ok = g_subprocess_communicate_utf8(proc, input.c_str(), nullptr, &out, nullptr, &error);
    CHECK(ok && out && !error);
    CHECK(g_subprocess_get_successful(proc));

    std::vector<std::pair<int, std::string>> lines;
    std::istringstream stream(out);
    std::string line;
    while (std::getline(stream, line)) {
        const auto tab = line.find('\t');
        CHECK(tab != std::string::npos);
        const int stops = std::stoi(line.substr(0, tab));
        std::string caption = line.substr(tab + 1);
        CHECK(g_utf8_validate(caption.c_str(), static_cast<gssize>(caption.size()), nullptr));
        lines.emplace_back(stops, std::move(caption));
    }
    return lines;
}

void test_font_helper(const fs::path& helper) {
    // 1. A font name without a size must not degrade to step = 1.
    auto no_size = run_helper(helper, "Adwaita Sans", "Codex\nAntigravity\n");
    CHECK(no_size.size() == 2);
    CHECK(no_size[0].first > 1 && no_size[1].first >= no_size[0].first);

    // 2. Invalid UTF-8 is sanitized; long Latin and CJK model names are elided in the middle.
    const std::string invalid_utf8 = std::string("\xff\xfe\xf0", 3) + "\n";
    const std::string long_ascii = "Very-Long-Experimental-Model-Name-Alpha-2026\n";
    const std::string long_cjk = "超长测试模型名称用于验证多字节字符中间省略逻辑AlphaBeta2026\n";
    auto mixed = run_helper(helper, "Adwaita Sans 11", invalid_utf8 + long_ascii + long_cjk);
    CHECK(mixed.size() == 3);
    CHECK(mixed[0].first >= 1 && !mixed[0].second.empty());
    CHECK(mixed[1].second.find("…") != std::string::npos);
    CHECK(mixed[2].second.find("…") != std::string::npos);

    auto check_caps = run_helper(helper, "Adwaita Sans 11", mixed[1].second + "\n" + mixed[2].second + "\n");
    CHECK(check_caps.size() == 2 && check_caps[0].first <= 14 && check_caps[1].first <= 14);
}

std::size_t first_tab_column_x(const std::string& line) {
    const auto pos = line.find('\t');
    CHECK(pos != std::string::npos);
    return pos;
}

void test_quota_layout() {
    std::vector<TrayGroup> groups{
        TrayGroup{
            "Codex",
            {},
            {
                {"", "5h", "75%", "12:00"},
                {"", "week", "50%", "11/25 12:00"},
            },
            "更新于 12:00",
        },
        TrayGroup{
            "Antigravity",
            {},
            {
                {"M0", "5h", "100%", "12:00"},
                {"M1", "5h", "99.9%", "12:00"},
                {"M2", "5h", "50%", "12:00"},
                {"M3", "5h", "20%", "12:00"},
                {"M4", "week", "0%", "11/25 12:00"},
            },
            "更新于 12:00",
        },
        TrayGroup{
            "系统",
            {"内存　4.0 / 16.0 GiB", "CPU　5%"},
            {},
            {},
        },
    };

    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    GroupPages pages;
    bool measured_ready = false;
    QuotaLayout layout([&] {
        pages = layout.format(groups, false);
        if (!pages.empty() && !pages[0].empty() && pages[0][0].find(" \t") != std::string::npos) {
            measured_ready = true;
            g_main_loop_quit(loop);
        }
    });

    pages = layout.format(groups, false);
    const guint timeout_id = g_timeout_add_seconds(10, [](gpointer data) -> gboolean {
        g_main_loop_quit(static_cast<GMainLoop*>(data));
        return G_SOURCE_REMOVE;
    }, loop);

    g_main_loop_run(loop);
    if (!measured_ready) g_source_remove(timeout_id);
    else {
        // The loop ended through measured_ready, so remove the timeout explicitly.
        if (auto* source = g_main_context_find_source_by_id(nullptr, timeout_id)) g_source_destroy(source);
    }
    g_main_loop_unref(loop);

    CHECK(measured_ready);
    CHECK(pages.size() == 3);
    CHECK(pages[0].size() == 1);
    // Five rows split into two pages of at most four lines.
    CHECK(pages[1].size() == 2);
    CHECK(pages[1][0].find("M0") != std::string::npos);
    CHECK(pages[1][0].find("M3") != std::string::npos);
    CHECK(pages[1][0].find("M4") == std::string::npos);
    CHECK(pages[1][1].find("M4") != std::string::npos);

    // Every provider has its heading column and alignment markers.
    const auto header0_end = pages[0][0].find('\n');
    const auto header1_end = pages[1][0].find('\n');
    CHECK(header0_end != std::string::npos && header1_end != std::string::npos);
    const auto h0 = pages[0][0].substr(0, header0_end);
    const auto h1 = pages[1][0].substr(0, header1_end);
    CHECK(first_tab_column_x(h0) > 0 && first_tab_column_x(h1) > 0);
    CHECK(h0.find(" \t") != std::string::npos && h1.find(" \t") != std::string::npos);
    CHECK(pages[2].size() == 1 && pages[2][0].find("内存") != std::string::npos);
}

} // namespace

int main() {
    setenv("GSETTINGS_BACKEND", "memory", 1);
    const auto helper = helper_path();
    test_font_helper(helper);
    test_quota_layout();
    std::cout << "layout tests passed\n";
    return 0;
}
