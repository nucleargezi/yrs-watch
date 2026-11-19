#include "schedule.hpp"
#include "check.hpp"

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
    CHECK(s.next_reset(state) == 100);
    s.checked_reset = 100;
    CHECK(s.next_reset(state) == 130);
    s.completed(false, 11);
    CHECK(s.due == 131 && !s.next_reset(state));
    s.completed(false, 131);
    CHECK(s.due == 431);
    s.completed(false, 431);
    CHECK(s.due == 1331);
    s.completed(true, 500);
    CHECK(s.due == 1400 && s.next_reset(state) == 130);
    s.completed(false, 1400);
    s.resumed(1500);
    CHECK(s.due == 1505 && s.failures == 0);
    s.resumed(1400);
    CHECK(s.due == 1405);
    s.completed(true, 1405);
    s.resumed(1410);
    CHECK(s.due == 1415);
    std::puts("schedule tests passed");
}
