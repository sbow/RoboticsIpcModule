#pragma once

#include "ipc/buffer.hpp"

#include <bit>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

// Wire format version of RouterFrame.
//
// Layout v2 (ADR 0008, supersedes the v1 block in ADR 0004):
//   offset  size  field           type    notes
//   ------  ----  -------------   ------  -----------------------------------------
//    0       1   source          u8      router-stamped on forward
//    1       1   flags           u8      bit0=has_sideband, bit1=keyframe,
//                                         bit2=is_ack, bit3=eos,
//                                         bits4..6=priority (0..7), bit7=reserved
//    2       2   topic_id        u16     publisher-set; subscribers dispatch on this
//    4       4   seq             u32     per-source monotonic; wraps
//    8       8   timestamp_ns    u64     monotonic ns, host (little-endian) order
//   16       2   sideband_idx    u16     index into source peer's sideband table;
//                                         kSidebandIdxNone (0xFFFF) = no sideband
//   18       6   sideband_len    u48     LE 6 bytes; cap 256 TB
//   24       8   sideband_seq    u64     slot index / sequence in sideband region
//   32      32   payload         u8[32]  inline scratchpad; zero-padded
//
// All multi-byte integers are host (little-endian) byte order. The
// static_assert below pins the supported endianness; porting to a
// big-endian target requires a new ADR + explicit byte-swap layer.
//
// Any change that alters frame size, field offsets, or interpretation
// MUST bump kRouterFrameVersion and ship a migration ADR (see ADR 0004).
constexpr uint8_t kRouterFrameVersion = 2;

constexpr size_t kRouterFrameSize = 64;

constexpr size_t kRouterSourceOffset       = 0;
constexpr size_t kRouterFlagsOffset        = 1;
constexpr size_t kRouterTopicIdOffset      = 2;
constexpr size_t kRouterSeqOffset          = 4;
constexpr size_t kRouterTimestampOffset    = 8;
constexpr size_t kRouterTimestampSize      = 8;
constexpr size_t kRouterSidebandIdxOffset  = 16;
constexpr size_t kRouterSidebandLenOffset  = 18;
constexpr size_t kRouterSidebandLenSize    = 6;
constexpr size_t kRouterSidebandSeqOffset  = 24;
constexpr size_t kRouterPayloadOffset      = 32;
constexpr size_t kRouterPayloadSize        = 32;

constexpr uint16_t kSidebandIdxNone = 0xFFFF;
constexpr uint64_t kRouterSidebandLenMask = 0x0000'FFFF'FFFF'FFFFull;

constexpr uint8_t kFlagHasSideband = 1u << 0;
constexpr uint8_t kFlagKeyframe    = 1u << 1;
constexpr uint8_t kFlagIsAck       = 1u << 2;
constexpr uint8_t kFlagEos         = 1u << 3;
constexpr uint8_t kFlagPriorityMask  = 0b0111'0000u;
constexpr uint8_t kFlagPriorityShift = 4;
constexpr uint8_t kFlagReservedMask  = 0b1000'0000u;

constexpr uint8_t kEndpointInvalid = 0;
constexpr uint8_t kEndpointServer = 255;

static_assert(std::endian::native == std::endian::little,
    "RouterFrame v2 assumes little-endian host order (ADR 0008). "
    "Porting to a big-endian target requires a new ADR and an explicit "
    "byte-swap layer.");

struct RouterFrame {
    uint8_t bytes[kRouterFrameSize]{};

    void init(uint8_t source_id) {
        std::memset(bytes, 0, kRouterFrameSize);
        set_source(source_id);
        set_sideband_idx(kSidebandIdxNone);
    }

    uint8_t source() const { return bytes[kRouterSourceOffset]; }
    void set_source(uint8_t id) { bytes[kRouterSourceOffset] = id; }

    uint8_t flags() const { return bytes[kRouterFlagsOffset]; }
    void set_flags(uint8_t f) { bytes[kRouterFlagsOffset] = f; }

