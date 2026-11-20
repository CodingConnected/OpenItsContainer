#include "runtime/RuntimeApp.hpp"

#include <utility>
#include <vector>

// The active-options accessor lives in this dedicated translation unit — apart
// from runContainer — on purpose. Static-lib linkage pulls whole objects: if
// activeOptions() shared a TU with runContainer, any code that reads the active
// options (notably the optional PTP transport) would drag runContainer and its
// entire dependency graph, which needs a hosted controller's CIF_* symbols.
// Isolating it here keeps activeOptions() cheap to reference from anywhere.
//
// The startup-hook registry below shares this TU for the same reason: it is the
// inversion point that lets the optional PTP transport ask to be brought up at
// container startup without the always-linked runtime ever naming a PTP symbol
// (which would force-link tlcfi_ptp into every controller). A PTP-enabled
// controller's PtpTransport TU registers a hook pre-main; runContainer runs the
// hooks once configuration is live. See [[ptp-optional-linkage]].

namespace tlcfi::runtime {

namespace {
// Points at runContainer's local RuntimeOptions once configuration is parsed.
// Valid for the remainder of the process (opts outlives everything it feeds).
const RuntimeOptions* g_activeOptions = nullptr;

// Function-local static (Meyers) to sidestep any static-init-order issue with
// the pre-main registrations that append to it.
std::vector<StartupHook>& startupHooks() {
    static std::vector<StartupHook> hooks;
    return hooks;
}
} // namespace

void setActiveOptions(const RuntimeOptions* opts) {
    g_activeOptions = opts;
}

const RuntimeOptions& activeOptions() {
    static const RuntimeOptions empty{};
    return g_activeOptions ? *g_activeOptions : empty;
}

void registerStartupHook(StartupHook hook) {
    if (hook) {
        startupHooks().push_back(std::move(hook));
    }
}

void runStartupHooks() {
    for (auto& hook : startupHooks()) {
        hook();
    }
}

} // namespace tlcfi::runtime
