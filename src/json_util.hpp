#pragma once

// Null-tolerant accessors over JSON-GLib nodes: a missing or mistyped member
// yields an empty value instead of a GLib critical.

#include <json-glib/json-glib.h>

#include <memory>
#include <optional>
#include <string>

namespace yrswatch::detail {

using Json = std::unique_ptr<JsonParser, decltype(&g_object_unref)>;

// Parses a client's JSON output. Notices printed before the document are skipped.
// Unparseable output raises NeedsLogin when it looks like a login prompt,
// otherwise Unavailable.
Json parse(const std::string& data, const std::string& diagnostics = {});

inline JsonObject* object(JsonNode* n) {
    return n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : nullptr;
}
inline JsonObject* root_object(const Json& json) { return object(json_parser_get_root(json.get())); }
inline JsonNode* get(JsonObject* obj, const char* key) {
    return obj ? json_object_get_member(obj, key) : nullptr;
}
inline std::string str(JsonNode* n) {
    return n && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : "";
}
inline std::optional<double> number(JsonNode* n) {
    if (!n) return {};
    if (json_node_get_value_type(n) == G_TYPE_INT64) return static_cast<double>(json_node_get_int(n));
    if (json_node_get_value_type(n) == G_TYPE_DOUBLE) return json_node_get_double(n);
    return {};
}
inline JsonArray* array(JsonNode* n) {
    return n && JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n) : nullptr;
}

} // namespace yrswatch::detail
