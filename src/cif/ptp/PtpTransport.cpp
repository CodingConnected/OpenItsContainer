// PTP driver contract — the C symbols the hosted controller's PTP module
// (ptpcom.c) calls to move raw characters over the transport. Providing these
// is the container's entire responsibility for PTP; all protocol content
// (framing, CRC, koppelsignaal encode/decode, timers) lives in the controller.
//
// These are the symbols whose presence (referenced by a PTP-enabled vendor
// controller) pulls this translation unit — and, transitively, the whole PTP
// transport — into the final binary. A controller built without PTP references
// none of them, so static-lib linkage drops the entire module: PTP is optional
// at zero cost.
//
// Contract (ASTRIN PTP spec §3): portnr is the controller's PTP->PORTNR — an
// engineer-chosen link key matched against tlcfi.json's ptpLinks[].linkId (any
// value; only uniqueness per VRI matters). EOB (-1) is the empty/overflow
// sentinel, and each ring is >= 2048 bytes. The controller polls bezet_COM_IBER
// before reading, so a data byte 0xFF (which casts to (char)-1 == EOB) is never
// mistaken for "empty".

#include "cif/ptp/PtpManager.hpp"

#include "runtime/RuntimeApp.hpp" // registerStartupHook / activeOptions

namespace {

// Bring the PTP transport up at container startup rather than lazily on the
// first driver call. This TU is linked exactly when a controller uses PTP (it
// references the *_COM_* symbols below), so this file-scope initializer runs
// pre-main only for PTP-enabled controllers — zero cost otherwise. It merely
// registers a hook; runContainer runs it once configuration is live, so
// activeOptions() is populated by the time it fires. Constructing the manager
// brings every configured link up and starts the io thread, which then lives
// for the process (self-reconnecting on break) — the "always-on" lifecycle.
//
// The magic-static manager means the lazy path below still works as a fallback
// and never double-constructs; the empty-links guard avoids spinning an idle io
// thread when a PTP-capable controller runs with no ptpLinks configured.
struct PtpStartupRegistrar {
    PtpStartupRegistrar() {
        ::tlcfi::runtime::registerStartupHook([] {
            if (!::tlcfi::runtime::activeOptions().ptpLinks.empty()) {
                (void)::cif::ptp::PtpManager::instance();
            }
        });
    }
} g_ptpStartupRegistrar;

} // namespace

extern "C" {

// --- Incoming characters (COM_IBER): socket -> controller ---

void init_COM_IBER(int portnr) {
    ::cif::ptp::PtpManager::instance().initIber(portnr);
}

char lees_char_COM_IBER(int portnr) {
    return static_cast<char>(::cif::ptp::PtpManager::instance().readChar(portnr));
}

int bezet_COM_IBER(int portnr) {
    return ::cif::ptp::PtpManager::instance().busy(portnr);
}

// --- Outgoing characters (COM_UBER): controller -> socket ---

void init_COM_UBER(int portnr) {
    ::cif::ptp::PtpManager::instance().initUber(portnr);
}

void schrijf_char_COM_UBER(int portnr, char c) {
    ::cif::ptp::PtpManager::instance().writeChar(portnr, c);
}

} // extern "C"
