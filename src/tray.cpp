#include "tray.hpp"
#include "quota_layout.hpp"

#include <gio/gio.h>

#include <algorithm>
#include <map>
#include <string_view>
#include <utility>

namespace yrswatch {
namespace {

constexpr char item_iface[] = "org.kde.StatusNotifierItem";
constexpr char watcher[] = "org.kde.StatusNotifierWatcher";
constexpr char menu_iface[] = "com.canonical.dbusmenu";
constexpr char item_path[] = "/StatusNotifierItem";
constexpr char menu_path[] = "/MenuBar";
constexpr char normal_icon[] = "power-profile-performance-symbolic";
constexpr char warning_icon[] = "dialog-warning-symbolic";
constexpr int refresh_id = 1, autostart_id = 2, quit_id = 3;
constexpr int page_id = 4;
constexpr int first_group_id = 100;

constexpr char interfaces[] = R"xml(<node>
<interface name="org.kde.StatusNotifierItem">
  <property name="Category" type="s" access="read"/>
  <property name="Id" type="s" access="read"/>
  <property name="Title" type="s" access="read"/>
  <property name="Status" type="s" access="read"/>
  <property name="WindowId" type="i" access="read"/>
  <property name="IconThemePath" type="s" access="read"/>
  <property name="IconName" type="s" access="read"/>
  <property name="IconPixmap" type="a(iiay)" access="read"/>
  <property name="OverlayIconName" type="s" access="read"/>
  <property name="OverlayIconPixmap" type="a(iiay)" access="read"/>
  <property name="AttentionIconName" type="s" access="read"/>
  <property name="AttentionIconPixmap" type="a(iiay)" access="read"/>
  <property name="AttentionMovieName" type="s" access="read"/>
  <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
  <property name="Menu" type="o" access="read"/>
  <property name="ItemIsMenu" type="b" access="read"/>
  <property name="XAyatanaLabel" type="s" access="read"/>
  <property name="XAyatanaLabelGuide" type="s" access="read"/>
  <property name="IconAccessibleDesc" type="s" access="read"/>
  <property name="AttentionAccessibleDesc" type="s" access="read"/>
  <method name="Activate"><arg type="i"/><arg type="i"/></method>
  <method name="SecondaryActivate"><arg type="i"/><arg type="i"/></method>
  <method name="XAyatanaSecondaryActivate"><arg type="u"/></method>
  <method name="ContextMenu"><arg type="i"/><arg type="i"/></method>
  <method name="Scroll"><arg type="i"/><arg type="s"/></method>
  <method name="ProvideXdgActivationToken"><arg type="s"/></method>
  <signal name="NewStatus"><arg type="s"/></signal>
  <signal name="NewToolTip"/>
  <signal name="XAyatanaNewLabel"><arg type="s"/><arg type="s"/></signal>
</interface>
<interface name="com.canonical.dbusmenu">
  <property name="Version" type="u" access="read"/>
  <property name="TextDirection" type="s" access="read"/>
  <property name="Status" type="s" access="read"/>
  <property name="IconThemePath" type="as" access="read"/>
  <method name="GetLayout">
    <arg type="i"/><arg type="i"/><arg type="as"/>
    <arg direction="out" type="u"/><arg direction="out" type="(ia{sv}av)"/>
  </method>
  <method name="GetGroupProperties">
    <arg type="ai"/><arg type="as"/><arg direction="out" type="a(ia{sv})"/>
  </method>
  <method name="GetProperty">
    <arg type="i"/><arg type="s"/><arg direction="out" type="v"/>
  </method>
  <method name="Event"><arg type="i"/><arg type="s"/><arg type="v"/><arg type="u"/></method>
  <method name="EventGroup"><arg type="a(isvu)"/><arg direction="out" type="ai"/></method>
  <method name="AboutToShow"><arg type="i"/><arg direction="out" type="b"/></method>
  <method name="AboutToShowGroup"><arg type="ai"/><arg direction="out" type="ai"/><arg direction="out" type="ai"/></method>
  <signal name="ItemsPropertiesUpdated"><arg type="a(ia{sv})"/><arg type="a(ias)"/></signal>
  <signal name="LayoutUpdated"><arg type="u"/><arg type="i"/></signal>
</interface>
</node>)xml";

enum class Kind { root, info, separator, action, check };
struct Row { int id; Kind kind; std::string label; bool checked = false, visible = true; };
using Rows = std::vector<Row>;

size_t page_count(const GroupPages& sections) {
    size_t count = 1;
    for (const auto& pages : sections) count = std::max(count, pages.size());
    return count;
}

Rows make_rows(const TrayView& view, const GroupPages& sections, size_t page) {
    Rows rows{{0, Kind::root, {}}};
    for (size_t i = 0; i < sections.size(); ++i) {
        const auto& pages = sections[i];
        const auto& summary = pages[std::min(page, pages.size() - 1)];
        const auto id = first_group_id + 2 * static_cast<int>(i);
        rows.push_back({id, Kind::info, summary});
        rows.push_back({id + 1, Kind::separator, {}});
    }
    const size_t pages = page_count(sections);
    const std::string direction = page + 1 < pages ? "下一页额度" : "返回首页额度";
    rows.push_back({page_id, Kind::action, direction + "　" + std::to_string(page + 1) +
        " / " + std::to_string(pages), false, pages > 1});
    rows.push_back({refresh_id, Kind::action, "立即刷新"});
    rows.push_back({autostart_id, Kind::check, "登录时自动启动", view.autostart});
    rows.push_back({quit_id, Kind::action, "退出"});
    return rows;
}

GVariant* empty_array(const char* element_type) {
    return g_variant_new_array(G_VARIANT_TYPE(element_type), nullptr, 0);
}

GVariant* row_property(const Row& row, std::string_view name) {
    if (name == "label") return g_variant_new_string(row.label.c_str());
    if (name == "visible") return g_variant_new_boolean(row.visible);
    if (name == "enabled") return g_variant_new_boolean(row.kind != Kind::separator);
    if (name == "type") return g_variant_new_string(row.kind == Kind::separator ? "separator" : "standard");
    if (name == "children-display" && row.kind == Kind::root) return g_variant_new_string("submenu");
    if (name == "toggle-type" && row.kind == Kind::check) return g_variant_new_string("checkmark");
    if (name == "toggle-state" && row.kind == Kind::check) return g_variant_new_int32(row.checked);
    return nullptr;
}

GVariant* properties(const Row& row, GVariant* names) {
    GVariantBuilder dict;
    g_variant_builder_init(&dict, G_VARIANT_TYPE_VARDICT);
    auto add = [&](const char* name) {
        if (auto value = row_property(row, name)) g_variant_builder_add(&dict, "{sv}", name, value);
    };
    if (g_variant_n_children(names)) {
        GVariantIter iter;
        g_variant_iter_init(&iter, names);
        const char* name;
        while (g_variant_iter_next(&iter, "&s", &name)) add(name);
    } else {
        for (auto name : {"type", "label", "visible", "enabled", "children-display", "toggle-type", "toggle-state"}) add(name);
    }
    return g_variant_builder_end(&dict);
}

const Row* find_row(const Rows& rows, int id) {
    auto it = std::ranges::find(rows, id, &Row::id);
    return it == rows.end() ? nullptr : &*it;
}

GVariant* layout(const Rows& rows, const Row& row, int depth, GVariant* names) {
    GVariantBuilder children;
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
    if (row.kind == Kind::root && depth != 0) {
        for (const auto& child : rows) {
            if (child.id) g_variant_builder_add(&children, "v", layout(rows, child, 0, names));
        }
    }
    return g_variant_new("(i@a{sv}@av)", row.id, properties(row, names), g_variant_builder_end(&children));
}

void invalid_item(GDBusMethodInvocation* invocation) {
    g_dbus_method_invocation_return_dbus_error(invocation, "com.canonical.dbusmenu.Error.InvalidItem", "Unknown menu item or property");
}

void run_action(const std::function<void()>& action) {
    if (action) action();
}

} // namespace

