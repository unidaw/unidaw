#pragma once
// THE ONE CLIP-WINDOW REQUEST IN FLIGHT, and the lock that makes "one" true.
//
// A UI asks for a window of a clip's notes; the consumer answers it on its own tick by publishing
// into shared memory. Only the LATEST request matters — a client that asked for bars 1-4 and then
// scrolled to bars 5-8 does not want the first answer — so this is an optional holding at most one,
// overwritten rather than queued. That is why it is a std::optional and not a ring.
//
// THE MUTEX AND THE SLOT ARE ONE THING: the command thread writes the request, the consumer takes
// and clears it, and both operations touch the same two words. They were two main() locals passed
// as two members into RequestCommandDeps and ConsumerDeps — the writer and the reader, each given
// half of an invariant that only holds when both halves move together.
#include <mutex>
#include <optional>

#include "engine_types.h"

namespace daw::engine {

struct ClipWindow {
  std::mutex clipWindowMutex;
  std::optional<ClipWindowPending> clipWindowPending;
};

}  // namespace daw::engine
