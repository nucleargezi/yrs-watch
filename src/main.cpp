#include "core.hpp"
#include "desktop.hpp"
#include "providers.hpp"
#include "schedule.hpp"
#include "tray.hpp"

#include <gio/gio.h>
#include <glib-unix.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

namespace yrswatch {
namespace {
constexpr char app_id[] = "io.github.yorisou.YrsWatch";
std::int64_t monotonic_seconds() { return g_get_monotonic_time() / G_USEC_PER_SEC; }
std::string title(size_t source) { return std::string(provider_table[source].title); }
bool is_cli(size_t source) { return provider_table[source].kind == SourceKind::Cli; }

void clear_timer(guint& id) {
    if (id) g_source_remove(std::exchange(id, 0));
}

std::string decimal(double value, int places) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(places) << value;
    return out.str();
}

std::string local_time(Epoch at, Epoch now, bool include_date = false) {
    auto* t = g_date_time_new_from_unix_local(at);
    auto* today = g_date_time_new_from_unix_local(now);
    if (!t || !today) {
        if (t) g_date_time_unref(t);
        if (today) g_date_time_unref(today);
        return "时间未知";
    }
    const bool same_day = g_date_time_get_year(t) == g_date_time_get_year(today) &&
                          g_date_time_get_day_of_year(t) == g_date_time_get_day_of_year(today);
    const bool same_year = g_date_time_get_year(t) == g_date_time_get_year(today);
    const char* format = same_year ? "%m/%d %H:%M" : "%Y/%m/%d %H:%M";
    gchar* printed = g_date_time_format(t, same_day && !include_date ? "%H:%M" : format);
    std::string result = printed ? std::string(printed) : "时间未知";
    g_free(printed);
    g_date_time_unref(t);
    g_date_time_unref(today);
    return result;
}

std::string compact_window(const std::string& window) {
    // Older cached samples use localized labels, newer clients may supply identifiers.
    if (window == "每周" || window == "weekly" || window == "week") return "week";
    if (window == "daily") return "1d";
    for (const auto& [suffix, unit] : {std::pair<std::string_view, std::string_view>{" 小时", "h"},
                                       {" 天", "d"}, {" 分钟", "m"}}) {
        if (window.ends_with(suffix)) return window.substr(0, window.size() - suffix.size()) + std::string(unit);
    }
    return window;
}

std::string quota_amount(const QuotaRow& row) {
    if (row.remaining) {
        const double percent = *row.remaining * 100;
        // Reserve 0% and 100% for confirmed empty and full windows.
        const auto tenths = std::lround(percent * 10);
        if (percent > 0 && tenths == 0) return "<0.1%";
        if (percent < 100 && tenths == 1000) return ">99.9%";
        return decimal(static_cast<double>(tenths) / 10.0, tenths % 10 == 0 ? 0 : 1) + "%";
    }
    if (row.quantity) {
        auto amount = decimal(*row.quantity, std::trunc(*row.quantity) == *row.quantity ? 0 : 2);
        return row.unit.empty() ? amount : amount + " " + row.unit;
    }
    return "—";
}

TrayQuotaRow quota_cells(const QuotaRow& row, Epoch now, const std::string& section) {
    auto group = row.group;
    if (group == section) group.clear();
    else if (g_ascii_strcasecmp(group.c_str(), "Gemini Models") == 0) group = "Gemini";
    else if (g_ascii_strcasecmp(group.c_str(), "Claude and GPT models") == 0) group = "Claude / GPT";
    if (group.empty() && row.window.empty()) group = row.label;
    auto amount = row.disabled ? "未开放" : quota_amount(row);
    std::string reset = "—";
    if (row.reset_at) reset = expired(row, now) ? "待确认" : local_time(*row.reset_at, now, true);
    return {std::move(group), compact_window(row.window), std::move(amount), std::move(reset)};
}

struct Source {
    ProviderState state;
    Schedule schedule{monotonic_seconds() + 30};
    std::jthread worker;
    bool attempted = false, busy = false, queued = false;
    Epoch started_at = 0;
};

class Watch : public std::enable_shared_from_this<Watch> {
public:
    explicit Watch(GApplication* app) : app_(app) {
        for (auto& sample : load_cache(cache_file_))
            sources_[index(sample.provider)].state.last = std::move(sample);
        autostart_ = autostart_enabled();
    }

