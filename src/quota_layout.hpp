#pragma once

#include "tray.hpp"

namespace yrswatch {

// Each entry contains the native multiline menu labels for one source, in page order.
using GroupPages = std::vector<std::vector<std::string>>;

class QuotaLayout {
public:
    explicit QuotaLayout(std::function<void()> changed);
    ~QuotaLayout();
    GroupPages format(const std::vector<TrayGroup>& groups, bool defer);
private:
    struct Font;
    std::function<void()> changed_;
    std::shared_ptr<Font> font_;
};

} // namespace yrswatch
