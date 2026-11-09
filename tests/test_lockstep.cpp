// Scheduler regression checks without ROMs or a running guest CPU.
#include "runtime/lockstep.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>

namespace {
size_t allocated_bytes = 0;
bool measure_allocations = false;
void check(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
void at(rt::Lockstep &scheduler, uint64_t count) {
    scheduler.count = count;
    scheduler.boundary();
}
}

// Count allocations only during the steady-state recurrence test. A small,
// trivial callback fits std::function's inline storage; growing scheduler
// containers must not scale with the number of completed callbacks.
void *operator new(size_t size) {
    if (measure_allocations) allocated_bytes += size;
    if (void *p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }

namespace {
void ordering() {
    rt::Cpu cpu(nullptr);
    rt::Lockstep scheduler(cpu);
    std::vector<int> seen;
    scheduler.add_callback(5, [&] { seen.push_back(1); });
    scheduler.add_callback(7, [&] { seen.push_back(7); });
    scheduler.add_callback(3, [&] { seen.push_back(3); });
    scheduler.add_callback(5, [&] {
        seen.push_back(2);
        scheduler.add_callback(5, [&] { seen.push_back(4); });
        scheduler.add_callback(6, [&] { seen.push_back(6); });
        seen.push_back(9); // the running callback survives reuse of its slot
    });
    at(scheduler, 2);
    check(seen.empty() && scheduler.next_count == 3, "callback fired early");
    for (unsigned count = 3; count <= 7; ++count) at(scheduler, count);
    check(seen == std::vector<int>({3, 2, 9, 4, 1, 6, 7}), "callback order changed");
    check(scheduler.next_count == UINT64_MAX, "completed callback is still pending");
    at(scheduler, 8);
    check(seen.size() == 7, "callback ran more than once");
}

void lifetime_and_growth() {
    rt::Cpu cpu(nullptr);
    rt::Lockstep scheduler(cpu);
    auto payload = std::make_shared<std::array<unsigned, 64>>();
    (*payload)[63] = 1234;
    std::weak_ptr<std::array<unsigned, 64>> weak = payload;
    unsigned finished = 0;
    scheduler.add_callback(1, [&, payload] {
        // Force both event and callback storage to grow while this callback
        // is running. Its captures must remain valid even if its slot moves.
        for (unsigned i = 0; i < 8192; ++i)
            scheduler.add_callback(2, [&] { ++finished; });
        check((*payload)[63] == 1234 && !weak.expired(), "running capture was destroyed");
    });
    payload.reset();
    at(scheduler, 1);
    check(weak.expired(), "completed callback retained its captures");
    at(scheduler, 2);
    check(finished == 8192, "queued callbacks lost or repeated after growth");
    check(scheduler.next_count == UINT64_MAX, "queue did not drain after compaction");
}

void bounded_recurrence() {
    rt::Cpu cpu(nullptr);
    rt::Lockstep scheduler(cpu);
    struct Repeat {
        rt::Lockstep *scheduler;
        uint64_t *runs;
        void operator()() const {
            ++*runs;
            scheduler->add_callback(scheduler->count + 1, *this);
        }
    };
    uint64_t runs = 0;
    scheduler.add_callback(1, Repeat{&scheduler, &runs});
    for (unsigned i = 1; i <= 20000; ++i) at(scheduler, i);
    allocated_bytes = 0;
    measure_allocations = true;
    for (unsigned i = 20001; i <= 1020000; ++i) at(scheduler, i);
    measure_allocations = false;
    check(runs == 1020000 && scheduler.next_count == 1020001, "recurrence drifted");
    // Allow a little implementation-specific bookkeeping, but reject storage
    // that continues growing with a million completed callbacks.
    check(allocated_bytes < 4096, "steady-state callback storage keeps growing");
    std::printf("1,000,000 callbacks after warmup: %zu bytes allocated\n", allocated_bytes);
}

void replay_order() {
    struct InterruptBus : rt::Bus {
        uint32_t fetch(uint32_t) override { throw std::runtime_error("unexpected fetch"); }
        uint8_t read_byte(uint32_t) override { throw std::runtime_error("unexpected byte read"); }
        uint16_t read_word(uint32_t) override { throw std::runtime_error("unexpected word read"); }
        uint32_t read_dword(uint32_t address) override {
            check(address == 20, "unexpected interrupt table address");
            return 0;
        }
        void write_byte(uint32_t, uint8_t) override { throw std::runtime_error("unexpected byte write"); }
        void write_word(uint32_t, uint16_t) override { throw std::runtime_error("unexpected word write"); }
        void write_dword(uint32_t, uint32_t) override { throw std::runtime_error("unexpected dword write"); }
    } bus;
    const auto path = std::filesystem::path("test_lockstep_irq.log");
    struct RemoveLog {
        std::filesystem::path path;
        ~RemoveLog() { std::error_code error; std::filesystem::remove(path, error); }
    } remove{path};
    {
        std::ofstream log(path);
        log << "line 10 0 1\nend 20\n";
        check(bool(log), "cannot write replay fixture");
    }
    rt::Cpu cpu(&bus);
    rt::Lockstep scheduler(cpu, path.string());
    cpu.m_ICR = 0x40;
    auto payload = std::make_shared<int>(0x20);
    std::weak_ptr<int> weak = payload;
    scheduler.add_callback(10, [&, payload] { cpu.m_ICR = *payload; });
    payload.reset();
    at(scheduler, 10);
    check(cpu.m_immediate_irq && cpu.m_immediate_vector == 0x20,
          "callback no longer precedes same-count interrupt line");
    check(weak.expired(), "replay callback retained its captures");
    at(scheduler, 20);
    check(scheduler.finished(), "replay end count changed");
}
}

int main() {
    try {
        bounded_recurrence();
        ordering();
        lifetime_and_growth();
        replay_order();
        std::puts("PASS callback ordering, lifetime, storage growth and replay");
    } catch (const std::exception &error) {
        measure_allocations = false;
        std::fprintf(stderr, "lockstep: %s\n", error.what());
        return 1;
    }
}
