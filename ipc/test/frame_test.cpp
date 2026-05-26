// Phase D / ADR 0008 — verify RouterFrame v2 layout, field accessors, and
// byte-level wire compatibility. Every offset asserted here is part of the
// frozen v2 contract; bumping any of these requires another ADR + version
// bump.

#include "router/frame.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int assertions_run = 0;
int assertions_failed = 0;

#define EXPECT(cond) do {                                                  \
    ++assertions_run;                                                      \
    if (!(cond)) {                                                         \
        ++assertions_failed;                                               \
        std::cerr << "EXPECT failed @ " << __FILE__ << ':' << __LINE__     \
                  << " : " #cond "\n";                                    \
    }                                                                      \
} while (0)

#define EXPECT_EQ(a, b) do {                                               \
    ++assertions_run;                                                      \
    if ((a) != (b)) {                                                      \
        ++assertions_failed;                                               \
        std::cerr << "EXPECT_EQ failed @ " << __FILE__ << ':' << __LINE__  \
                  << " : " #a " == " #b " ("                              \
                  << +(a) << " vs " << +(b) << ")\n";                      \
    }                                                                      \
} while (0)

void test_version_and_size_constants() {
    EXPECT_EQ(kRouterFrameVersion, 2);
    EXPECT_EQ(kRouterFrameSize, 64u);
    EXPECT_EQ(sizeof(RouterFrame), kRouterFrameSize);

    EXPECT_EQ(kRouterSourceOffset, 0u);
    EXPECT_EQ(kRouterFlagsOffset, 1u);
    EXPECT_EQ(kRouterTopicIdOffset, 2u);
    EXPECT_EQ(kRouterSeqOffset, 4u);
    EXPECT_EQ(kRouterTimestampOffset, 8u);
    EXPECT_EQ(kRouterTimestampSize, 8u);
    EXPECT_EQ(kRouterSidebandIdxOffset, 16u);
    EXPECT_EQ(kRouterSidebandLenOffset, 18u);
    EXPECT_EQ(kRouterSidebandLenSize, 6u);
    EXPECT_EQ(kRouterSidebandSeqOffset, 24u);
    EXPECT_EQ(kRouterPayloadOffset, 32u);
    EXPECT_EQ(kRouterPayloadSize, 32u);

    EXPECT_EQ(kSidebandIdxNone, 0xFFFFu);
}

void test_init_zeroes_everything_and_stamps_source() {
    RouterFrame f;
    std::memset(f.bytes, 0xAB, kRouterFrameSize);

    f.init(42);

    EXPECT_EQ(f.source(), 42);
    EXPECT_EQ(f.flags(), 0);
    EXPECT_EQ(f.topic_id(), 0);
    EXPECT_EQ(f.seq(), 0u);
    EXPECT_EQ(f.timestamp_ns(), 0u);
    EXPECT_EQ(f.sideband_idx(), kSidebandIdxNone);
    EXPECT_EQ(f.sideband_len(), 0u);
    EXPECT_EQ(f.sideband_seq(), 0u);
    EXPECT(f.payload() == std::string(""));

    for (size_t i = kRouterPayloadOffset; i < kRouterFrameSize; ++i) {
        EXPECT_EQ(f.bytes[i], 0);
    }
}

void test_source_field() {
    RouterFrame f;
    f.init(0);
    f.set_source(0xAA);
    EXPECT_EQ(f.bytes[0], 0xAA);
    EXPECT_EQ(f.source(), 0xAA);
}

void test_flags_field_and_helpers() {
    RouterFrame f;
    f.init(1);

    f.set_flags(kFlagHasSideband | kFlagKeyframe | kFlagIsAck | kFlagEos);
    EXPECT(f.has_sideband());
    EXPECT(f.is_keyframe());
    EXPECT(f.is_ack());
    EXPECT(f.is_eos());
    EXPECT_EQ(f.priority(), 0u);

    f.set_flags(static_cast<uint8_t>(5u << kFlagPriorityShift));
    EXPECT_EQ(f.priority(), 5u);
    EXPECT(!f.has_sideband());
    EXPECT(!f.is_keyframe());

    f.set_flags(0xFF & ~kFlagReservedMask);
    EXPECT_EQ(f.priority(), 7u);
    EXPECT(f.has_sideband() && f.is_keyframe() && f.is_ack() && f.is_eos());
}

void test_topic_id_field_layout() {
    RouterFrame f;
    f.init(1);
    f.set_topic_id(0x1234);
    EXPECT_EQ(f.topic_id(), 0x1234);
    EXPECT_EQ(f.bytes[kRouterTopicIdOffset + 0], 0x34);  // little-endian low byte
    EXPECT_EQ(f.bytes[kRouterTopicIdOffset + 1], 0x12);
}

