#pragma once

#include <string>

#include "Output/TargetContextDecision.h"

class DasherBridge;
class DirectModeService;

// RFC 0015 clause 8 / RFC 0019 clause 6 — caret-move context triggers on
// direct-mode targets (GTK's deferred atspi watch). Two guards from the
// Windows experience (Dasher-Windows#58) are designed in from the start:
//
//   1. SELF-INJECTION QUIET WINDOW — our own ydotool injections change the
//      target's text/caret and fire the very events we listen for; seeding
//      on them resets the canvas mid-dash (the Outlook "every Enter resets
//      the canvas" bug). Events within kQuietWindowMs of the last injection
//      are dropped.
//   2. SHADOW-COMPARE SKIP — when the target read matches what the engine's
//      edit buffer already holds (text + caret), the seed would be a no-op
//      for the model but a full node-tree rebuild for the canvas; skip it.
//
// Focus changes take the same path (tier 2: switching fields re-reads and
// re-seeds instead of resetting to empty).
class TargetContextWatcher {
  public:
    // atspi reachable (bus + init). Cheap once-initialised probe.
    static bool available();

    // Start watching the desktop's focus + caret moves. Callbacks arrive on
    // the GLib main loop (same thread as GTK). Idempotent.
    void start(DasherBridge* bridge, DirectModeService* direct);

    // Stop listening; cancels any pending debounced read. Idempotent.
    void stop();

    ~TargetContextWatcher();

    struct Impl; // pimpl: the atspi plumbing TU needs the type

  private:
    Impl* m_impl = nullptr;
};
