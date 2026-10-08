// vita::FlatIndex (gpu_gl.cpp's per-polygon lookups) against std::unordered_map:
// inserts, lookups, O(1) clears, and the 3/4 fill limit.
#include "../platform/vita/flat_index.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <unordered_map>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "check failed: %s\n", #x); std::exit(1); } } while (0)

int main() {
    std::mt19937_64 rng(0xf1a7);
    vita::FlatIndex<uint64_t, uint32_t, 12> index;
    std::unordered_map<uint64_t, uint32_t> reference;
    for (unsigned round = 0; round < 200; ++round) {
        const unsigned inserts = unsigned(rng() % (index.kMaxCount + 200));
        for (unsigned i = 0; i < inserts; ++i) {
            const uint64_t key = rng() % 5000u * 0x100000001ull; // collisions and repeats
            if (reference.count(key)) continue;
            const uint32_t value = uint32_t(rng());
            const bool added = index.insert(key, value);
            CHECK(added == (reference.size() < index.kMaxCount));
            if (added) reference.emplace(key, value);
        }
        CHECK(index.size() == reference.size());
        for (unsigned i = 0; i < 20000; ++i) {
            const uint64_t key = rng() % 6000u * 0x100000001ull;
            const auto found = reference.find(key);
            const uint32_t *value = index.find(key);
            CHECK((value != nullptr) == (found != reference.end()));
            if (value) CHECK(*value == found->second);
        }
        index.clear();
        reference.clear();
        CHECK(index.size() == 0 && !index.find(0));
    }
    std::puts("vita::FlatIndex matches std::unordered_map: inserts, lookups, clears, fill limit");
}
