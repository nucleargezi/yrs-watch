#include "providers.hpp"

#include "child_process.hpp"
#include "provider_impl.hpp"

namespace yrswatch {

QuotaResult fetch_quota(Provider provider, std::stop_token stop) {
    try {
        if (stop.stop_requested()) detail::fail(QuotaError::Cancelled, "已取消");
        QuotaSample sample;
        switch (provider) {
            case Provider::Codex: sample = detail::fetch_codex(stop); break;
            case Provider::Antigravity: sample = detail::fetch_antigravity(stop); break;
        }
        if (sample.rows.empty()) detail::fail(QuotaError::Unavailable, "客户端未返回额度窗口");
        sample.provider = provider;
        if (sample.observed_at <= 0) sample.observed_at = now_epoch();
        return {std::move(sample), QuotaError::None, {}};
    } catch (const detail::Failure& f) {
        return {{}, f.code, f.message};
    }
}

} // namespace yrswatch
