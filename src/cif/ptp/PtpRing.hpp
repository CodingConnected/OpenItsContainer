#pragma once

// --- Standard Library Includes ---
#include <atomic>
#include <cstddef>
#include <vector>

// --- Namespace Declaration ---
namespace cif::ptp {

/**
 * @brief Single-producer / single-consumer lock-free byte ring buffer.
 *
 * One thread pushes (the producer), one thread pops (the consumer); no other
 * synchronisation is required between them. In the PTP transport each link owns
 * two of these:
 *   - IBER: the PTP io thread pushes bytes read from the socket; the control
 *     thread (inside applicatieprogramma) pops them via lees_char_COM_IBER.
 *   - UBER: the control thread pushes bytes via schrijf_char_COM_UBER; the io
 *     thread pops them to write to the socket.
 * Because each ring has exactly one producer and one consumer, the head/tail
 * indices need only acquire/release ordering — no mutex.
 *
 * One slot is reserved to distinguish full from empty, so a ring constructed
 * with capacity N holds up to N bytes and allocates N+1 slots.
 *
 * @warning @ref reset is NOT safe against concurrent push/pop; it is only for
 *          link (re)initialisation while no io thread is touching the ring
 *          (init_COM_IBER / init_COM_UBER).
 */
class PtpRing {
public:
    /**
     * @brief Construct a ring holding up to @p capacity bytes.
     * @param capacity Usable byte capacity; raised to @ref MIN_RING_BYTES-worth
     *        of storage by the transport, but any positive value works here.
     */
    explicit PtpRing(std::size_t capacity)
        : slots_(capacity + 1)
        , buf_(capacity + 1) {}

    /// @brief Usable capacity in bytes (one fewer than the slot count).
    std::size_t capacity() const noexcept { return slots_ - 1; }

    /**
     * @brief Push one byte (producer side).
     * @return true if stored; false if the ring is full (byte dropped).
     */
    bool push(char c) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = advance(head);
        if (next == tail_.load(std::memory_order_acquire)) {
            return false; // full
        }
        buf_[head] = c;
        head_.store(next, std::memory_order_release);
        return true;
    }

    /**
     * @brief Pop one byte (consumer side).
     * @param[out] out Receives the byte if one was available.
     * @return true if a byte was popped; false if the ring is empty.
     */
    bool pop(char& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false; // empty
        }
        out = buf_[tail];
        tail_.store(advance(tail), std::memory_order_release);
        return true;
    }

    /// @brief True if no byte is currently available to pop.
    bool empty() const noexcept {
        return tail_.load(std::memory_order_acquire) ==
               head_.load(std::memory_order_acquire);
    }

    /**
     * @brief Discard all buffered bytes.
     * @warning Not thread-safe against concurrent push/pop; use only at link
     *          (re)initialisation.
     */
    void reset() noexcept {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

private:
    std::size_t advance(std::size_t i) const noexcept {
        return (i + 1 == slots_) ? 0 : i + 1;
    }

    std::size_t slots_;                 ///< Allocated slot count (capacity + 1).
    std::vector<char> buf_;             ///< Backing storage.
    std::atomic<std::size_t> head_{0};  ///< Next write index (producer owns).
    std::atomic<std::size_t> tail_{0};  ///< Next read index (consumer owns).
};

} // namespace cif::ptp
