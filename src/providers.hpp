#pragma once

#include "core.hpp"

#include <stop_token>

namespace yrswatch {

enum class QuotaError { None, NeedsLogin, Missing, Unavailable, Cancelled, Timeout };

struct QuotaResult {
    std::optional<QuotaSample> sample;
    QuotaError error = QuotaError::None;
    std::string message;
};

// Call from a worker thread. Cancellation wakes a pending read and retires
// this request's process group before returning.
QuotaResult fetch_quota(Provider provider, std::stop_token stop = {});

} // namespace yrswatch