    void start() {
        tray_ = std::make_unique<Tray>(TrayActions{
            [this]{ opened(); }, [this]{ closed(); }, [this]{ refresh(); },
            [this]{ toggle_startup(); }, [this]{ g_application_quit(app_); },
            [](const std::string& message){ g_warning("yrs-watch: %s", message.c_str()); },
            [this](size_t group){
                // Menu blocks follow rendered_sources_, then the system block.
                if (group < rendered_sources_.size()) launch(rendered_sources_[group]);
                else if (visible_) sample_system();
                render();
                arm();
            }
        });
        render();
        arm();
        watch_sleep();
    }

    ~Watch() {
        clear_timer(schedule_timer_);
        clear_timer(system_timer_);
        if (sleep_signal_) g_dbus_connection_signal_unsubscribe(system_bus_, sleep_signal_);
        g_clear_object(&system_bus_);
        for (auto& source : sources_) source.worker.request_stop();
        for (auto& source : sources_) if (source.worker.joinable()) source.worker.join();
    }

private:
    GApplication* app_;
    std::array<Source, provider_count> sources_;
    std::vector<size_t> rendered_sources_; // Source index of each quota block in the menu.
    std::filesystem::path cache_file_ = std::filesystem::path(g_get_user_cache_dir()) / "yrs-watch" / "quota.cache";
    std::unique_ptr<Tray> tray_;
    guint schedule_timer_ = 0, system_timer_ = 0, sleep_signal_ = 0;
    GDBusConnection* system_bus_ = nullptr;
    bool visible_ = false, autostart_ = false;
    std::optional<Memory> memory_;
    std::optional<CpuTicks> previous_cpu_;
    std::optional<double> cpu_;

    std::int64_t deadline(const Source& source) const {
        auto due = source.schedule.due;
        if (source.attempted) {
            if (auto reset = source.schedule.next_reset(source.state))
                due = std::min(due, monotonic_seconds() + std::max<Epoch>(0, *reset - now_epoch()));
        }
        return due;
    }

    void arm() {
        clear_timer(schedule_timer_);
        std::optional<std::int64_t> earliest;
        for (const auto& source : sources_)
            if (!source.busy && !source.queued && (!earliest || deadline(source) < *earliest)) earliest = deadline(source);
        if (!earliest) return;
        const auto seconds = static_cast<guint>(std::clamp<std::int64_t>(*earliest - monotonic_seconds(), 1, 900));
        schedule_timer_ = g_timeout_add_seconds(seconds, [](gpointer data) -> gboolean {
            auto& watch = *static_cast<Watch*>(data);
            watch.schedule_timer_ = 0;
            for (size_t i = 0; i < watch.sources_.size(); ++i) {
                auto& source = watch.sources_[i];
                if (!source.busy && !source.queued && watch.deadline(source) <= monotonic_seconds()) watch.launch(i);
            }
            watch.render();
            watch.arm();
            return G_SOURCE_REMOVE;
        }, this);
    }

    bool cli_busy() const {
        for (size_t i = 0; i < sources_.size(); ++i)
            if (is_cli(i) && sources_[i].busy) return true;
        return false;
    }

    struct Delivery {
        std::weak_ptr<Watch> target;
        size_t index;
        QuotaResult result;
    };

