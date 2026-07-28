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
#include <pthread/qos.h>
#endif

namespace daw {

// Raise the CALLING thread's scheduling class toward the audio deadline. Best-effort:
// a failure or an unsupported platform simply leaves the thread at default priority.
inline void elevateToAudioPriority() {
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

}  // namespace daw
