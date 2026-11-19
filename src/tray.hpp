#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace yrswatch {

struct TrayActions {
    std::function<void()> open, close, refresh, toggle_autostart, quit;
    std::function<void(const std::string&)> error;
    std::function<void(std::size_t)> refresh_group = {};
};

struct TrayQuotaRow {
    std::string group, window, amount, reset;
};

struct TrayGroup {
    std::string title;
    std::vector<std::string> lines;
    std::vector<TrayQuotaRow> quota = {};
    std::string status = {};
};

struct TrayView {
    std::string label, tooltip;
    bool attention = false;
    bool autostart = false;
    std::vector<TrayGroup> groups;
    bool defer_font_query = false;
};

// Construct and update on the thread running the default GLib main context.
class Tray {
public:
    explicit Tray(TrayActions actions);
    ~Tray();
    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;
    void update(TrayView view);
    // Notifications sharing a non-empty key replace each other on screen.
    void notify(const std::string& title, const std::string& body, bool warning = false,
                const std::string& key = {});
    bool menu_open() const;
    bool ready() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace yrswatch
