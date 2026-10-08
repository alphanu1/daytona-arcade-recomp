#include "../platform/vita/core_policy.h"
#include <cstdlib>
#include <vector>
#define CHECK(x) do { if (!(x)) std::abort(); } while (0)
int main() {
    std::vector<int> calls;
    int actual = 333;
    auto set = [&](int n) { calls.push_back(n); actual = n; return 0; };
    CHECK(vita::request_cpu_clock(500, set, [&] { return actual; }) == 0);
    CHECK(actual == 500 && calls.size() == 1);
    calls.clear();
    auto reject = [&](int n) { calls.push_back(n); if (n == 500) return -1; actual = n; return 0; };
    CHECK(vita::request_cpu_clock(500, reject, [&] { return actual; }) == 0);
    CHECK(actual == 444 && calls == std::vector<int>({500,444}));
    calls.clear();
    CHECK(vita::request_cpu_clock(500, set, [] { return 333; }) == 0);
    CHECK(calls == std::vector<int>({500,444}));
    calls.clear();
    CHECK(vita::request_cpu_clock(333, set, [&] { return actual; }) == 0);
    CHECK(calls == std::vector<int>({333}));
    CHECK(vita::request_core_mask(15, 7, set, [&] { return actual; }));
    CHECK(actual == 15);
    CHECK(!vita::request_core_mask(15, 7, set, [] { return 7; }));
    CHECK(actual == 7);
    CHECK(!vita::request_core_mask(15, 7, [](int) { return -1; }, [] { return 15; }));
    CHECK(vita::request_core_mask(7, 7, set, [&] { return actual; }));
    CHECK(actual == 7);
    // Pinned cores (default GPU builds) vs unpinned (-1, --free-core).
    static_assert(vita::core_pin_mask(0) == 0x10000 && vita::core_pin_mask(2) == 0x40000);
    static_assert(vita::core_pin_mask(-1) == 0 && vita::core_pin_mask(3) == 0);
}
