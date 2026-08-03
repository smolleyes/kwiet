#pragma once

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

// Single-producer / single-consumer lock-free ring of floats.
//
// Exactly one thread may call Push/PushZeros and exactly one (different)
// thread may call Pop. Init/Free are not concurrent with either and must run
// off the real-time path: they are the only members that allocate.
//
// Indices increase monotonically and are masked on access, so a full ring is
// distinguishable from an empty one without wasting a slot. size_t wraparound
// is well defined for unsigned types and the differences stay correct.
class SpscRing final
{
public:
    SpscRing() = default;

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    ~SpscRing()
    {
        Free();
    }

    // capacity is rounded up to a power of two. Non-RT: allocates.
    bool Init(size_t capacity)
    {
        Free();
        size_t pow2 = 1;
        while (pow2 < capacity) {
            pow2 <<= 1;
            if (pow2 == 0) {
                return false; // overflow
            }
        }
        m_data = static_cast<float*>(calloc(pow2, sizeof(float)));
        if (m_data == nullptr) {
            return false;
        }
        m_mask = pow2 - 1;
        m_write.store(0, std::memory_order_relaxed);
        m_read.store(0, std::memory_order_relaxed);
        return true;
    }

    // Non-RT: frees.
    void Free()
    {
        if (m_data != nullptr) {
            free(m_data);
            m_data = nullptr;
        }
        m_mask = 0;
        m_write.store(0, std::memory_order_relaxed);
        m_read.store(0, std::memory_order_relaxed);
    }

    size_t Capacity() const
    {
        return m_data != nullptr ? m_mask + 1 : 0;
    }

    // Readable sample count. Safe from either side.
    size_t Available() const
    {
        const size_t w = m_write.load(std::memory_order_acquire);
        const size_t r = m_read.load(std::memory_order_acquire);
        return w - r;
    }

    // Writable sample count. Producer side.
    size_t Space() const
    {
        return Capacity() - Available();
    }

    // Producer. All-or-nothing: returns false and writes nothing if full.
    bool Push(const float* src, size_t count)
    {
        if (m_data == nullptr || src == nullptr || count > Space()) {
            return false;
        }
        const size_t w = m_write.load(std::memory_order_relaxed);
        const size_t first = Min(count, Capacity() - (w & m_mask));
        memcpy(m_data + (w & m_mask), src, first * sizeof(float));
        if (count > first) {
            memcpy(m_data, src + first, (count - first) * sizeof(float));
        }
        // Release: the samples above must be visible before the new index.
        m_write.store(w + count, std::memory_order_release);
        return true;
    }

    // Producer. Used to prime the output ring with the fixed latency.
    bool PushZeros(size_t count)
    {
        if (m_data == nullptr || count > Space()) {
            return false;
        }
        const size_t w = m_write.load(std::memory_order_relaxed);
        const size_t first = Min(count, Capacity() - (w & m_mask));
        memset(m_data + (w & m_mask), 0, first * sizeof(float));
        if (count > first) {
            memset(m_data, 0, (count - first) * sizeof(float));
        }
        m_write.store(w + count, std::memory_order_release);
        return true;
    }

    // Consumer. All-or-nothing: returns false and reads nothing if short.
    bool Pop(float* dst, size_t count)
    {
        if (m_data == nullptr || dst == nullptr || count > Available()) {
            return false;
        }
        const size_t r = m_read.load(std::memory_order_relaxed);
        const size_t first = Min(count, Capacity() - (r & m_mask));
        memcpy(dst, m_data + (r & m_mask), first * sizeof(float));
        if (count > first) {
            memcpy(dst + first, m_data, (count - first) * sizeof(float));
        }
        // Release: frees the slots for the producer.
        m_read.store(r + count, std::memory_order_release);
        return true;
    }

private:
    static size_t Min(size_t a, size_t b)
    {
        return a < b ? a : b;
    }

    float* m_data = nullptr;
    size_t m_mask = 0;

    // Kept on separate cache lines: they are written from different threads and
    // sharing a line would cost a coherency round trip on every quantum.
    // The padding MSVC warns about (C4324) is exactly what is being asked for.
#pragma warning(push)
#pragma warning(disable : 4324)
    alignas(64) std::atomic<size_t> m_write{ 0 };
    alignas(64) std::atomic<size_t> m_read{ 0 };
#pragma warning(pop)
};
