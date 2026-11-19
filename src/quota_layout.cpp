#include "quota_layout.hpp"

#include <gio/gio.h>


#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <set>
#include <sstream>
#include <utility>
#include <map>

namespace yrswatch {
namespace {
constexpr unsigned lines_per_page = 4;
constexpr int max_model_stops = 4;
constexpr char gutter[] = " "; // an en space keeps adjacent columns visibly separate
constexpr char amount_heading[] = "余量";
constexpr char reset_heading[] = "重置";

std::string plain(std::string value) {
    for (char& c : value) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return value;
}

void append_line(std::string& label, const std::string& line) {
    if (!line.empty()) label += "\n" + line;
}
} // namespace

struct QuotaLayout::Font : std::enable_shared_from_this<Font> {
    struct Metric { int stops; std::string caption; };
    using Names = std::set<std::string>;
    std::map<std::string, Metric> cache;
    GSettings* settings = nullptr;
    GSubprocess* process = nullptr;
    GDBusProxy* display = nullptr;
    std::string name;
    std::uint64_t generation = 0;
    std::uint64_t display_request = 0;
    gint64 retry_after = 0;
    double scale = 1, render_scale = 1;
    bool checking_display = true;
    std::function<void()> changed;

    explicit Font(std::function<void()> callback) : changed(std::move(callback)) {
        if (auto* schemas = g_settings_schema_source_get_default()) {
            g_autoptr(GSettingsSchema) schema = g_settings_schema_source_lookup(schemas, "org.gnome.desktop.interface", true);
            if (schema) {
                settings = g_settings_new_full(schema, nullptr, nullptr);
                if (settings) {
                    auto on_changed = G_CALLBACK(+[](GSettings*, const char*, gpointer user) {
                        static_cast<Font*>(user)->changed();
                    });
                    g_signal_connect(settings, "changed::font-name", on_changed, this);
                    g_signal_connect(settings, "changed::text-scaling-factor", on_changed, this);
                }
            }
        }
    }

    ~Font() {
        if (process) g_subprocess_force_exit(process);
        g_clear_object(&process);
        if (display) g_signal_handlers_disconnect_by_data(display, this);
        g_clear_object(&display);
        if (settings) g_signal_handlers_disconnect_by_data(settings, this);
        g_clear_object(&settings);
    }

