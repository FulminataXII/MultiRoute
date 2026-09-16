#pragma once
//==============================================================================
// AudioRingBuffer.h - Lock-free single-producer / single-consumer byte FIFO.
//
// Topology guarantee this relies on: exactly ONE capture thread writes and
// exactly ONE render thread reads a given instance. Fan-out to N outputs is
// achieved with N independent instances, not by sharing one.
//
// Algorithm unchanged from the validated version; only namespacing and
// documentation were added during the modular refactor.
//==============================================================================

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace mux {

// Padded to the typical destructive interference size so producer and consumer
// indices never share a cache line.
struct alignas(64) PaddedAtomic {
    std::atomic<size_t> val{0};
};

class AudioRingBuffer {
public:
    explicit AudioRingBuffer(size_t min_size) {
        if (min_size > (SIZE_MAX / 2) - 1)
            throw std::overflow_error("AudioRingBuffer: requested size too large");
        allocated_capacity_ = nextPowerOf2(min_size + 1);
        mask_ = allocated_capacity_ - 1;
        buffer_.resize(allocated_capacity_, 0);
    }

    AudioRingBuffer(const AudioRingBuffer&) = delete;
    AudioRingBuffer& operator=(const AudioRingBuffer&) = delete;

    // Producer side. Returns false and writes nothing if the payload does not
    // fit; partial writes would desynchronise frame boundaries.
    bool write(const uint8_t* data, size_t size) {
        const size_t h = head_.val.load(std::memory_order_acquire);
        const size_t t = tail_.val.load(std::memory_order_relaxed);

        const size_t available_write = (h - t - 1) & mask_;
        if (available_write < size) return false;

        const size_t first_part = std::min(size, allocated_capacity_ - t);
        std::memcpy(&buffer_[t], data, first_part);
        if (first_part < size)
            std::memcpy(&buffer_[0], data + first_part, size - first_part);

        tail_.val.store((t + size) & mask_, std::memory_order_release);
        return true;
    }

    bool write_silence(size_t size) {
        const size_t h = head_.val.load(std::memory_order_acquire);
        const size_t t = tail_.val.load(std::memory_order_relaxed);

        const size_t available_write = (h - t - 1) & mask_;
        if (available_write < size) return false;

        const size_t first_part = std::min(size, allocated_capacity_ - t);
        std::memset(&buffer_[t], 0, first_part);
        if (first_part < size)
            std::memset(&buffer_[0], 0, size - first_part);

        tail_.val.store((t + size) & mask_, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns bytes actually read (may be short, may be 0).
    size_t read(uint8_t* data, size_t size) {
        const size_t h = head_.val.load(std::memory_order_relaxed);
        const size_t t = tail_.val.load(std::memory_order_acquire);

        const size_t available_read = (t - h) & mask_;
        const size_t to_read = std::min(size, available_read);
        if (to_read == 0) return 0;

        const size_t first_part = std::min(to_read, allocated_capacity_ - h);
        std::memcpy(data, &buffer_[h], first_part);
        if (first_part < to_read)
            std::memcpy(data + first_part, &buffer_[0], to_read - first_part);

        head_.val.store((h + to_read) & mask_, std::memory_order_release);
        return to_read;
    }

    // Consumer side. Discards up to `size` bytes without copying them
    // anywhere. Same ownership rules as read(): ONLY the consumer thread may
    // call this, because it advances head_. This exists so a delay reduction
    // can drop already-buffered audio in place, rather than a third thread
    // reaching in and mutating the indices — which would break the SPSC
    // invariant this whole class depends on.
    // Returns the number of bytes actually discarded (may be less than asked
    // if the buffer holds less).
    size_t skip(size_t size) {
        const size_t h = head_.val.load(std::memory_order_relaxed);
        const size_t t = tail_.val.load(std::memory_order_acquire);

        const size_t available_read = (t - h) & mask_;
        const size_t to_skip = std::min(size, available_read);
        if (to_skip == 0) return 0;

        head_.val.store((h + to_skip) & mask_, std::memory_order_release);
        return to_skip;
    }

    size_t get_available_read() const {
        const size_t h = head_.val.load(std::memory_order_relaxed);
        const size_t t = tail_.val.load(std::memory_order_acquire);
        return (t - h) & mask_;
    }

    size_t get_capacity() const { return allocated_capacity_ - 1; }

    // Only safe while no producer or consumer thread is running.
    void reset_unsafe() {
        head_.val.store(0, std::memory_order_relaxed);
        tail_.val.store(0, std::memory_order_relaxed);
        std::fill(buffer_.begin(), buffer_.end(), uint8_t{0});
    }

private:
    static size_t nextPowerOf2(size_t n) {
        if (n <= 1) return 1;
        size_t p = 1;
        while (p < n) {
            if (p > (SIZE_MAX / 2)) throw std::overflow_error("AudioRingBuffer: size overflow");
            p <<= 1;
        }
        return p;
    }

    std::vector<uint8_t> buffer_;
    PaddedAtomic head_;  // consumer index
    PaddedAtomic tail_;  // producer index
    size_t allocated_capacity_ = 0;
    size_t mask_ = 0;
};

}  // namespace mux
