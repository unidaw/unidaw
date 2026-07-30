#pragma once

// Thread scheduling helpers for the audio path.
//
// Context: the CoreAudio IO thread that runs the engine's audio callback is placed in
// the device's realtime workgroup automatically by CoreAudio, so it already gets
// deadline scheduling. But the two threads that actually FEED it are plain std::threads
// at default priority:
//   - the engine's producer (renders/dispatches each block ahead of the device), and
//   - the out-of-process host's control loop (which renders every plugin block inline).
// Under CPU contention the scheduler can preempt either for milliseconds, blowing the
// per-block deadline and starving the ring — the underruns the engine now measures.
//
// This raises those threads to the USER_INTERACTIVE QoS class so background and UI work
// no longer preempts them. It is a portable, low-risk step; it is NOT the same as true
// realtime (mach time-constraint) scheduling or workgroup membership, which is the
// deeper fix — but unlike a hard realtime policy it is safe on a thread that also does
// non-realtime work (plugin instantiation, state restore) because it never demotes a
// thread that overruns a computation budget.

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#include <mach/mach_init.h>
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#endif

namespace daw {

// FLUSH DENORMALS TO ZERO on the CALLING thread. Nothing else in this repo did this, and it is
// not a micro-optimisation: a filter or an envelope decaying toward zero enters the denormal
// range and STAYS there for as long as the voice lives, at roughly 100x the cost of normal
// arithmetic on x86. A sustained pad's tail can push a comfortable render into underruns for no
// audible reason at all, and the profile blames whatever happened to be running at the time.
//
// Set ONCE per thread, at start, not per block — it is a control-register write, and doing it in
// the render loop would cost more than the denormals.
//
// On Apple Silicon the FPU flushes denormals by default in the ARM64 ABI, so this is a no-op
// there rather than a lie: the flag it sets is the same one, and setting it costs nothing.
inline void enableFlushToZero() {
#if defined(__x86_64__) || defined(_M_X64)
  // FTZ (bit 15) makes denormal RESULTS flush to zero; DAZ (bit 6) makes denormal INPUTS read as
  // zero. Both are needed: FTZ alone still pays full cost on a denormal that arrives from a
  // buffer written by something else.
  unsigned int csr = __builtin_ia32_stmxcsr();
  csr |= (1u << 15) | (1u << 6);
  __builtin_ia32_ldmxcsr(csr);
#elif defined(__aarch64__)
  uint64_t fpcr = 0;
  __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
  fpcr |= (1ull << 24);  // FZ
  __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#endif
}

// Raise the CALLING thread's scheduling class toward the audio deadline. Best-effort:
// a failure or an unsupported platform simply leaves the thread at default priority.
inline void elevateToAudioPriority() {
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

// Promote the CALLING thread to true realtime (mach time-constraint) scheduling, tied to
// the audio block period. This is what CoreAudio's own IO thread uses: the scheduler
// promises the thread the CPU once per `periodMs`, expecting it to need about
// `computeMs` of work and to finish within `constraintMs`. A realtime thread preempts
// normal and even USER_INTERACTIVE threads, so a block render is no longer at the mercy
// of background/UI work — which is exactly what lets the pipeline run shallow (low
// latency) without starving. Best-effort: on failure or off-Apple it falls back to the
// USER_INTERACTIVE QoS and returns false.
//
// computeMs should be a generous estimate of the per-block CPU cost: if a thread
// habitually overruns it the system may stop honoring the realtime designation, so it is
// better to over- than under-state it. constraintMs is the hard deadline (<= periodMs).
inline bool setRealtimeThreadPriority(double periodMs,
                                      double computeMs,
                                      double constraintMs) {
#if defined(__APPLE__)
  mach_timebase_info_data_t timebase{};
  if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.numer == 0) {
    elevateToAudioPriority();
    return false;
  }
  // Convert milliseconds to mach absolute-time ticks: ticks = ns * denom / numer.
  const auto msToAbs = [&](double ms) -> uint32_t {
    const double ticks = ms * 1.0e6 * static_cast<double>(timebase.denom) /
                         static_cast<double>(timebase.numer);
    return static_cast<uint32_t>(ticks);
  };
  thread_time_constraint_policy_data_t policy{};
  policy.period = msToAbs(periodMs);
  policy.computation = msToAbs(computeMs);
  policy.constraint = msToAbs(constraintMs);
  policy.preemptible = 1;
  const kern_return_t rc = thread_policy_set(
      pthread_mach_thread_np(pthread_self()),
      THREAD_TIME_CONSTRAINT_POLICY,
      reinterpret_cast<thread_policy_t>(&policy),
      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
  if (rc != KERN_SUCCESS) {
    elevateToAudioPriority();
    return false;
  }
  return true;
#else
  (void)periodMs;
  (void)computeMs;
  (void)constraintMs;
  elevateToAudioPriority();
  return false;
#endif
}

}  // namespace daw