    uint16_t topic_id() const {
        uint16_t v = 0;
        std::memcpy(&v, bytes + kRouterTopicIdOffset, sizeof(v));
        return v;
    }
    void set_topic_id(uint16_t v) {
        std::memcpy(bytes + kRouterTopicIdOffset, &v, sizeof(v));
    }

    uint32_t seq() const {
        uint32_t v = 0;
        std::memcpy(&v, bytes + kRouterSeqOffset, sizeof(v));
        return v;
    }
    void set_seq(uint32_t v) {
        std::memcpy(bytes + kRouterSeqOffset, &v, sizeof(v));
    }

    uint64_t timestamp_ns() const {
        uint64_t v = 0;
        std::memcpy(&v, bytes + kRouterTimestampOffset, sizeof(v));
        return v;
    }
    void set_timestamp_ns(uint64_t v) {
        std::memcpy(bytes + kRouterTimestampOffset, &v, sizeof(v));
    }

    uint16_t sideband_idx() const {
        uint16_t v = 0;
        std::memcpy(&v, bytes + kRouterSidebandIdxOffset, sizeof(v));
        return v;
    }
    void set_sideband_idx(uint16_t v) {
        std::memcpy(bytes + kRouterSidebandIdxOffset, &v, sizeof(v));
    }

    uint64_t sideband_len() const {
        uint64_t v = 0;
        std::memcpy(&v, bytes + kRouterSidebandLenOffset, kRouterSidebandLenSize);
        return v & kRouterSidebandLenMask;
    }
    void set_sideband_len(uint64_t v) {
        const uint64_t masked = v & kRouterSidebandLenMask;
        std::memcpy(bytes + kRouterSidebandLenOffset, &masked, kRouterSidebandLenSize);
    }

    uint64_t sideband_seq() const {
        uint64_t v = 0;
        std::memcpy(&v, bytes + kRouterSidebandSeqOffset, sizeof(v));
        return v;
    }
    void set_sideband_seq(uint64_t v) {
        std::memcpy(bytes + kRouterSidebandSeqOffset, &v, sizeof(v));
    }

    bool has_sideband() const { return (flags() & kFlagHasSideband) != 0; }
    bool is_keyframe() const  { return (flags() & kFlagKeyframe) != 0; }
    bool is_ack() const       { return (flags() & kFlagIsAck) != 0; }
    bool is_eos() const       { return (flags() & kFlagEos) != 0; }
    uint8_t priority() const {
        return static_cast<uint8_t>(
            (flags() & kFlagPriorityMask) >> kFlagPriorityShift);
    }

    void set_payload(const void* data, size_t len) {
        if (len > kRouterPayloadSize) {
            throw std::runtime_error("router payload too large");
        }
        std::memset(bytes + kRouterPayloadOffset, 0, kRouterPayloadSize);
        if (len > 0) {
            std::memcpy(bytes + kRouterPayloadOffset, data, len);
        }
    }

    void set_payload(std::string_view payload) {
        set_payload(payload.data(), payload.size());
    }

    std::string payload() const {
        const char* begin = reinterpret_cast<const char*>(bytes + kRouterPayloadOffset);
        size_t len = kRouterPayloadSize;
        while (len > 0 && begin[len - 1] == '\0') {
            --len;
        }
        return std::string(begin, len);
    }

    std::string describe(const char* source_name) const {
        return "source=" + std::string(source_name)
            + " topic=" + std::to_string(topic_id())
            + " seq=" + std::to_string(seq())
            + " ts=" + std::to_string(timestamp_ns())
            + " payload=" + payload();
    }

    Buffer writable() { return Buffer::writable(bytes, kRouterFrameSize); }

    Buffer read_only() const { return Buffer::read_only(bytes, kRouterFrameSize); }
};

static_assert(sizeof(RouterFrame) == kRouterFrameSize,
    "RouterFrame must be exactly kRouterFrameSize bytes — ADR 0008");