    void watch_display() {
        constexpr auto flags = static_cast<GDBusProxyFlags>(G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
            G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START);
        g_dbus_proxy_new_for_bus(G_BUS_TYPE_SESSION, flags, nullptr, "org.gnome.Mutter.DisplayConfig",
            "/org/gnome/Mutter/DisplayConfig", "org.gnome.Mutter.DisplayConfig", nullptr,
            [](GObject*, GAsyncResult* res, gpointer data) {
                std::unique_ptr<std::weak_ptr<Font>> weak(static_cast<std::weak_ptr<Font>*>(data));
                g_autoptr(GDBusProxy) proxy = g_dbus_proxy_new_for_bus_finish(res, nullptr);
                auto self = weak->lock();
                if (!self) return;
                if (proxy) {
                    self->display = G_DBUS_PROXY(g_object_ref(proxy));
                    g_signal_connect(proxy, "g-signal", G_CALLBACK(+[](GDBusProxy*, const char*, const char* signal, GVariant*, gpointer user) {
                        if (g_str_equal(signal, "MonitorsChanged")) static_cast<Font*>(user)->read_display();
                    }), self.get());
                    g_signal_connect(proxy, "notify::g-name-owner", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer user) {
                        static_cast<Font*>(user)->read_display();
                    }), self.get());
                }
                self->read_display();
            }, new std::weak_ptr<Font>(weak_from_this()));
    }

    void read_display() {
        using Pending = std::pair<std::weak_ptr<Font>, std::uint64_t>;
        ++display_request;
        g_autofree char* owner = display ? g_dbus_proxy_get_name_owner(display) : nullptr;
        if (!owner) {
            render_scale = 1;
            checking_display = false;
            changed();
            return;
        }
        checking_display = true;
        g_dbus_proxy_call(display, "GetCurrentState", nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START,
            1500, nullptr, [](GObject* source, GAsyncResult* res, gpointer data) {
                std::unique_ptr<Pending> pending(static_cast<Pending*>(data));
                g_autoptr(GVariant) reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, nullptr);
                auto self = pending->first.lock();
                if (!self || self->display_request != pending->second) return;
                double factor = 1;
                if (reply && g_variant_n_children(reply) >= 3) {
                    g_autoptr(GVariant) logical = g_variant_get_child_value(reply, 2);
                    if (g_variant_is_of_type(logical, G_VARIANT_TYPE("a(iiduba(ssss)a{sv})"))) {
                        for (gsize i = 0; i < g_variant_n_children(logical); ++i) {
                            g_autoptr(GVariant) monitor = g_variant_get_child_value(logical, i);
                            double monitor_scale = 1;
                            gboolean primary = false;
                            g_variant_get_child(monitor, 2, "d", &monitor_scale);
                            g_variant_get_child(monitor, 4, "b", &primary);
                            if (i == 0 || primary) factor = monitor_scale;
                            if (primary) break;
                        }
                    }
                }
                // Clutter shapes its text at the integer backing scale before compositing.
                self->render_scale = std::isfinite(factor) ? std::clamp(std::ceil(factor), 1.0, 8.0) : 1.0;
                self->checking_display = false;
                self->changed();
            }, new Pending(weak_from_this(), display_request));
    }

    void sync_settings() {
        g_autofree char* current = settings ? g_settings_get_string(settings, "font-name") : nullptr;
        std::string family = (current && *current) ? current : "Adwaita Sans 11";
        double text_scale = settings ? g_settings_get_double(settings, "text-scaling-factor") : 1.0;
        if (!std::isfinite(text_scale)) text_scale = 1.0;
        text_scale = std::clamp(text_scale, 0.5, 4.0);
        double factor = render_scale * text_scale;
        if (name != family || scale != factor) {
            name = std::move(family);
            scale = factor;
            ++generation;
            cache.clear();
            retry_after = 0;
        }
    }

    void read_result(const Names& names, const char* output) {
        std::istringstream stream(output);
        std::map<std::string, Metric> batch;
        for (const auto& text : names) {
            std::string line;
            if (!std::getline(stream, line)) return;
            const auto tab = line.find('\t');
            if (tab == std::string::npos) return;
            int count{};
            auto parsed = std::from_chars(line.data(), line.data() + tab, count);
            if (parsed.ec != std::errc{} || parsed.ptr != line.data() + tab || count <= 0) return;
            batch.emplace(text, Metric{count, line.substr(tab + 1)});
        }
        cache.merge(batch);
        retry_after = 0;
    }

    void query(Names missing) {
        struct Request { std::weak_ptr<Font> self; Names fields; std::uint64_t generation; };
        retry_after = g_get_monotonic_time() + 60 * G_USEC_PER_SEC;
        std::error_code error;
        auto exe = std::filesystem::read_symlink("/proc/self/exe", error);
        if (error) return;
        auto helper = exe.parent_path() / "yrs-watch-font-metrics";
        char factor[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_dtostr(factor, sizeof(factor), scale);
        const auto flags = static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDIN_PIPE |
            G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
        process = g_subprocess_new(flags, nullptr, helper.c_str(), name.c_str(), factor, nullptr);
        if (!process) return;
        std::string input;
        for (const auto& field : missing) input += field + "\n";
        g_subprocess_communicate_utf8_async(process, input.c_str(), nullptr, [](GObject* object, GAsyncResult* res, gpointer data) {
            std::unique_ptr<Request> req(static_cast<Request*>(data));
            auto* child = G_SUBPROCESS(object);
            g_autofree char* output = nullptr;
            const bool ok = g_subprocess_communicate_utf8_finish(child, res, &output, nullptr, nullptr);
            if (auto self = req->self.lock()) {
                g_clear_object(&self->process);
                if (ok && output && g_subprocess_get_successful(child) && req->generation == self->generation)
                    self->read_result(req->fields, output);
                self->changed();
            }
        }, new Request{weak_from_this(), std::move(missing), generation});
    }

    bool prepare(const Names& fields, bool defer) {
        sync_settings();
        if (cache.size() > 256 && cache.size() > fields.size())
            std::erase_if(cache, [&](const auto& it) { return !fields.contains(it.first); });
        Names missing;
        for (const auto& text : fields)
            if (!cache.contains(text)) missing.insert(text);
        if (missing.empty()) return true;
        if (!defer && !checking_display && !process && g_get_monotonic_time() >= retry_after) query(std::move(missing));
        return false;
    }

    int stops(const std::string& text) const { return cache.at(text).stops; }
    const std::string& caption(const std::string& text) const { return cache.at(text).caption; }
    std::string cell(const std::string& text, int width) const {
        return text + gutter + std::string(std::max(1, width - stops(text) + 1), '\t');
    }
};

