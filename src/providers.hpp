#pragma once

#include "core.hpp"

#include <filesystem>
#include <stop_token>

namespace yrswatch {

// Offline: the client ran but could not complete the lookup, usually because its
// service is unreachable. The UI shows a neutral waiting state rather than a message.
enum class QuotaError { None, NeedsLogin, Missing, Offline, Unavailable, Cancelled, Timeout };

struct QuotaResult {
    std::optional<QuotaSample> sample;
    QuotaError error = QuotaError::None;
    std::string message;
};

// Call from a worker thread. Cancellation wakes a pending read and retires
// this request's process group before returning.
QuotaResult fetch_quota(Provider provider, std::stop_token stop = {});

// The file a SourceKind::Snapshot provider reads; empty for command-line sources.
std::filesystem::path snapshot_file(Provider provider);

} // namespace yrswatch