struct Tray::Impl : std::enable_shared_from_this<Impl> {
    explicit Impl(TrayActions callbacks) : actions(std::move(callbacks)), formatter([this]{ update(view); }) {}
    TrayActions actions;
    TrayView view;
    size_t page = 0;
    QuotaLayout formatter;
    GroupPages sections;
    Rows rows = make_rows(view, sections, page);
    guint revision = 1, item_id = 0, menu_id = 0, watch_id = 0, retry_id = 0;
    std::string watcher_owner;
    std::map<std::string, guint32> notification_ids;
    bool registered = false, opened = false;
    GDBusConnection* bus = nullptr;
    GCancellable* cancellable = g_cancellable_new();

    ~Impl() {
        g_cancellable_cancel(cancellable);
        if (retry_id) g_source_remove(retry_id);
        if (watch_id) g_bus_unwatch_name(watch_id);
        if (item_id) g_dbus_connection_unregister_object(bus, item_id);
        if (menu_id) g_dbus_connection_unregister_object(bus, menu_id);
        g_clear_object(&bus);
        g_object_unref(cancellable);
    }

    void error(const char* message) {
        if (actions.error) actions.error(message);
        else g_warning("yrs-watch: %s", message);
    }

    void start() {
        g_bus_get(G_BUS_TYPE_SESSION, cancellable, [](GObject*, GAsyncResult* res, gpointer data) {
            std::unique_ptr<std::weak_ptr<Impl>> weak(static_cast<std::weak_ptr<Impl>*>(data));
            g_autoptr(GError) err = nullptr;
            g_autoptr(GDBusConnection) connection = g_bus_get_finish(res, &err);
            if (auto self = weak->lock()) {
                if (!connection) self->error(err->message);
                else self->export_objects(connection);
            }
        }, new std::weak_ptr<Impl>(weak_from_this()));
    }