QuotaLayout::QuotaLayout(std::function<void()> changed) : changed_(std::move(changed)) {}
QuotaLayout::~QuotaLayout() = default;

GroupPages QuotaLayout::format(const std::vector<TrayGroup>& groups, bool defer) {
    Font::Names fields;
    for (const auto& group : groups) {
        if (group.quota.empty()) continue;
        fields.insert(plain(group.title));
        for (const auto& row : group.quota)
            for (const auto* value : {&row.group, &row.window, &row.amount}) fields.insert(plain(*value));
    }
    int model_width = 0, window_width = 1, amount_start = 0, amount_width = 1;
    bool measured = false;
    if (!fields.empty()) {
        fields.insert("");
        fields.insert(amount_heading);
        if (!font_) {
            font_ = std::make_shared<Font>(changed_);
            font_->watch_display();
        }
        measured = font_->prepare(fields, defer);
    }
    if (measured) {
        amount_width = font_->stops(amount_heading);
        for (const auto& group : groups) {
            if (group.quota.empty()) continue;
            amount_start = std::max(amount_start, font_->stops(plain(group.title)));
            for (const auto& record : group.quota) {
                const auto model = plain(record.group);
                const int width = font_->stops(model);
                if (!model.empty() && width <= max_model_stops) model_width = std::max(model_width, width);
                window_width = std::max(window_width, font_->stops(plain(record.window)));
                amount_width = std::max(amount_width, font_->stops(plain(record.amount)));
            }
        }
        amount_start = std::max(amount_start, model_width + window_width);
    }

    GroupPages result;
    for (const auto& group : groups) {
        if (group.quota.empty()) {
            std::string label = group.title;
            for (const auto& line : group.lines) append_line(label, line);
            append_line(label, group.status);
            result.push_back({std::move(label)});
            continue;
        }
        const std::string title = plain(group.title);
        const std::string header = measured
            ? font_->cell(title, amount_start) + font_->cell(amount_heading, amount_width) + reset_heading
            : title + "\t" + amount_heading + "\t" + reset_heading;
        std::vector<std::string> pages{header};
        std::string previous;
        unsigned used = 0;
        for (const auto& record : group.quota) {
            const auto model = plain(record.group);
            const bool separate = measured && !model.empty() && font_->stops(model) > max_model_stops;
            if (used + 1 + unsigned(separate && model != previous) > lines_per_page) {
                append_line(pages.back(), group.status);
                pages.push_back(header);
                previous.clear();
                used = 0;
            }
            std::string row;
            if (separate && model != previous) {
                append_line(pages.back(), font_->caption(model));
                ++used;
            }
            const bool inline_model = !model.empty() && !separate;
            if (measured) {
                if (inline_model) row = font_->cell(model == previous ? "" : model, model_width);
                row += font_->cell(plain(record.window), amount_start - (inline_model ? model_width : 0));
                row += font_->cell(plain(record.amount), amount_width);
            } else {
                if (!model.empty() && model != previous) row += model + "  ";
                row += plain(record.window) + "\t" + plain(record.amount) + "\t";
            }
            row += plain(record.reset);
            append_line(pages.back(), row);
            ++used;
            previous = model;
        }
        append_line(pages.back(), group.status);
        result.push_back(std::move(pages));
    }
    return result;
}

} // namespace yrswatch
