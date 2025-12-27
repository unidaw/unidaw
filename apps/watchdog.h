#pragma once

#include <cstdint>
#include <functional>
#include <iostream>

#include "apps/shared_memory.h"

namespace daw {

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