void test_seq_field_layout_and_wrap() {
    RouterFrame f;
    f.init(1);

    f.set_seq(0xDEAD'BEEFu);
    EXPECT_EQ(f.seq(), 0xDEAD'BEEFu);
    EXPECT_EQ(f.bytes[kRouterSeqOffset + 0], 0xEF);
    EXPECT_EQ(f.bytes[kRouterSeqOffset + 1], 0xBE);
    EXPECT_EQ(f.bytes[kRouterSeqOffset + 2], 0xAD);
    EXPECT_EQ(f.bytes[kRouterSeqOffset + 3], 0xDE);

    f.set_seq(0xFFFF'FFFFu);
    EXPECT_EQ(f.seq(), 0xFFFF'FFFFu);
    f.set_seq(0u);
    EXPECT_EQ(f.seq(), 0u);

    const uint32_t a = 0xFFFF'FFF0u;
    const uint32_t b = 0x0000'0010u;
    const uint32_t diff = b - a;
    EXPECT_EQ(diff, 32u);
}

void test_timestamp_field_layout_round_trip() {
    RouterFrame f;
    f.init(1);

    f.set_timestamp_ns(0x0123'4567'89AB'CDEFull);
    EXPECT_EQ(f.timestamp_ns(), 0x0123'4567'89AB'CDEFull);
    EXPECT_EQ(f.bytes[kRouterTimestampOffset + 0], 0xEF);
    EXPECT_EQ(f.bytes[kRouterTimestampOffset + 7], 0x01);

    f.set_timestamp_ns(0);
    EXPECT_EQ(f.timestamp_ns(), 0u);
    f.set_timestamp_ns(UINT64_MAX);
    EXPECT_EQ(f.timestamp_ns(), UINT64_MAX);
}

void test_sideband_idx_field() {
    RouterFrame f;
    f.init(1);
    EXPECT_EQ(f.sideband_idx(), kSidebandIdxNone);  // set by init()

    f.set_sideband_idx(0);
    EXPECT_EQ(f.sideband_idx(), 0);
    f.set_sideband_idx(7);
    EXPECT_EQ(f.sideband_idx(), 7);
    f.set_sideband_idx(0xFEED);
    EXPECT_EQ(f.sideband_idx(), 0xFEED);

    EXPECT_EQ(f.bytes[kRouterSidebandIdxOffset + 0], 0xED);
    EXPECT_EQ(f.bytes[kRouterSidebandIdxOffset + 1], 0xFE);
}

void test_sideband_len_uint48_truncation() {
    RouterFrame f;
    f.init(1);
    EXPECT_EQ(f.sideband_len(), 0u);

    f.set_sideband_len(1024);
    EXPECT_EQ(f.sideband_len(), 1024u);

    const uint64_t max48 = (1ull << 48) - 1;
    f.set_sideband_len(max48);
    EXPECT_EQ(f.sideband_len(), max48);

    f.set_sideband_len(max48 + 1);
    EXPECT_EQ(f.sideband_len(), 0u);

    const uint64_t with_high_bits = (max48 | (0xAAull << 48));
    f.set_sideband_len(with_high_bits);
    EXPECT_EQ(f.sideband_len(), max48);

    f.set_sideband_len(0x0123'4567'89ABull);
    EXPECT_EQ(f.sideband_len(), 0x0123'4567'89ABull);
    EXPECT_EQ(f.bytes[kRouterSidebandLenOffset + 0], 0xAB);
    EXPECT_EQ(f.bytes[kRouterSidebandLenOffset + 1], 0x89);
    EXPECT_EQ(f.bytes[kRouterSidebandLenOffset + 2], 0x67);
    EXPECT_EQ(f.bytes[kRouterSidebandLenOffset + 3], 0x45);
    EXPECT_EQ(f.bytes[kRouterSidebandLenOffset + 4], 0x23);
    EXPECT_EQ(f.bytes[kRouterSidebandLenOffset + 5], 0x01);
}

void test_sideband_len_does_not_clobber_neighbors() {
    RouterFrame f;
    f.init(1);
    f.set_sideband_idx(0xBEEF);
    f.set_sideband_seq(0xCAFEBABE'DEADBEEFull);

    f.set_sideband_len((1ull << 48) - 1);

    EXPECT_EQ(f.sideband_idx(), 0xBEEF);
    EXPECT_EQ(f.sideband_seq(), 0xCAFEBABE'DEADBEEFull);
    EXPECT_EQ(f.sideband_len(), (1ull << 48) - 1);
}

void test_sideband_seq_field() {
    RouterFrame f;
    f.init(1);
    f.set_sideband_seq(0x0123'4567'89AB'CDEFull);
    EXPECT_EQ(f.sideband_seq(), 0x0123'4567'89AB'CDEFull);
    EXPECT_EQ(f.bytes[kRouterSidebandSeqOffset + 0], 0xEF);
    EXPECT_EQ(f.bytes[kRouterSidebandSeqOffset + 7], 0x01);
}

void test_payload_basic_round_trip() {
    RouterFrame f;
    f.init(1);
    f.set_payload(std::string_view("hello"));
    EXPECT(f.payload() == std::string("hello"));

    f.set_payload(std::string_view(""));
    EXPECT(f.payload() == std::string(""));
}

void test_payload_fills_32_bytes() {
    RouterFrame f;
    f.init(1);
    const std::string big(kRouterPayloadSize, 'x');
    f.set_payload(big);
    EXPECT(f.payload() == big);

    for (size_t i = 0; i < kRouterPayloadSize; ++i) {
        EXPECT_EQ(f.bytes[kRouterPayloadOffset + i], 'x');
    }
}

void test_payload_throws_on_overflow() {
    RouterFrame f;
    f.init(1);
    const std::string too_big(kRouterPayloadSize + 1, 'x');
    bool threw = false;
    try {
        f.set_payload(too_big);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    EXPECT(threw);
}

void test_payload_does_not_touch_header() {
    RouterFrame f;
    f.init(7);
    f.set_topic_id(0xABCD);
    f.set_seq(0x1234'5678u);
    f.set_timestamp_ns(0x0BAD'F00Du);
    f.set_sideband_idx(0x4242);
    f.set_sideband_len(0xDEAD'BEEFull);
    f.set_sideband_seq(0xC0DE'CAFEull);
    f.set_flags(kFlagKeyframe | kFlagHasSideband);

    f.set_payload(std::string(kRouterPayloadSize, 'Z'));

    EXPECT_EQ(f.source(), 7);
    EXPECT_EQ(f.topic_id(), 0xABCD);
    EXPECT_EQ(f.seq(), 0x1234'5678u);
    EXPECT_EQ(f.timestamp_ns(), 0x0BAD'F00Du);
    EXPECT_EQ(f.sideband_idx(), 0x4242);
    EXPECT_EQ(f.sideband_len(), 0xDEAD'BEEFull);
    EXPECT_EQ(f.sideband_seq(), 0xC0DE'CAFEull);
    EXPECT_EQ(f.flags(), kFlagKeyframe | kFlagHasSideband);
}

void test_full_roundtrip_one_frame() {
    RouterFrame a;
    a.init(11);
    a.set_flags(kFlagHasSideband | kFlagKeyframe
                | static_cast<uint8_t>(3u << kFlagPriorityShift));
    a.set_topic_id(0x0BAD);
    a.set_seq(0xCAFEBABEu);
    a.set_timestamp_ns(0xFEEDFACE'DEADBEEFull);
    a.set_sideband_idx(2);
    a.set_sideband_len(8 * 1024 * 1024);
    a.set_sideband_seq(7);
    a.set_payload(std::string_view("control plane"));

    RouterFrame b;
    std::memcpy(b.bytes, a.bytes, kRouterFrameSize);

    EXPECT_EQ(b.source(), 11);
    EXPECT(b.has_sideband() && b.is_keyframe() && !b.is_ack() && !b.is_eos());
    EXPECT_EQ(b.priority(), 3u);
    EXPECT_EQ(b.topic_id(), 0x0BAD);
    EXPECT_EQ(b.seq(), 0xCAFEBABEu);
    EXPECT_EQ(b.timestamp_ns(), 0xFEEDFACE'DEADBEEFull);
    EXPECT_EQ(b.sideband_idx(), 2);
    EXPECT_EQ(b.sideband_len(), static_cast<uint64_t>(8 * 1024 * 1024));
    EXPECT_EQ(b.sideband_seq(), 7u);
    EXPECT(b.payload() == std::string("control plane"));
}

}  // namespace

int main() {
    test_version_and_size_constants();
    test_init_zeroes_everything_and_stamps_source();
    test_source_field();
    test_flags_field_and_helpers();
    test_topic_id_field_layout();
    test_seq_field_layout_and_wrap();
    test_timestamp_field_layout_round_trip();
    test_sideband_idx_field();
    test_sideband_len_uint48_truncation();
    test_sideband_len_does_not_clobber_neighbors();
    test_sideband_seq_field();
    test_payload_basic_round_trip();
    test_payload_fills_32_bytes();
    test_payload_throws_on_overflow();
    test_payload_does_not_touch_header();
    test_full_roundtrip_one_frame();

    std::cout << "frame_test: "
              << (assertions_run - assertions_failed) << '/' << assertions_run
              << " assertions passed\n";

    return assertions_failed == 0 ? 0 : 1;
}
