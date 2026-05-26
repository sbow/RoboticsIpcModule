// Phase D1 — log_path_for_role arity regression (LESSONS-LEARNED.md).
//
// The router_client demo CLI grammar gives controller and recorder asymmetric
// argv positions for their log path arguments. We document and lock that
// arity here so the CLI is never silently re-shuffled.
//
// Arity (also documented in ipc/test/router_cli_args.hpp):
//   role = "controller" → argv[5] preferred, argv[3] fallback, then fallback_fn
//   role = "recorder"   → argv[4] preferred, argv[3] fallback, then fallback_fn
//   unknown role        → "" (fallback_fn not invoked)

#include "router_cli_args.hpp"

#include <cstring>
#include <iostream>
#include <string>

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

#define EXPECT_STR_EQ(a, b)                                                 \
    do {                                                                    \
        ++g_total;                                                          \
        const std::string _a = (a);                                         \
        const std::string _b = (b);                                         \
        if (_a != _b) {                                                     \
            ++g_failed;                                                     \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__             \
                      << " EXPECT_STR_EQ(" #a ", " #b ") -> '"              \
                      << _a << "' != '" << _b << "'\n";                     \
        }                                                                   \
    } while (0)

// Stand-in fallback that returns a deterministic per-role default. The
// real demo binds `demo_log_path`; the test binds this one so we don't
// pull the demo's hardcoded paths into the regression surface.
const char* test_fallback(const char* role) {
    if (std::strcmp(role, "controller") == 0) {
        return "/tmp/test_controller.log";
    }
    if (std::strcmp(role, "recorder") == 0) {
        return "/tmp/test_recorder.log";
    }
    return nullptr;
}

// Build a fake argv. C++ string literals decay to const char*; the helper
// takes char*[] (POSIX argv shape) so we cast away const for the test
// fixture. Safe — the helper only reads.
char* arg(const char* s) {
    return const_cast<char*>(s);
}

void test_controller_argv_5_wins_over_argv_3_and_fallback() {
    char* argv[] = {
        arg("router_client"),     // argv[0]
        arg("controller"),        // argv[1]
        arg("uds"),               // argv[2]
        arg("/tmp/router.sock"),  // argv[3]  (router path)
        arg("/tmp/client.sock"),  // argv[4]  (client path)
        arg("/tmp/wanted.log"),   // argv[5]  ← controller log path
    };
    EXPECT_STR_EQ(
        log_path_for_role("controller", 6, argv, &test_fallback),
        "/tmp/wanted.log");
}

void test_controller_falls_back_to_argv_3_when_argv_5_absent() {
    char* argv[] = {
        arg("router_client"),
        arg("controller"),
        arg("uds"),
        arg("/tmp/fallback.log"),   // argv[3] — only positional present
    };
    EXPECT_STR_EQ(
        log_path_for_role("controller", 4, argv, &test_fallback),
        "/tmp/fallback.log");
}

void test_controller_falls_back_to_fallback_fn_when_only_argv_0_1_2() {
    char* argv[] = {
        arg("router_client"),
        arg("controller"),
        arg("uds"),
    };
    EXPECT_STR_EQ(
        log_path_for_role("controller", 3, argv, &test_fallback),
        "/tmp/test_controller.log");
}

void test_controller_argc_just_below_argv_5_does_not_step_past_array() {
    // argc == 5 means argv[5] is NOT present — must use argv[3] instead.
    char* argv[] = {
        arg("router_client"),
        arg("controller"),
        arg("uds"),
        arg("/tmp/wanted.log"),
        arg("/tmp/client.sock"),
    };
    EXPECT_STR_EQ(
        log_path_for_role("controller", 5, argv, &test_fallback),
        "/tmp/wanted.log");
}

void test_recorder_argv_4_wins_over_argv_3_and_fallback() {
    char* argv[] = {
        arg("router_client"),
        arg("recorder"),
        arg("uds"),
        arg("/tmp/client.sock"),   // argv[3]
        arg("/tmp/wanted.log"),    // argv[4] ← recorder log path
    };
    EXPECT_STR_EQ(
        log_path_for_role("recorder", 5, argv, &test_fallback),
        "/tmp/wanted.log");
}

