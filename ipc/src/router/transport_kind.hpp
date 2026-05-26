#pragma once

#include <cstring>

enum class TransportKind {
    Uds,
    Udp,
    Shm,
};

inline TransportKind transport_kind_from_cli(const char* arg) {
    if (std::strcmp(arg, "uds") == 0) {
        return TransportKind::Uds;
    }
    if (std::strcmp(arg, "udp") == 0) {
        return TransportKind::Udp;
    }
    if (std::strcmp(arg, "shm") == 0) {
        return TransportKind::Shm;
    }
    return TransportKind::Uds;
}

inline bool transport_kind_valid(const char* arg) {
    return std::strcmp(arg, "uds") == 0
        || std::strcmp(arg, "udp") == 0
        || std::strcmp(arg, "shm") == 0;
}

inline const char* transport_kind_name(TransportKind kind) {
    switch (kind) {
        case TransportKind::Uds:
            return "uds";
        case TransportKind::Udp:
            return "udp";
        case TransportKind::Shm:
            return "shm";
    }
    return "unknown";
}

inline bool transport_kind_is_datagram(TransportKind kind) {
    return kind == TransportKind::Uds || kind == TransportKind::Udp;
}
