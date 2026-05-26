// Unit tests for ipc/src/router/last_value_cache.hpp.

#include "router/frame.hpp"
#include "router/last_value_cache.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace {

int g_failed = 0;
int g_total  = 0;

#define EXPECT(cond)                                                        \
    do {                                                                    \
        ++g_total;                                                          \
        if (!(cond)) {                                                      \
            ++g_failed;                                                     \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__             \
                      << " EXPECT(" #cond ")\n";                            \
        }                                                                   \
    } while (0)

#define EXPECT_EQ(a, b)                                                     \
    do {                                                                    \
        ++g_total;                                                          \
        const auto _a = (a);                                                \
        const auto _b = (b);                                                \
        if (!(_a == _b)) {                                                  \
            ++g_failed;                                                     \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__             \
                      << " EXPECT_EQ(" #a ", " #b ") -> "                   \
                      << _a << " != " << _b << "\n";                        \
        }                                                                   \
    } while (0)

RouterFrame make_frame(uint8_t source, uint64_t ts_ns, std::string_view payload) {
    RouterFrame f;
    f.init(source);
    f.set_timestamp_ns(ts_ns);
    f.set_payload(payload);
    return f;
}

void test_empty_cache_returns_false() {
    LastValueCache<> cache;
    RouterFrame out;
    EXPECT(!cache.has(1));
    EXPECT(!cache.latest(1, out));
    EXPECT_EQ(cache.size(), static_cast<std::size_t>(0));
}

void test_update_then_latest() {
    LastValueCache<> cache;
    RouterFrame in = make_frame(7, 12345, "hello");
    cache.update(7, in);

    EXPECT(cache.has(7));
    EXPECT_EQ(cache.size(), static_cast<std::size_t>(1));

    RouterFrame out;
    EXPECT(cache.latest(7, out));
    EXPECT_EQ(static_cast<int>(out.source()), 7);
    EXPECT_EQ(out.timestamp_ns(), static_cast<uint64_t>(12345));
    EXPECT(out.payload() == std::string("hello"));
}

void test_overwrite_keeps_latest_only() {
    LastValueCache<> cache;
    cache.update(4, make_frame(4, 1, "old"));
    cache.update(4, make_frame(4, 2, "newer"));
    cache.update(4, make_frame(4, 3, "newest"));

    EXPECT_EQ(cache.size(), static_cast<std::size_t>(1));
    RouterFrame out;
    EXPECT(cache.latest(4, out));
    EXPECT_EQ(out.timestamp_ns(), static_cast<uint64_t>(3));
    EXPECT(out.payload() == std::string("newest"));
}

void test_independent_sources() {
    LastValueCache<> cache;
    cache.update(1, make_frame(1, 10, "a"));
    cache.update(2, make_frame(2, 20, "b"));
    cache.update(3, make_frame(3, 30, "c"));
    EXPECT_EQ(cache.size(), static_cast<std::size_t>(3));

    RouterFrame out;
    EXPECT(cache.latest(2, out));
    EXPECT(out.payload() == std::string("b"));
    EXPECT(cache.latest(3, out));
    EXPECT(out.payload() == std::string("c"));
    EXPECT(!cache.has(99));
}

void test_clear_drops_everything() {
    LastValueCache<> cache;
    cache.update(1, make_frame(1, 1, "x"));
    cache.update(2, make_frame(2, 2, "y"));
    EXPECT_EQ(cache.size(), static_cast<std::size_t>(2));
    cache.clear();
    EXPECT_EQ(cache.size(), static_cast<std::size_t>(0));
    EXPECT(!cache.has(1));
    EXPECT(!cache.has(2));
}

void test_small_n_template_param() {
    LastValueCache<4> cache;
    cache.update(0, make_frame(0, 1, "zero"));
    cache.update(3, make_frame(3, 4, "three"));
    EXPECT(cache.has(0));
    EXPECT(cache.has(3));
    // Source id beyond N is silently ignored — caller's responsibility.
    cache.update(4, make_frame(4, 5, "out"));
    EXPECT(!cache.has(4));
}

}  // namespace

int main() {
    test_empty_cache_returns_false();
    test_update_then_latest();
    test_overwrite_keeps_latest_only();
    test_independent_sources();
    test_clear_drops_everything();
    test_small_n_template_param();

    std::cout << "last_value_cache_test: " << (g_total - g_failed) << '/'
              << g_total << " assertions passed\n";
    return g_failed == 0 ? 0 : 1;
}
