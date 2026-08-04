#pragma once
// THE PER-TRACK RENDER POOL AND WHETHER TO USE IT THIS BLOCK — which is not "always".
//
// SIZED TO LEAVE ROOM, not to claim every core. A producer that finishes a block fractionally
// sooner by starving the thread that PLAYS it has made things worse, so the pool is
// hardware_concurrency - 2, and DAW_ENGINE_RENDER_THREADS overrides. 0 or 1 keeps everything on
// the producer thread, which is also the reference the parallel path is checked for bit-identical
// output against.
//
// IT ENGAGES ON THE WORK, NOT ON THE TRACK COUNT. Measured on a real device: at 8 sampler tracks
// one thread spends 0.18x of the block budget and has room to spare, and waking seven workers
// every block to help costs MORE than it saves — across four runs the pool dropped 4/0/2/7
// callbacks where one thread dropped 0/3/0/0. Those workers compete for cores with the audio
// callback itself, the one thread that must never wait.
//
// THE SIGNAL IS SUMMED SAMPLER CPU PER BLOCK, which is the serial-equivalent cost and therefore
// means the same thing whichever mode is currently running. A wall-clock signal would read low
// BECAUSE the pool was on, and oscillate the moment it turned off — the classic feedback loop in a
// controller that measures its own effect instead of its input.
//
// The pool and its three decision variables were four main() locals in two Deps structs. They are
// one thing: the engage decision is meaningless without the pool, and the pool is never used
// without consulting it.
#include "render_pool.h"

namespace daw::engine {

struct RenderPoolOwner {
  daw::RenderPool renderPool;
  bool poolAlwaysOn = false;   // DAW_ENGINE_RENDER_THREADS was set explicitly
  bool poolEngaged = false;    // engaged for THIS block
  double poolWorkEwmaUs = 0.0;  // the smoothed signal the decision reads
};

}  // namespace daw::engine