    void launch(size_t i) {
        auto& source = sources_[i];
        if (source.busy) return;
        // Each official client briefly loads a large runtime. Keep only one alive at a time.
        if (is_cli(i) && cli_busy()) {
            source.queued = true;
            return;
        }
        source.queued = false;
        source.attempted = source.busy = true;
        source.started_at = now_epoch();
        source.worker = std::jthread([target = weak_from_this(), i](std::stop_token stop) {
            auto result = fetch_quota(provider_table[i].id, stop);
            if (stop.stop_requested()) return;
            // Always dispatch to the GUI context, including during application shutdown.
            g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
                auto& delivery = *static_cast<Delivery*>(data);
                if (auto watch = delivery.target.lock())
                    watch->received(delivery.index, std::move(delivery.result));
                return G_SOURCE_REMOVE;
            }, new Delivery{target, i, std::move(result)}, [](gpointer data){ delete static_cast<Delivery*>(data); });
        });
    }

    void received(size_t i, QuotaResult result) {
        auto& source = sources_[i];
        source.busy = false;
        source.schedule.completed(result.sample.has_value(), monotonic_seconds());
        std::vector<Alert> alerts;
        if (result.sample) {
            alerts = apply_sample(source.state, std::move(*result.sample));
            for (const auto& row : source.state.last->rows) {
                if (row.reset_at && *row.reset_at <= source.started_at)
                    source.schedule.checked_reset = std::max(source.schedule.checked_reset, *row.reset_at);
            }
            std::vector<QuotaSample> snapshots;
            for (const auto& item : sources_) if (item.state.last) snapshots.push_back(*item.state.last);
            if (!save_cache(cache_file_, snapshots)) g_warning("yrs-watch: could not save quota cache");
        } else {
            switch (result.error) {
                case QuotaError::NeedsLogin: source.state.error = "需要登录"; break;
                case QuotaError::Missing: source.state.error = "未找到客户端"; break;
                case QuotaError::Timeout: source.state.error = "等待连接 · 查询超时"; break;
                case QuotaError::Offline: source.state.error = "等待连接"; break;
                default: source.state.error = result.message.empty() ? "等待连接" : result.message; break;
            }
        }
        for (size_t pending = 0; pending < sources_.size(); ++pending)
            if (sources_[pending].queued) launch(pending);
        render();
        arm();
        for (const auto& alert : alerts) {
            std::string body = alert.label;
            switch (alert.kind) {
                case Alert::Kind::Low:
                    body += " · 剩余额度已降至 " + decimal(low_quota_fraction * 100, 0) + "% 或以下";
                    break;
                case Alert::Kind::Exhausted: body += " · 额度已用尽"; break;
                case Alert::Kind::Recovered: body += " · 额度已恢复"; break;
            }
            tray_->notify(title(i), body, alert.kind != Alert::Kind::Recovered,
                          std::string(provider_table[i].key) + "/" + alert.row_id);
        }
    }

    void refresh() {
        for (size_t i = 0; i < sources_.size(); ++i) launch(i);
        render();
        arm();
    }

    using WeakWatch = std::weak_ptr<Watch>;

    // logind announces suspend and resume on the system bus. Without it, refreshes
    // simply follow the regular schedule.
    void watch_sleep() {
        g_bus_get(G_BUS_TYPE_SYSTEM, nullptr, [](GObject*, GAsyncResult* res, gpointer data) {
            std::unique_ptr<WeakWatch> weak(static_cast<WeakWatch*>(data));
            g_autoptr(GError) error = nullptr;
            g_autoptr(GDBusConnection) bus = g_bus_get_finish(res, &error);
            auto self = weak->lock();
            if (!self) return;
            if (!bus) {
                g_message("yrs-watch: no system bus, refresh after resume is disabled: %s", error->message);
                return;
            }
            self->system_bus_ = G_DBUS_CONNECTION(g_object_ref(bus));
            self->sleep_signal_ = g_dbus_connection_signal_subscribe(bus, "org.freedesktop.login1",
                "org.freedesktop.login1.Manager", "PrepareForSleep", "/org/freedesktop/login1", nullptr,
                G_DBUS_SIGNAL_FLAGS_NONE,
                [](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* args, gpointer user) {
                    gboolean sleeping = true;
                    if (g_variant_is_of_type(args, G_VARIANT_TYPE("(b)"))) g_variant_get(args, "(b)", &sleeping);
                    if (sleeping) return;
                    if (auto watch = static_cast<WeakWatch*>(user)->lock()) watch->resumed();
                }, new WeakWatch(self), [](gpointer user) { delete static_cast<WeakWatch*>(user); });
        }, new WeakWatch(weak_from_this()));
    }

    void resumed() {
        const auto now = monotonic_seconds();
        for (auto& source : sources_) source.schedule.resumed(now);
        arm();
    }

    void sample_system() {
        memory_ = read_memory();
        auto ticks = read_cpu();
        cpu_.reset();
        if (ticks && previous_cpu_) cpu_ = cpu_fraction(*previous_cpu_, *ticks);
        previous_cpu_ = ticks;
    }

    void opened() {
        if (visible_) return;
        visible_ = true;
        previous_cpu_.reset();
        sample_system();
        system_timer_ = g_timeout_add(1000, [](gpointer data) -> gboolean {
            auto& watch = *static_cast<Watch*>(data);
            watch.sample_system();
            watch.render();
            watch.system_timer_ = g_timeout_add_seconds(3, [](gpointer ptr) -> gboolean {
                auto& self = *static_cast<Watch*>(ptr);
                self.sample_system();
                self.render();
                return G_SOURCE_CONTINUE;
            }, data);
            return G_SOURCE_REMOVE;
        }, this);
        for (size_t i = 0; i < sources_.size(); ++i) {
            const auto& source = sources_[i];
            if ((!source.state.last || stale(*source.state.last, now_epoch(), 300)) &&
                (!source.schedule.failures || monotonic_seconds() >= source.schedule.due)) launch(i);
        }
        render();
        arm();
    }

    void closed() {
        visible_ = false;
        clear_timer(system_timer_);
        previous_cpu_.reset();
    }

    void toggle_startup() {
        if (set_autostart(!autostart_)) autostart_ = !autostart_;
        else tray_->notify("yrs-watch", "无法保存登录启动设置", true);
        render();
    }

    void render() {
        if (!tray_) return;
        TrayView view;
        view.tooltip = "yrs-watch";
        view.autostart = autostart_;
        // Measuring fonts spawns a helper; wait until no heavy client is running.
        view.defer_font_query = false;
        for (size_t i = 0; i < sources_.size(); ++i)
            if (is_cli(i) && (sources_[i].busy || sources_[i].queued)) view.defer_font_query = true;
        const Epoch now = now_epoch();
        rendered_sources_.clear();
        for (size_t i = 0; i < sources_.size(); ++i) {
            const auto& source = sources_[i];
            const auto& last = source.state.last;
            if (provider_table[i].hide_without_data && !last) continue;
            rendered_sources_.push_back(i);
            TrayGroup group;
            group.title = title(i);
            std::string status = source.state.error;
            if (source.busy) status = "刷新中…";
            else if (source.queued) status = "等待刷新";
            if (last) {
                for (const auto& row : last->rows) {
                    group.quota.push_back(quota_cells(row, now, group.title));
                    if (source.state.error.empty() && !stale(*last, now, 960) && !expired(row, now) &&
                        ((row.remaining && *row.remaining <= low_quota_fraction) ||
                        (!row.remaining && row.quantity && *row.quantity == 0))) view.attention = true;
                }
                if (status.empty() && stale(*last, now, 960)) status = "缓存";
                status += status.empty() ? "更新于 " : " · 上次更新 ";
                status += local_time(last->observed_at, now);
            } else if (status.empty()) status = "等待连接";
            group.status = std::move(status);
            view.groups.push_back(std::move(group));
        }
        constexpr double gib = 1024.0 * 1024 * 1024;
        const std::string ram = memory_ ? decimal(static_cast<double>(memory_->used) / gib, 1) + " / " +
            decimal(static_cast<double>(memory_->total) / gib, 1) + " GiB" : "—";
        TrayGroup system;
        system.title = "系统";
        system.lines = {"内存　" + ram, "CPU　" + (cpu_ ? decimal(*cpu_ * 100, 0) + "%" : "—")};
        view.groups.push_back(std::move(system));
        tray_->update(std::move(view));
    }
};

struct Session { std::shared_ptr<Watch> watch; };
} // namespace
} // namespace yrswatch

int main(int argc, char** argv) {
    using namespace yrswatch;
    g_set_application_name("yrs-watch");
    auto* app = g_application_new(app_id, G_APPLICATION_DEFAULT_FLAGS);
    Session session;
    g_signal_connect(app, "activate", G_CALLBACK(+[](GApplication* application, gpointer data) {
        auto& state = *static_cast<Session*>(data);
        if (state.watch) return;
        g_application_hold(application);
        state.watch = std::make_shared<Watch>(application);
        state.watch->start();
    }), &session);
    auto quit = +[](gpointer data) -> gboolean {
        g_application_quit(G_APPLICATION(data));
        return G_SOURCE_CONTINUE;
    };
    const guint term = g_unix_signal_add(SIGTERM, quit, app);
    const guint intr = g_unix_signal_add(SIGINT, quit, app);
    const int result = g_application_run(app, argc, argv);
    g_source_remove(term);
    g_source_remove(intr);
    session.watch.reset();
    g_object_unref(app);
    return result;
}
