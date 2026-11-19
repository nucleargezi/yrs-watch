#include "schedule.hpp"
#include <cassert>

int main() {
    using namespace yrswatch;
    Schedule s{30};
    ProviderState state;
    QuotaSample sample;
    QuotaRow row;
    row.reset_at = 100;
    sample.rows.push_back(row);
    row.reset_at = 130;
    sample.rows.push_back(row);
    state.last = sample;
    assert(s.next_reset(state) == 100);
    s.checked_reset = 100;
    assert(s.next_reset(state) == 130);
    s.completed(false, 11);
    assert(s.due == 131 && !s.next_reset(state));
    s.completed(false, 131);
    assert(s.due == 431);
    s.completed(false, 431);
    assert(s.due == 1331);
    s.completed(true, 500);
    assert(s.due == 1400 && s.next_reset(state) == 130);
}