    void export_objects(GDBusConnection* connection) {
        bus = G_DBUS_CONNECTION(g_object_ref(connection));
        static const GDBusInterfaceVTable vtable{
            .method_call = [](GDBusConnection*, const char*, const char*, const char* iface,
                              const char* method, GVariant* args, GDBusMethodInvocation* call, gpointer data) {
                auto& self = *static_cast<Impl*>(data);
                if (std::string_view(iface) == menu_iface) self.menu_method(method, args, call);
                else g_dbus_method_invocation_return_value(call, nullptr);
            },
            .get_property = [](GDBusConnection*, const char*, const char*, const char* iface,
                               const char* name, GError**, gpointer data) {
                return static_cast<Impl*>(data)->property(iface, name);
            }, .set_property = nullptr, .padding = {}
        };
        g_autoptr(GError) err = nullptr;
        g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(interfaces, &err);
        if (!node) { error(err->message); return; }
        auto add = [&](const char* path, const char* iface) {
            return g_dbus_connection_register_object(bus, path, g_dbus_node_info_lookup_interface(node, iface),
                                                     &vtable, this, nullptr, &err);
        };
        item_id = add(item_path, item_iface);
        if (item_id) menu_id = add(menu_path, menu_iface);
        if (!menu_id) { error(err->message); return; }
        watch_id = g_bus_watch_name_on_connection(bus, watcher, G_BUS_NAME_WATCHER_FLAGS_NONE,
            [](GDBusConnection*, const char*, const char* owner, gpointer data) {
                auto& self = *static_cast<Impl*>(data);
                self.watcher_owner = owner;
                self.register_item();
            }, [](GDBusConnection*, const char*, gpointer data) {
                auto& self = *static_cast<Impl*>(data);
                self.watcher_owner.clear();
                self.registered = false;
                self.set_open(false);
                if (self.retry_id) { g_source_remove(self.retry_id); self.retry_id = 0; }
            }, this, nullptr);
    }

