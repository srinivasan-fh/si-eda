// SiEDA Core — the C API's project handle, shared by the C ABI translation units (sieda_c.cpp, sieda_c_routing.cpp).
#pragma once

#include <atomic>
#include <memory>

#include "sieda/InteractiveRouter.hpp"
#include "sieda/Project.hpp"

struct SiedaProject {
    sieda::Project project;
    std::unique_ptr<sieda::InteractiveRouter> router;  // interactive route session (created on first use)
    /// sieda_router_abort bumps it (lock-free, from any thread) to cancel the router's head computation in flight.
    std::atomic<unsigned> routerAbort{0};
};
