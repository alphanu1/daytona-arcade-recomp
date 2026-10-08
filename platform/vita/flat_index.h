#pragma once
// Small open-addressing hash table for the renderer's per-polygon lookups (vitaGL path).
//
// std::unordered_map is slow on the Vita's Cortex-A9 for these: its bucket index is a
// modulo by a prime, and the A9 has no integer divide instruction (a library call of
// tens of cycles), each entry is a separate heap node (cache misses), and clear() frees
// every node (the batch index is cleared every frame). Here: a power-of-two table,
// multiplicative hash (no division), linear probing in one array, O(1) clear by epoch,
// no allocation after construction. Insert fails when 3/4 full; callers treat that as
// "not indexed" (they keep working, only slower).
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vita {

template <typename Key, typename Value, unsigned kBits>
class FlatIndex {
public:
    static constexpr uint32_t kCapacity = 1u << kBits;
    static constexpr uint32_t kMaxCount = kCapacity / 4u * 3u;

    FlatIndex() : entries_(kCapacity) {}

    const Value *find(Key key) const {
        for (uint32_t i = slot(key);; i = (i + 1) & (kCapacity - 1)) {
            const Entry &e = entries_[i];
            if (e.epoch != epoch_) return nullptr; // empty slot: not present
            if (e.key == key) return &e.value;
        }
    }
    // Adds key (must not be present). False when the table is too full.
    bool insert(Key key, Value value) {
        if (count_ >= kMaxCount) return false;
        uint32_t i = slot(key);
        while (entries_[i].epoch == epoch_) i = (i + 1) & (kCapacity - 1);
        entries_[i] = {key, value, epoch_};
        ++count_;
        return true;
    }
    void clear() {
        count_ = 0;
        if (++epoch_ == 0) { // wrapped after 4 billion clears: really empty the slots once
            for (Entry &e : entries_) e.epoch = 0;
            epoch_ = 1;
        }
    }
    uint32_t size() const { return count_; }

private:
    struct Entry {
        Key key{};
        Value value{};
        uint32_t epoch = 0; // slot used when equal to the table's epoch
    };
    static uint32_t slot(Key key) {
        const uint64_t k = uint64_t(key);
        const uint32_t mixed = uint32_t(k) ^ uint32_t(k >> 32) * 0x85ebca6bu;
        return (mixed * 0x9e3779b1u) >> (32u - kBits);
    }
    std::vector<Entry> entries_;
    uint32_t epoch_ = 1, count_ = 0;
};

} // namespace vita
