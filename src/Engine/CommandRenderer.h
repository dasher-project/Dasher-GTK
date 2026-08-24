#pragma once

#include "Engine/DasherBridge.h"
#include <cairomm/context.h>
#include <string>
#include <vector>

class CommandRenderer {
public:
    CommandRenderer();

    void render(const DasherBridge::FrameResult& frame, const Cairo::RefPtr<Cairo::Context>& cr);

    // The canvas font lives on the bridge (single source of truth) so the
    // engine's text-measurement callback and this renderer always agree;
    // the renderer reads it per frame via the provided bridge.
    void set_bridge(std::shared_ptr<DasherBridge> bridge);

  private:
    std::shared_ptr<DasherBridge> m_bridge;
};