void test_recorder_falls_back_to_argv_3_when_argv_4_absent() {
    char* argv[] = {
        arg("router_client"),
        arg("recorder"),
        arg("uds"),
        arg("/tmp/fallback.log"),
    };
    EXPECT_STR_EQ(
        log_path_for_role("recorder", 4, argv, &test_fallback),
        "/tmp/fallback.log");
}

void test_recorder_argc_just_below_argv_4_does_not_step_past_array() {
    // argc == 4 means argv[4] is NOT present — must use argv[3] instead.
    char* argv[] = {
        arg("router_client"),
        arg("recorder"),
        arg("uds"),
        arg("/tmp/three.log"),
    };
    EXPECT_STR_EQ(
        log_path_for_role("recorder", 4, argv, &test_fallback),
        "/tmp/three.log");
}

void test_recorder_falls_back_to_fallback_fn_when_only_argv_0_1_2() {
    char* argv[] = {
        arg("router_client"),
        arg("recorder"),
        arg("uds"),
    };
    EXPECT_STR_EQ(
        log_path_for_role("recorder", 3, argv, &test_fallback),
        "/tmp/test_recorder.log");
}

void test_recorder_does_not_consume_argv_5() {
    // Even with argc>=6 the recorder branch must NOT pick argv[5] (that
    // was the original Phase A bug). It should pick argv[4].
    char* argv[] = {
        arg("router_client"),
        arg("recorder"),
        arg("uds"),
        arg("/tmp/three.log"),
        arg("/tmp/four.log"),       // ← recorder uses this
        arg("/tmp/five_should_be_ignored.log"),
    };
    EXPECT_STR_EQ(
        log_path_for_role("recorder", 6, argv, &test_fallback),
        "/tmp/four.log");
}

void test_unknown_role_returns_empty_and_does_not_invoke_fallback() {
    // The fallback returns nullptr for "sensor", but we should not even
    // attempt to use it — unknown roles short-circuit to empty string.
    char* argv[] = {
        arg("router_client"),
        arg("sensor"),
        arg("uds"),
    };
    EXPECT_STR_EQ(
        log_path_for_role("sensor", 3, argv, &test_fallback),
        "");
    EXPECT_STR_EQ(
        log_path_for_role("dashboard", 3, argv, &test_fallback),
        "");
}

void test_null_fallback_returns_empty_when_argv_too_short() {
    char* argv[] = {
        arg("router_client"),
        arg("controller"),
        arg("uds"),
    };
    EXPECT_STR_EQ(
        log_path_for_role("controller", 3, argv, nullptr),
        "");
}

void test_fallback_returning_null_yields_empty_string_not_crash() {
    // Unknown role passed to the real fallback returns nullptr; but we
    // also want the safety net for a misconfigured fallback that returns
    // nullptr for a known role. The helper must produce "", not crash.
    auto null_fallback = +[](const char*) -> const char* { return nullptr; };
    char* argv[] = {
        arg("router_client"),
        arg("controller"),
        arg("uds"),
    };
    EXPECT_STR_EQ(
        log_path_for_role("controller", 3, argv, null_fallback),
        "");
}

}  // namespace

int main() {
    test_controller_argv_5_wins_over_argv_3_and_fallback();
    test_controller_falls_back_to_argv_3_when_argv_5_absent();
    test_controller_falls_back_to_fallback_fn_when_only_argv_0_1_2();
    test_controller_argc_just_below_argv_5_does_not_step_past_array();

    test_recorder_argv_4_wins_over_argv_3_and_fallback();
    test_recorder_falls_back_to_argv_3_when_argv_4_absent();
    test_recorder_argc_just_below_argv_4_does_not_step_past_array();
    test_recorder_falls_back_to_fallback_fn_when_only_argv_0_1_2();
    test_recorder_does_not_consume_argv_5();

    test_unknown_role_returns_empty_and_does_not_invoke_fallback();
    test_null_fallback_returns_empty_when_argv_too_short();
    test_fallback_returning_null_yields_empty_string_not_crash();

    std::cout << "cli_args_test: " << (g_total - g_failed) << '/'
              << g_total << " assertions passed\n";
    return g_failed == 0 ? 0 : 1;
}
