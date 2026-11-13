// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
namespace Common {
// A guest context can resume on another host thread; std::mutex ownership cannot follow it.
class FiberContextGuard {
public:
    bool try_lock() noexcept {
        return !held.test_and_set(std::memory_order_acquire);
    }
    void unlock() noexcept {
        held.clear(std::memory_order_release);
    }
    bool IsLocked() const noexcept {
        return held.test(std::memory_order_relaxed);
    }

private:
    std::atomic_flag held = ATOMIC_FLAG_INIT;
};
} // namespace Common
