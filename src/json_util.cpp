#include "json_util.hpp"

#include "child_process.hpp"

namespace yrswatch::detail {

Json parse(const std::string& data, const std::string& diagnostics) {
    Json parser(json_parser_new(), g_object_unref);
    if (json_parser_load_from_data(parser.get(), data.data(), static_cast<gssize>(data.size()), nullptr))
        return parser;
    // Some CLI builds print notices before the JSON document. Retry from each
    // line that opens an object, starting with the last one.
    std::size_t end = data.size();
    for (int attempt = 0; attempt < 16 && end > 0; ++attempt) {
        const auto start = data.rfind("\n{", end - 1);
        if (start == std::string::npos) break;
        const auto offset = start + 1;
        if (json_parser_load_from_data(parser.get(), data.data() + offset,
                                       static_cast<gssize>(data.size() - offset), nullptr))
            return parser;
        end = start;
    }
    if (suggests_login(data)) cli_failure(data);
    if (suggests_login(diagnostics)) cli_failure(diagnostics);
    invalid_response();
}

} // namespace yrswatch::detail
