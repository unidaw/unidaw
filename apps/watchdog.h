#pragma once

#include <cstdint>
#include <functional>
#include <iostream>

#include "apps/shared_memory.h"

namespace daw {

// THE EVICTION BOUND, AUTHORED. AE-P1.2 G3 ruling R3 pins this at 3; the value is AUTHORED, not
// derived, and the packet says so — nothing in the tree sourced the previous 500, which appeared as
// a bare literal at three construction sites with no unit anywhere.
//
// THE UNIT IS OBSERVATIONS, NOT MILLISECONDS. It counts consecutive calls to Watchdog::check() that
// found the host late. A reader of `Watchdog(mailbox, 500, cb)` had no way to tell whether 500 was
// a duration, a block count or a sample count — which is why this carries its unit in its name
// rather than in a comment beside one of the three call sites.
//
// One definition, three users. tools/watchdog_bound_check.sh fails if a construction passes an
// integer literal instead of this constant, or if this value drifts from the authored 3 without the
// ruling moving with it.
inline constexpr uint32_t kHostLateObservationsBeforeEviction = 3;

enum class FaultType {
  None,
  TransientLate,  // Simulate a single missed deadline
  HardHang        // Simulate a persistent hang
};

class Watchdog {
 public:
  using RestartCallback = std::function<void()>;

  Watchdog(const BlockMailbox* mailbox,
           uint32_t hardTimeoutBlocks,
           RestartCallback onRestart)
      : mailbox_(mailbox),
        hardTimeoutBlocks_(hardTimeoutBlocks),
        onRestart_(std::move(onRestart)) {}

  void injectFault(FaultType type) {
    fault_ = type;
  }

  // Returns true if the block is ready (or valid), false if late (silence required).
  bool check(uint32_t expectedBlockId) {
    if (!mailbox_) {
      return false;
    }

    // handle injected faults
    if (fault_ == FaultType::HardHang) {
        // Force failure logic below
    } else if (fault_ == FaultType::TransientLate) {
        fault_ = FaultType::None; // One-shot
        return false; // Return late immediately
    }

    const uint32_t completed =
        mailbox_->completedBlockId.load(std::memory_order_acquire);

    // If we have a HardHang fault, we ignore the actual completed ID
    bool isLate = (fault_ == FaultType::HardHang) || (completed < expectedBlockId);

    if (!isLate) {
      consecutiveLateBlocks_ = 0;
      return true;
    }

    // Host is late (or forced late).
    consecutiveLateBlocks_++;
    
    if (consecutiveLateBlocks_ >= hardTimeoutBlocks_) {
      std::cerr << "Watchdog: Timeout! (Fault=" << (int)fault_ << ") Triggering restart." << std::endl;
      if (onRestart_) {
        onRestart_();
        reset(); // Clear state after restart trigger
      }
    }

    return false;
  }

  void reset() {
    consecutiveLateBlocks_ = 0;
    fault_ = FaultType::None;
  }

 private:
  const BlockMailbox* mailbox_;
  uint32_t hardTimeoutBlocks_;
  RestartCallback onRestart_;
  uint32_t consecutiveLateBlocks_ = 0;
  FaultType fault_ = FaultType::None;
};

}  // namespace daw
