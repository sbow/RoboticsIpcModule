#pragma once

#include "ipc/buffer.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

// Wire format version of RouterFrame.
//
// Layout v1 (frozen by ADR 0004):
//   byte  0      source peer id            (1 B)
//   bytes 1..9   monotonic timestamp ns    (9 B, big-endian)
//   bytes 10..31 payload                   (22 B, zero-padded)
//
// Any change that alters frame size, field offsets, or interpretation MUST
// bump this constant and ship a migration ADR (see docs/adr/0004).
constexpr uint8_t kRouterFrameVersion = 1;

constexpr size_t kRouterFrameSize = 32;
constexpr size_t kRouterTimestampOffset = 1;
constexpr size_t kRouterTimestampSize = 9;
constexpr size_t kRouterPayloadOffset = 10;
constexpr size_t kRouterPayloadSize = 22;

constexpr uint8_t kEndpointInvalid = 0;
constexpr uint8_t kEndpointServer = 255;

struct RouterFrame {
    uint8_t bytes[kRouterFrameSize]{};

    void init(uint8_t source_id) {
        std::memset(bytes, 0, kRouterFrameSize);
        set_source(source_id);
    }

    uint8_t source() const { return bytes[0]; }

    void set_source(uint8_t id) { bytes[0] = id; }

    void set_timestamp_ns(uint64_t ns) {
        for (int i = 8; i >= 0; --i) {
            bytes[kRouterTimestampOffset + static_cast<size_t>(i)] =
                static_cast<uint8_t>(ns & 0xff);
            ns >>= 8;
        }
    }

    uint64_t timestamp_ns() const {
        uint64_t ns = 0;
        for (size_t i = 0; i < kRouterTimestampSize; ++i) {
            ns = (ns << 8) | bytes[kRouterTimestampOffset + i];
        }
        return ns;
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
            + " ts=" + std::to_string(timestamp_ns())
            + " payload=" + payload();
    }

    Buffer writable() { return Buffer::writable(bytes, kRouterFrameSize); }

    Buffer read_only() const { return Buffer::read_only(bytes, kRouterFrameSize); }
};
