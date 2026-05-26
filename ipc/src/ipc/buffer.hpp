#pragma once

#include <cstddef>

// Non-owning view over caller storage (stack vector, etc.). Does not allocate.
struct Buffer {
    void* data;
    size_t size;
    size_t capacity;

    static Buffer writable(void* data, size_t capacity) {
        return Buffer{data, 0, capacity};
    }

    static Buffer read_only(const void* data, size_t size) {
        return Buffer{const_cast<void*>(data), size, size};
    }

private:
    Buffer(void* data_, size_t size_, size_t capacity_)
        : data(data_), size(size_), capacity(capacity_) {}
};
