#pragma once

#include "router/frame.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

struct RouteTargets {
    std::array<uint8_t, 8> ids{};
    size_t count = 0;

    const uint8_t* begin() const { return ids.data(); }
    const uint8_t* end() const { return ids.data() + count; }
};

struct RouteRule {
    uint8_t source;
    uint8_t dest0;
    uint8_t dest1;
};

inline RouteTargets route_targets_for(
    const RouteRule* rules,
    size_t rule_count,
    uint8_t source) {
    for (size_t i = 0; i < rule_count; ++i) {
        if (rules[i].source != source) {
            continue;
        }
        if (rules[i].dest1 != 0) {
            return RouteTargets{{{rules[i].dest0, rules[i].dest1}}, 2};
        }
        return RouteTargets{{{rules[i].dest0}}, 1};
    }
    return {};
}

struct ForwardResult {
    uint8_t source = kEndpointInvalid;
    RouteTargets targets{};

    explicit operator bool() const { return source != kEndpointInvalid; }
};