    void register_item() {
        if (retry_id) { g_source_remove(retry_id); retry_id = 0; }
        using Pending = std::pair<std::weak_ptr<Impl>, std::string>;
        g_dbus_connection_call(bus, watcher_owner.c_str(), "/StatusNotifierWatcher", watcher,
            "RegisterStatusNotifierItem", g_variant_new("(s)", item_path), G_VARIANT_TYPE_UNIT,
            G_DBUS_CALL_FLAGS_NONE, 5000, cancellable, [](GObject* source, GAsyncResult* res, gpointer data) {
                std::unique_ptr<Pending> pending(static_cast<Pending*>(data));
                g_autoptr(GError) err = nullptr;
                g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
                auto self = pending->first.lock();
                if (!self || self->watcher_owner != pending->second) return;
                self->registered = reply != nullptr;
                if (!reply) {
                    self->error(err->message);
                    self->retry_id = g_timeout_add_seconds(15, [](gpointer arg) -> gboolean {
                        auto& tray = *static_cast<Impl*>(arg);
                        tray.retry_id = 0;
                        tray.register_item();
                        return G_SOURCE_REMOVE;
                    }, self.get());
                }
            }, new Pending(weak_from_this(), watcher_owner));
    }

    const char* status() const { return view.attention ? "NeedsAttention" : "Active"; }
    GVariant* property(std::string_view iface, std::string_view name) const {
        if (iface == menu_iface) {
            if (name == "Version") return g_variant_new_uint32(3);
            if (name == "IconThemePath") return empty_array("s");
            return g_variant_new_string(name == "TextDirection" ? "ltr" : "normal");
        }
        if (name == "Status") return g_variant_new_string(status());
        if (name == "Category") return g_variant_new_string("SystemServices");
        if (name == "Id") return g_variant_new_string("yrs-watch");
        if (name == "Title") return g_variant_new_string("Yrs Watch");
        if (name == "Menu") return g_variant_new_object_path(menu_path);
        if (name == "ItemIsMenu") return g_variant_new_boolean(true);
        if (name == "WindowId") return g_variant_new_int32(0);
        if (name.ends_with("Pixmap")) return empty_array("(iiay)");
        if (name == "IconName") return g_variant_new_string(normal_icon);
        if (name == "AttentionIconName") return g_variant_new_string(warning_icon);
        if (name == "XAyatanaLabel") return g_variant_new_string(view.label.c_str());
        if (name.ends_with("AccessibleDesc")) return g_variant_new_string(view.tooltip.c_str());
        if (name == "ToolTip") return g_variant_new("(s@a(iiay)ss)", "", empty_array("(iiay)"), "Yrs Watch", view.tooltip.c_str());
        return g_variant_new_string("");
    }

    void set_open(bool value) {
        if (opened == value) return;
        opened = value;
        run_action(value ? actions.open : actions.close);
    }

    bool event(int id, std::string_view event_name) {
        const auto* row = find_row(rows, id);
        if (!row || !row->visible) return false;
        if (id == 0 && event_name == "opened") set_open(true);
        if (id == 0 && event_name == "closed") set_open(false);
        if (event_name == "clicked") {
            if (id == page_id) {
                page = (page + 1) % page_count(sections);
                update(view);
                return true;
            }
            if (row->kind == Kind::info && actions.refresh_group)
                actions.refresh_group(static_cast<std::size_t>((id - first_group_id) / 2));
            if (id == refresh_id) run_action(actions.refresh);
            if (id == autostart_id) run_action(actions.toggle_autostart);
            if (id == quit_id) run_action(actions.quit);
        }
        return true;
    }

