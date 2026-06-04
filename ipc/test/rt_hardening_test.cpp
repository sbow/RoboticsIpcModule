// C7 — real-time hardening hook smoke test (ADR 0016).
//
// router_lock_memory() / router_pin_to_core() are opt-in init hooks. They must
// never throw and must report success/failure as a bool so callers can log and
// continue. This test exercises the contract; it does NOT require the hooks to
// succeed (mlockall needs CAP_IPC_LOCK; affinity needs the core to exist), only
// that they behave predictably for the input-validation cases and don't crash.

#include "router_app.h"

#include <iostream>
#include <unistd.h>

namespace {

int g_total  = 0;
int g_failed = 0;

#define EXPECT(cond)                                                        \
    do {                                                                    \
        ++g_total;                                                          \
        if (!(cond)) {                                                      \
            ++g_failed;                                                     \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__             \
                      << " EXPECT(" #cond ")\n";                            \
        }                                                                   \
    } while (0)

void test_pin_to_negative_core_is_rejected() {
    EXPECT(router_pin_to_core(-1) == false);
    EXPECT(router_pin_to_core(-1000) == false);
}

void test_pin_to_core_zero_does_not_crash() {
    // Core 0 exists on every machine. On most hosts this succeeds; in a
    // restricted sandbox it may fail. Either way it must return cleanly.
    const bool ok = router_pin_to_core(0);
    (void)ok;
    EXPECT(true);  // reached here without throwing/crashing
}

void test_pin_to_absurd_core_returns_false() {
    // A core number far beyond any real CPU set should fail, not crash.
    EXPECT(router_pin_to_core(1 << 20) == false);
}

void test_lock_memory_returns_bool_without_throwing() {
    // Succeeds with CAP_IPC_LOCK / sufficient RLIMIT_MEMLOCK; fails otherwise.
    // The contract is "no throw, returns a bool" — assert we got here.
    const bool ok = router_lock_memory();
    (void)ok;
    EXPECT(true);
}

}  // namespace

int main() {
    test_pin_to_negative_core_is_rejected();
    test_pin_to_core_zero_does_not_crash();
    test_pin_to_absurd_core_returns_false();
    test_lock_memory_returns_bool_without_throwing();

    std::cout << "rt_hardening_test: " << (g_total - g_failed) << '/'
              << g_total << " assertions passed\n";
    return g_failed == 0 ? 0 : 1;
}
