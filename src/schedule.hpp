#pragma once

#include "core.hpp"

#include <algorithm>

namespace yrswatch {

// Deadlines use monotonic seconds. Only the provider's reset timestamps use wall time.
struct Schedule {
    std::int64_t due;
    unsigned failures = 0;
    Epoch checked_reset = 0;

    void completed(bool success, std::int64_t now) {
        failures = success ? 0 : std::min(failures + 1, 3u);
        const int delay = failures == 1 ? 120 : failures == 2 ? 300 : 900;
        due = now + delay;
    }

    std::optional<Epoch> next_reset(const ProviderState& state) const {
        std::optional<Epoch> earliest;
        if (failures || !state.last) return earliest;
        for (const auto& row : state.last->rows) {
            if (row.reset_at && *row.reset_at > checked_reset &&
                (!earliest || *row.reset_at < *earliest)) earliest = row.reset_at;
        }
        return earliest;
    }
};

} // namespace yrswatch
