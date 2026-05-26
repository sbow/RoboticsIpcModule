#pragma once

#include "buffer.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct ShmRegion;

struct ShmSpsc {
    using Handle = std::unique_ptr<ShmRegion>;

    struct BindParams {
        const char* name;
        bool create = false;
        size_t slot_count = 256;
        size_t max_payload = 1024;
    };

    struct SendParams {
        Buffer payload;
    };

    struct RecvResult {};

    static Handle open() { return nullptr; }

    static void bind(Handle& handle, const BindParams& params);

    static bool try_recv(const Handle& handle, Buffer& buf, RecvResult& out);

    static void recv(const Handle& handle, Buffer& buf, RecvResult& out);

    static void send(const Handle& handle, const SendParams& params, const Buffer& payload);

    static void connect_or_send(const Handle& handle, const SendParams& params) {
        send(handle, params, params.payload);
    }

    static void echo(const Handle& handle, const RecvResult& recv, const Buffer& payload) {
        (void)recv;
        SendParams params{.payload = payload};
        send(handle, params, payload);
    }

    static void recv_from(const Handle& handle, Buffer& buf, RecvResult& out) {
        recv(handle, buf, out);
    }
};

struct ShmRegion {
    bool is_creator = false;
    std::string name;
    size_t slot_count = 0;
    size_t max_payload = 0;
    int fd = -1;
    void* mapping = nullptr;
    size_t mapping_size = 0;

    std::atomic<uint64_t>* req_head = nullptr;
    std::atomic<uint64_t>* req_tail = nullptr;
    std::atomic<uint64_t>* rep_head = nullptr;
    std::atomic<uint64_t>* rep_tail = nullptr;
    uint8_t* req_slots = nullptr;
    uint8_t* rep_slots = nullptr;
    size_t slot_stride = 0;

    bool is_server_role = false;

    ~ShmRegion();
};

inline size_t shm_slot_stride(size_t max_payload) {
    return sizeof(std::atomic<uint32_t>) + max_payload;
}

inline size_t shm_region_size(size_t slot_count, size_t max_payload) {
    const size_t stride = shm_slot_stride(max_payload);
    return 64 + 2 * slot_count * stride;
}

inline void shm_init_control(ShmRegion& region) {
    region.req_head->store(0, std::memory_order_relaxed);
    region.req_tail->store(0, std::memory_order_relaxed);
    region.rep_head->store(0, std::memory_order_relaxed);
    region.rep_tail->store(0, std::memory_order_relaxed);
}

inline void shm_map_layout(ShmRegion& region) {
    auto* base = static_cast<uint8_t*>(region.mapping);
    region.req_head = reinterpret_cast<std::atomic<uint64_t>*>(base);
    region.req_tail = region.req_head + 1;
    region.rep_head = region.req_head + 2;
    region.rep_tail = region.req_head + 3;
    region.slot_stride = shm_slot_stride(region.max_payload);
    region.req_slots = base + 64;
    region.rep_slots = region.req_slots + region.slot_count * region.slot_stride;
}

inline void ShmSpsc::bind(Handle& handle, const BindParams& params) {
    handle = std::make_unique<ShmRegion>();
    ShmRegion& region = *handle;
    region.name = params.name;
    region.slot_count = params.slot_count;
    region.max_payload = params.max_payload;
    region.is_creator = params.create;
    region.is_server_role = params.create;

    const size_t size = shm_region_size(region.slot_count, region.max_payload);
    region.mapping_size = size;

    const int flags = params.create ? (O_CREAT | O_RDWR) : O_RDWR;
    region.fd = shm_open(params.name, flags, 0666);
    if (region.fd < 0) {
        throw std::runtime_error("shm_open failed");
    }

    if (params.create) {
        if (ftruncate(region.fd, static_cast<off_t>(size)) < 0) {
            throw std::runtime_error("ftruncate failed");
        }
    }

    region.mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, region.fd, 0);
    if (region.mapping == MAP_FAILED) {
        throw std::runtime_error("mmap failed");
    }

    shm_map_layout(region);
    if (params.create) {
        std::memset(region.mapping, 0, size);
        shm_init_control(region);
    }
}