    void menu_method(std::string_view method, GVariant* args, GDBusMethodInvocation* call) {
        auto reply = [&](GVariant* value) { g_dbus_method_invocation_return_value(call, value); };
        g_autoptr(GVariant) first = g_variant_get_child_value(args, 0);
        if (method == "GetLayout") {
            const Row* row = find_row(rows, g_variant_get_int32(first));
            if (!row) { invalid_item(call); return; }
            int depth;
            g_variant_get_child(args, 1, "i", &depth);
            g_autoptr(GVariant) names = g_variant_get_child_value(args, 2);
            reply(g_variant_new("(u@(ia{sv}av))", revision, layout(rows, *row, depth, names)));
        } else if (method == "GetGroupProperties") {
            g_autoptr(GVariant) names = g_variant_get_child_value(args, 1);
            GVariantBuilder items;
            g_variant_builder_init(&items, G_VARIANT_TYPE("a(ia{sv})"));
            gsize count;
            const auto ids = static_cast<const gint32*>(g_variant_get_fixed_array(first, &count, sizeof(gint32)));
            for (const auto& row : rows) {
                if (!count || std::find(ids, ids + count, row.id) != ids + count)
                    g_variant_builder_add(&items, "(i@a{sv})", row.id, properties(row, names));
            }
            reply(g_variant_new("(@a(ia{sv}))", g_variant_builder_end(&items)));
        } else if (method == "GetProperty") {
            const char* name;
            g_variant_get_child(args, 1, "&s", &name);
            const auto* row = find_row(rows, g_variant_get_int32(first));
            auto* value = row ? row_property(*row, name) : nullptr;
            if (value) reply(g_variant_new("(v)", value));
            else invalid_item(call);
        } else if (method == "Event") {
            const char* event_name;
            g_variant_get_child(args, 1, "&s", &event_name);
            if (event(g_variant_get_int32(first), event_name)) reply(nullptr);
            else invalid_item(call);
        } else if (method == "AboutToShow") {
            // GNOME also requests this while constructing the tray at login.
            // Only an actual 'opened' event indicates a visible menu.
            reply(g_variant_new("(b)", false));
        } else {
            GVariantBuilder errors;
            g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
            for (gsize i = 0; i < g_variant_n_children(first); ++i) {
                g_autoptr(GVariant) entry = g_variant_get_child_value(first, i);
                if (method == "EventGroup") {
                    int id;
                    const char* event_name;
                    g_variant_get_child(entry, 0, "i", &id);
                    g_variant_get_child(entry, 1, "&s", &event_name);
                    if (!event(id, event_name)) g_variant_builder_add(&errors, "i", id);
                } else {
                    int id = g_variant_get_int32(entry);
                    if (!find_row(rows, id)) g_variant_builder_add(&errors, "i", id);
                }
            }
            if (method == "EventGroup") reply(g_variant_new("(@ai)", g_variant_builder_end(&errors)));
            else reply(g_variant_new("(@ai@ai)", empty_array("i"), g_variant_builder_end(&errors)));
        }
    }

    void signal(const char* path, const char* iface, const char* name, GVariant* args) {
        g_dbus_connection_emit_signal(bus, nullptr, path, iface, name, args, nullptr);
    }

