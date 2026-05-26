#pragma once

// Sideband region descriptor — small POD type that names an out-of-band
// data channel (e.g. a shared-memory region holding NV12 camera frames
// or an ML inference tensor) carried *outside* the 32 B RouterFrame.
//
// Rationale + naming convention + lifecycle: docs/adr/0005-payload-policy-and-sideband.md
// (registered by Phase B of the robotics IPC module plan pack).
//
// This header is intentionally minimal:
//   * No open/create/close. Sidebands are owned by their producer peer
//     (typically a bridge or hardware adapter); the module is the *router*,
//     not the buffer pool.
//   * No transport coupling. A SidebandRegion may live in /dev/shm, on a
//     filesystem path, or behind any other named handle (DMA-BUF FDs, GPU
//     allocators, etc.). The router carries only the *descriptor* — peers
//     resolve it on their own terms.
//   * Header-only, no allocations.

#include "router/frame.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

// Discoverable in-region header that every sideband producer should write at
// offset 0 of the region. Consumers validate magic + version before reading
// past byte sizeof(SidebandHeader).
//
//   bytes 0..3   magic    'R','S','B','1' ('RouterSideBand v1')
//   bytes 4..7   version  (host-endian uint32, currently kSidebandVersion)
//   bytes 8..15  payload_bytes (uint64, host-endian) — bytes after the header
//
// 16 B total; sized so a SIMD-friendly aligned payload starts at offset 16.
struct SidebandHeader {
    char     magic[4];     // 'R' 'S' 'B' '1'
    uint32_t version;
    uint64_t payload_bytes;
};

static_assert(sizeof(SidebandHeader) == 16,
              "SidebandHeader must remain 16 B for ADR 0005 layout");

constexpr uint32_t kSidebandVersion = 1;

constexpr char kSidebandMagic0 = 'R';
constexpr char kSidebandMagic1 = 'S';
constexpr char kSidebandMagic2 = 'B';
constexpr char kSidebandMagic3 = '1';

// Per ADR 0005: sideband region names follow `/robot_<peer>_<class>`.
// kSidebandNameMaxLen mirrors the practical limit set by shm_open() on Linux
// (NAME_MAX-derived, ~250). Use this when validating loaded topology fields.
constexpr size_t kSidebandNameMaxLen = 250;

// "Data class" categories the system uses (mirrors SYSTEM-VISION.md peer
// catalog). Free-form strings are allowed in user code; these constants are
// the documented conventions.
constexpr const char* kSidebandClassVisionNv12  = "vision_nv12";
constexpr const char* kSidebandClassVisionJpeg  = "vision_jpeg";
constexpr const char* kSidebandClassMlInput     = "ml_tensor_in";
constexpr const char* kSidebandClassMlOutput    = "ml_tensor_out";
constexpr const char* kSidebandClassMavlinkBulk = "mavlink_bulk";

// Descriptor a router peer publishes / consumes. The 22 B RouterFrame payload
// MAY carry a small reference (frame_id, sequence) that points consumers at
// the right SidebandRegion. The descriptor itself is configuration / topology
// data, not in-band.
struct SidebandRegion {
    // Region name as it would appear to shm_open() ("/robot_vision_nv12") or
    // any other naming scheme the consumer agrees with the producer on.
    // Lifetime: borrowed; storage owned by topology / config loader.
    const char* name = nullptr;

    // Soft upper bound on payload bytes after the SidebandHeader. Producers
    // SHOULD respect it; consumers MUST treat any header.payload_bytes >
    // max_payload_bytes as malformed.
    size_t max_payload_bytes = 0;

    // Wire / layout version the producer commits to. Must match
    // kSidebandVersion for ADR-0005-compliant regions.
    uint32_t version = kSidebandVersion;
};

// Initialize the in-region header (producer-side helper). Caller is
// responsible for mapping the region and ensuring at least sizeof(SidebandHeader)
// bytes are writable at `header`.
inline void sideband_header_init(SidebandHeader& header, uint64_t payload_bytes) {
    header.magic[0] = kSidebandMagic0;
    header.magic[1] = kSidebandMagic1;
    header.magic[2] = kSidebandMagic2;
    header.magic[3] = kSidebandMagic3;
    header.version       = kSidebandVersion;
    header.payload_bytes = payload_bytes;
}

// Consumer-side validation. Returns true iff magic + version match and
// payload_bytes does not exceed the descriptor's declared maximum.
inline bool sideband_header_is_valid(const SidebandHeader& header,
                                     const SidebandRegion& region) {
    if (header.magic[0] != kSidebandMagic0
     || header.magic[1] != kSidebandMagic1
     || header.magic[2] != kSidebandMagic2
     || header.magic[3] != kSidebandMagic3) {
        return false;
    }
    if (header.version != region.version) {
        return false;
    }
    if (header.payload_bytes > region.max_payload_bytes) {
        return false;
    }
    return true;
}
