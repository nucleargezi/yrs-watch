#pragma once

// One fetch function per source, dispatched by fetch_quota(). Each may throw
// detail::Failure; provider, observed_at and the empty-result check are
// filled in by the dispatcher unless the source sets observed_at itself.

#include "core.hpp"

#include <stop_token>

namespace yrswatch::detail {

QuotaSample fetch_codex(std::stop_token stop);
QuotaSample fetch_antigravity(std::stop_token stop);
QuotaSample fetch_claude(std::stop_token stop);

} // namespace yrswatch::detail
