#include "core.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>

using namespace yrswatch;
namespace fs = std::filesystem;

namespace {

void check(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}

QuotaSample observation(double remaining, Epoch now, std::optional<Epoch> reset = 2000,
                        std::string account = "account-a") {
    QuotaRow row;
    row.id = "pool/weekly";
    row.label = "模型额度";
    row.group = "pool";
    row.window = "weekly";
    row.remaining = remaining;
    row.reset_at = reset;
    QuotaSample sample;
    sample.account = std::move(account);
    sample.observed_at = now;
    sample.rows.push_back(std::move(row));
    return sample;
}

std::vector<Alert> apply(ProviderState& state, QuotaSample sample) {
    const auto now = sample.observed_at;
    return apply_sample(state, std::move(sample), .2, now);
}

void expect(const std::vector<Alert>& alerts, Alert::Kind kind,
            std::source_location caller = std::source_location::current()) {
    if (alerts.size() != 1 || alerts[0].kind != kind || alerts[0].row_id != "pool/weekly") {
        throw std::runtime_error("Unexpected alert at core_test.cpp:" + std::to_string(caller.line()));
    }
}

void test_quota(const fs::path& path) {
    auto initial = observation(0.2, 1000);
    check(!stale(initial, 1899) && stale(initial, 1900) && stale(initial, 999), "sample age");
    check(!expired(initial.rows[0], 1999) && expired(initial.rows[0], 2000), "window boundary");
    ProviderState state;
    expect(apply(state, initial), Alert::Kind::Low);
    check(apply(state, observation(.22, 1001)).empty(), "above low watermark");
    auto other = observation(.77, 1001);
    other.provider = Provider::Antigravity;
    other.rows[0].quantity = 1234.5;
    other.rows[0].remaining.reset();
    other.rows[0].unit = "credits";
    other.rows[0].reset_at.reset();
    check(save_cache(path, {*state.last, other}), "cache save");
    struct stat mode{};
    check(stat(path.c_str(), &mode) == 0 && (mode.st_mode & 0777) == 0600, "private cache mode");
    check(stat(path.parent_path().c_str(), &mode) == 0 && (mode.st_mode & 0777) == 0700, "private directory");
    auto loaded = load_cache(path);
    check(loaded.size() == 2 && loaded[0].rows[0].label == "模型额度" &&
          loaded[1].rows[0].quantity == 1234.5 && !loaded[1].rows[0].remaining &&
          !loaded[1].rows[0].reset_at && loaded[1].rows[0].unit == "credits", "cache round trip");
    state.last = loaded[0];
    check(apply(state, observation(.18, 1002, 2002)).empty(), "restart and reset time jitter");
    expect(apply(state, observation(0, 1003)), Alert::Kind::Exhausted);
    check(apply(state, observation(0, 1004)).empty(), "already exhausted");
    check(apply(state, observation(.9, 1001)).empty() && state.last->observed_at == 1004, "out-of-order sample");
    check(apply(state, observation(.75, 2000, 2000)).empty(), "expired row cannot report recovery");
    expect(apply(state, observation(.80, 2001, 3000)), Alert::Kind::Recovered);
    check(apply(state, observation(.79, 2002, 3000)).empty(), "recovery once");
    expect(apply(state, observation(.10, 2003, 3000)), Alert::Kind::Low);
    expect(apply(state, observation(.09, 2004, 3000, "account-b")), Alert::Kind::Low);
    check(apply_sample(state, observation(0, 2005, 11000, "account-b"), .2, 5000).empty(), "stale data cannot alert");
    expect(apply(state, observation(0, 5001, 11000, "account-b")), Alert::Kind::Exhausted);
    auto unknown = observation(std::numeric_limits<double>::quiet_NaN(), 5002, 11000, "account-b");
    check(apply(state, unknown).empty() && !state.last->rows[0].remaining, "unknown preserves recovery state");
    expect(apply(state, observation(.05, 5003, 11000, "account-b")), Alert::Kind::Recovered);

    ProviderState quantity;
    other.rows[0].quantity = 0;
    expect(apply(quantity, other), Alert::Kind::Exhausted);
    other.rows[0].quantity = 1;
    expect(apply(quantity, other), Alert::Kind::Recovered);
    other.rows[0].quantity = 0;
    expect(apply(quantity, other), Alert::Kind::Exhausted);
    auto refill = observation(0, 1000, {});
    ProviderState no_reset;
    expect(apply(no_reset, refill), Alert::Kind::Exhausted);
    refill.rows[0].remaining = 1;
    expect(apply(no_reset, refill), Alert::Kind::Recovered);
    refill.rows[0].remaining = 0;
    expect(apply(no_reset, refill), Alert::Kind::Exhausted);

    for (const Epoch new_deadline : {2000, 2100}) {
        ProviderState early_refill;
        expect(apply(early_refill, observation(.19, 1000)), Alert::Kind::Low);
        check(apply(early_refill, observation(1, 1001, new_deadline)).empty(), "early full refill");
        expect(apply(early_refill, observation(.18, 1002, new_deadline)), Alert::Kind::Low);
    }

    fs::path link = path.string() + ".symlink";
    fs::create_symlink(path, link);
    check(load_cache(link).empty(), "reject symlink cache");
    check(chmod(path.c_str(), 0644) == 0 && load_cache(path).empty(), "reject nonprivate cache");
    check(save_cache(path, {initial}), "atomic replacement");
    check(stat(path.c_str(), &mode) == 0 && (mode.st_mode & 0777) == 0600, "replaced file is private");
    std::ofstream(path) << "[cache]\nversion=2\n";
    check(load_cache(path).empty(), "unknown cache version");

    // An explicit disabled field round-trips and never alerts.
    QuotaSample disabled_sample = observation(0.0, 2000);
    disabled_sample.rows[0].disabled = true;
    disabled_sample.rows[0].remaining.reset();
    disabled_sample.rows[0].reset_at.reset();
    ProviderState disabled_state;
    check(apply(disabled_state, disabled_sample).empty(), "disabled row does not alert");
    check(save_cache(path, {*disabled_state.last}), "save disabled sample");
    auto loaded_disabled = load_cache(path);
    check(loaded_disabled.size() == 1 && loaded_disabled[0].rows[0].disabled &&
          loaded_disabled[0].rows[0].label == "模型额度", "disabled round trip");

    // Legacy caches that only mark disabled rows with a label suffix are migrated.
    std::ofstream(path)
        << "[cache]\nversion=1\n\n"
        << "[antigravity]\nobserved=1500\nrows=2\nalerts=0\n\n"
        << "[antigravity.row.0]\nid=pool/disabled\nlabel=旧模型（未开放）\n\n"
        << "[antigravity.row.1]\nid=pool/only-suffix\nlabel=（未开放）\n";
    check(chmod(path.c_str(), 0600) == 0, "legacy cache mode");
    auto migrated = load_cache(path);
    check(migrated.size() == 1 && migrated[0].rows.size() == 2, "legacy cache count");
    check(migrated[0].rows[0].disabled && migrated[0].rows[0].label == "旧模型", "legacy suffix stripped");
    check(migrated[0].rows[1].disabled && migrated[0].rows[1].label == "pool/only-suffix", "legacy empty label fallback");
}

void test_metrics() {
    std::istringstream memory("MemTotal:       16384 kB\nMemFree:          100 kB\nMemAvailable:    8192 kB\n");
    const auto ram = parse_memory(memory);
    check(ram && ram->total == 16777216 && ram->used == 8388608 && ram->available == 8388608, "Linux memory accounting");
    std::istringstream missing("MemTotal: 1024 kB\nMemFree: 512 kB\n");
    check(!parse_memory(missing), "missing MemAvailable");
    std::istringstream cpu("cpu  100 10 20 500 15 5 5 2 10 0\ncpu0 0 0 0 0\n");
    auto ticks = parse_cpu(cpu);
    check(ticks && ticks->active == 142 && ticks->total == 657, "aggregate CPU ignores guest duplicate");
    check(cpu_fraction(*ticks, {192, 857}) == 0.25, "CPU fraction");
    check(!cpu_fraction(*ticks, *ticks) && !cpu_fraction(*ticks, {0, 700}), "invalid CPU delta");
    std::istringstream incomplete("cpu 100 2 3\n");
    check(!parse_cpu(incomplete), "incomplete CPU data");
    check(read_cpu().has_value() && read_memory().has_value(), "host Linux metrics");
}

} // namespace

int main() {
    char temp[] = "/tmp/yrs-watch-core-XXXXXX";
    if (!mkdtemp(temp)) return 1;
    try {
        test_quota(fs::path(temp) / "state" / "cache");
        test_metrics();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        fs::remove_all(temp);
        return 1;
    }
    fs::remove_all(temp);
    std::cout << "core tests passed\n";
}