    void update(TrayView next) {
        auto before = std::exchange(view, std::move(next));
        sections = formatter.format(view.groups, view.defer_font_query);
        page = std::min(page, page_count(sections) - 1);
        auto old_rows = std::exchange(rows, make_rows(view, sections, page));
        if (!menu_id) return;

        GVariantDict changed;
        g_variant_dict_init(&changed, nullptr);
        if (view.label != before.label) {
            g_variant_dict_insert(&changed, "XAyatanaLabel", "s", view.label.c_str());
            signal(item_path, item_iface, "XAyatanaNewLabel", g_variant_new("(ss)", view.label.c_str(), ""));
        }
        if (view.attention != before.attention) {
            g_variant_dict_insert(&changed, "Status", "s", status());
            signal(item_path, item_iface, "NewStatus", g_variant_new("(s)", status()));
        }
        if (view.tooltip != before.tooltip) {
            g_variant_dict_insert_value(&changed, "ToolTip", property(item_iface, "ToolTip"));
            for (const char* key : {"IconAccessibleDesc", "AttentionAccessibleDesc"})
                g_variant_dict_insert(&changed, key, "s", view.tooltip.c_str());
            signal(item_path, item_iface, "NewToolTip", nullptr);
        }
        g_autoptr(GVariant) props = g_variant_ref_sink(g_variant_dict_end(&changed));
        if (g_variant_n_children(props)) {
            signal(item_path, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                   g_variant_new("(s@a{sv}@as)", item_iface, props, empty_array("s")));
        }

        if (!std::ranges::equal(old_rows, rows, {}, &Row::id, &Row::id))
            signal(menu_path, menu_iface, "LayoutUpdated", g_variant_new("(ui)", ++revision, 0));
        GVariantBuilder updates;
        g_variant_builder_init(&updates, G_VARIANT_TYPE("a(ia{sv})"));
        for (const auto& row : rows) {
            auto old = find_row(old_rows, row.id);
            if (!old || (old->label == row.label && old->checked == row.checked && old->visible == row.visible)) continue;
            GVariantDict changes;
            g_variant_dict_init(&changes, nullptr);
            if (old->label != row.label) g_variant_dict_insert(&changes, "label", "s", row.label.c_str());
            if (old->checked != row.checked) g_variant_dict_insert(&changes, "toggle-state", "i", int(row.checked));
            if (old->visible != row.visible) g_variant_dict_insert(&changes, "visible", "b", row.visible);
            g_variant_builder_add(&updates, "(i@a{sv})", row.id, g_variant_dict_end(&changes));
        }
        g_autoptr(GVariant) updated = g_variant_ref_sink(g_variant_builder_end(&updates));
        if (g_variant_n_children(updated)) {
            signal(menu_path, menu_iface, "ItemsPropertiesUpdated",
                   g_variant_new("(@a(ia{sv})@a(ias))", updated, empty_array("(ias)")));
        }
    }
};

Tray::Tray(TrayActions actions) : impl_(std::make_shared<Impl>(std::move(actions))) { impl_->start(); }
Tray::~Tray() = default;
bool Tray::menu_open() const { return impl_->opened; }
bool Tray::ready() const { return impl_->registered; }
void Tray::update(TrayView view) { impl_->update(std::move(view)); }

void Tray::notify(const std::string& title, const std::string& body, bool warning, const std::string& key) {
    if (!impl_->bus) return;
    guint32 replaces = 0;
    if (auto it = impl_->notification_ids.find(key); !key.empty() && it != impl_->notification_ids.end())
        replaces = it->second;
    g_autofree gchar* escaped = g_markup_escape_text(body.c_str(), -1);
    using Pending = std::pair<std::weak_ptr<Impl>, std::string>;
    g_dbus_connection_call(impl_->bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "Notify",
        g_variant_new("(susss@as@a{sv}i)", "Yrs Watch", replaces, warning ? warning_icon : normal_icon,
                      title.c_str(), escaped, empty_array("s"), empty_array("{sv}"), -1),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, impl_->cancellable,
        [](GObject* source, GAsyncResult* res, gpointer data) {
            std::unique_ptr<Pending> pending(static_cast<Pending*>(data));
            g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, nullptr);
            auto self = pending->first.lock();
            if (!self || !reply || pending->second.empty()) return;
            guint32 id = 0;
            g_variant_get(reply, "(u)", &id);
            if (id) self->notification_ids[pending->second] = id;
        }, new Pending(impl_, key));
}

} // namespace yrswatch