inline ShmRegion::~ShmRegion() {
    if (mapping != nullptr && mapping != MAP_FAILED) {
        munmap(mapping, mapping_size);
    }
    if (fd >= 0) {
        close(fd);
    }
    if (is_creator) {
        shm_unlink(name.c_str());
    }
}

inline bool shm_push_slot(
    std::atomic<uint64_t>& head,
    std::atomic<uint64_t>& tail,
    uint8_t* slots,
    size_t slot_count,
    size_t slot_stride,
    size_t max_payload,
    const Buffer& payload) {
    while (true) {
        const uint64_t h = head.load(std::memory_order_relaxed);
        const uint64_t t = tail.load(std::memory_order_acquire);
        if (h - t >= slot_count) {
            continue;
        }

        const size_t index = static_cast<size_t>(h % slot_count);
        uint8_t* slot = slots + index * slot_stride;
        auto* len = reinterpret_cast<std::atomic<uint32_t>*>(slot);
        const size_t n = payload.size < max_payload ? payload.size : max_payload;
        std::memcpy(slot + sizeof(std::atomic<uint32_t>), payload.data, n);
        len->store(static_cast<uint32_t>(n), std::memory_order_relaxed);
        head.store(h + 1, std::memory_order_release);
        return true;
    }
}

inline bool shm_try_pop_slot(
    std::atomic<uint64_t>& head,
    std::atomic<uint64_t>& tail,
    uint8_t* slots,
    size_t slot_count,
    size_t slot_stride,
    size_t max_payload,
    Buffer& buf) {
    const uint64_t t = tail.load(std::memory_order_relaxed);
    const uint64_t h = head.load(std::memory_order_acquire);
    if (t >= h) {
        return false;
    }

    const size_t index = static_cast<size_t>(t % slot_count);
    const uint8_t* slot = slots + index * slot_stride;
    const auto* len = reinterpret_cast<const std::atomic<uint32_t>*>(slot);
    const uint32_t n = len->load(std::memory_order_acquire);
    const size_t copy = n < max_payload ? n : max_payload;
    if (copy > buf.capacity) {
        throw std::runtime_error("shm recv buffer too small");
    }
    std::memcpy(buf.data, slot + sizeof(std::atomic<uint32_t>), copy);
    buf.size = copy;
    tail.store(t + 1, std::memory_order_release);
    return true;
}

inline bool shm_pop_slot(
    std::atomic<uint64_t>& head,
    std::atomic<uint64_t>& tail,
    uint8_t* slots,
    size_t slot_count,
    size_t slot_stride,
    size_t max_payload,
    Buffer& buf) {
    while (!shm_try_pop_slot(head, tail, slots, slot_count, slot_stride, max_payload, buf)) {
    }
    return true;
}

inline void ShmSpsc::send(const Handle& handle, const SendParams& params, const Buffer& payload) {
    if (!handle) {
        throw std::runtime_error("shm handle not bound");
    }
    ShmRegion& region = *handle;
    if (region.is_server_role) {
        shm_push_slot(*region.rep_head, *region.rep_tail, region.rep_slots,
            region.slot_count, region.slot_stride, region.max_payload, payload);
    } else {
        shm_push_slot(*region.req_head, *region.req_tail, region.req_slots,
            region.slot_count, region.slot_stride, region.max_payload, payload);
    }
    (void)params;
}

inline bool ShmSpsc::try_recv(const Handle& handle, Buffer& buf, RecvResult& out) {
    (void)out;
    if (!handle) {
        throw std::runtime_error("shm handle not bound");
    }
    ShmRegion& region = *handle;
    if (region.is_server_role) {
        return shm_try_pop_slot(*region.req_head, *region.req_tail, region.req_slots,
            region.slot_count, region.slot_stride, region.max_payload, buf);
    }
    return shm_try_pop_slot(*region.rep_head, *region.rep_tail, region.rep_slots,
        region.slot_count, region.slot_stride, region.max_payload, buf);
}

inline void ShmSpsc::recv(const Handle& handle, Buffer& buf, RecvResult& out) {
    while (!try_recv(handle, buf, out)) {
    }
}
