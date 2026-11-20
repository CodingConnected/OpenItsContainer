#include "cif/CifHost.hpp"
#include "cif/CifController.h"
#include "cif/CifMessageServer.hpp"
#include "fi/FiControllerApp.hpp"
#include "models/tlc-fi/Enums.hpp"

#include <ctime>
#include <iostream>
#include <mutex>
#include <sstream>

// The CIF_PB_AANT_PARM1 object-count constant is defined by CIF_INC.h under
// CIF_PUBLIC in the controller TU (see ActuatedController.cpp). In C such a
// namespace-scope const has external linkage automatically; in C++ it defaults
// to INTERNAL linkage. Pre-declaring extern here forces external linkage so
// CifHost can read the size of the linked controller's CIF_PARM1 buffer.
// Mirror of the CIF_PB_AANT_US_* shim in ActuatedController.cpp.
extern "C"
{
    extern const short CIF_PB_AANT_PARM1;
}

namespace cif
{

    CifHost::CifHost(tlcfi::fi::FiControllerApp &app, CifBridge &bridge)
        : app_(app), bridge_(bridge)
    {
        onLog = [](::tlcfi::fi::LogLevel level, const std::string &message)
        {
            using L = ::tlcfi::fi::LogLevel;
            switch (level)
            {
            case L::Trace:
                std::cout << "[TRACE] " << message << "\n";
                break;
            case L::Debug:
                std::cout << "[DEBUG] " << message << "\n";
                break;
            case L::Info:
                std::cout << "[INFO] " << message << "\n";
                break;
            case L::Warning:
                std::cout << "[WARNING] " << message << "\n";
                break;
            case L::Error:
                std::cerr << "[ERROR] " << message << "\n";
                break;
            case L::Fatal:
                std::cerr << "[FATAL] " << message << "\n";
                break;
            }
        };
    }

    void CifHost::log(::tlcfi::fi::LogLevel level, const std::string &message)
    {
        if (onLog)
            onLog(level, message);
    }

    CifHost::~CifHost()
    {
        stopLoop();
        if (messageServer_)
        {
            messageServer_->stop();
        }
        if (loggingServer_)
        {
            loggingServer_->stop();
        }
    }

    void CifHost::initRingPointers()
    {
        // Initialize ring buffer pointers for IBER/UBER message buffers
        CIF_IBER_LEES = CIF_IBER;
        CIF_IBER_SCHRIJF = CIF_IBER;
        CIF_UBER_LEES = CIF_UBER;
        CIF_UBER_SCHRIJF = CIF_UBER;

        // Initialize ring buffer pointers for MON IBER/UBER message buffers
        CIF_MON_IBER_LEES = CIF_MON_IBER;
        CIF_MON_IBER_SCHRIJF = CIF_MON_IBER;
        CIF_MON_UBER_LEES = CIF_MON_UBER;
        CIF_MON_UBER_SCHRIJF = CIF_MON_UBER;

        // Initialize ring buffer pointers and status for FILE UBER logging buffer
        CIF_FILE_UBER_LEES = CIF_FILE_UBER;
        CIF_FILE_UBER_SCHRIJF = CIF_FILE_UBER;
        CIF_FILE_UBER_FILEWISSEL = nullptr;
        CIF_PB_FILE_UBER_FILEWISSEL = 0;
        CIF_FILE_UBER_STATUS = 1;
        CIF_FILE_UBER_FILEEXT[0] = 'v';
        CIF_FILE_UBER_FILEEXT[1] = 'l';
        CIF_FILE_UBER_FILEEXT[2] = 'g';
        CIF_FILE_UBER_FILEEXT[3] = '\0';
    }

    void CifHost::buildMappingsFromController()
    {
        // Per-type object counts come from the CVN-C interface constants, which a
        // conformant controller sizes to its actual configuration. Counts set the
        // CVN offsets in the bridge (outputs follow signal groups in CIF_GUS[];
        // inputs follow detectors in CIF_IS[]), so set them before the mappings.
        const int nSg = CIF_PB_AANT_US_FC;
        const int nOut = CIF_PB_AANT_US_OV;
        const int nDet = CIF_PB_AANT_IS_D;
        const int nIn = CIF_PB_AANT_IS_OV;

        bridge_.setSignalGroupCount(nSg);
        bridge_.setDetectorCount(nDet);

        // Scan the controller's (cifName, index) pairs per type, then hand them
        // to the bridge in one call. The bridge resolves manual couples (which
        // win over identity matches) and controller-only exclusions internally,
        // so the ordering of the couple/identity decision does not depend on the
        // order the controller reports its items in.
        std::vector<std::pair<std::string, int>> sg, out, det, in;
        for (int i = 0; i < nSg; ++i)
            if (const char *name = getCifItemName(CIF_OBJ_SIGNALGROUP, i))
                sg.emplace_back(name, i);
        for (int i = 0; i < nOut; ++i)
            if (const char *name = getCifItemName(CIF_OBJ_OUTPUT, i))
                out.emplace_back(name, i);
        for (int i = 0; i < nDet; ++i)
            if (const char *name = getCifItemName(CIF_OBJ_DETECTOR, i))
                det.emplace_back(name, i);
        for (int i = 0; i < nIn; ++i)
            if (const char *name = getCifItemName(CIF_OBJ_INPUT, i))
                in.emplace_back(name, i);

        bridge_.buildMappings(sg, out, det, in);
    }

    void CifHost::init()
    {
        // Construct the CIF_PARM1 persistence store before the first CIF_INIT so
        // reapplyParam1Overrides() below can use it. If no path was set,
        // param1Store_ stays null and the reapply / poll calls are no-ops.
        if (!param1StorePath_.empty())
        {
            param1Store_ = std::make_unique<Param1Store>(param1StorePath_, param1StoreInterval_);
        }

        // Initialise the hosted controller BEFORE reading its object table.
        // Controllers commonly populate their id tables during CIF_INIT (e.g. CVN
        // code arrays filled inside applicatieprogramma), so getCifItemName() only
        // returns valid names after CIF_INIT has run once.
        initRingPointers();
        (void)applicatieprogramma(1); // CIF_INIT
        initRingPointers();           // CIF_INIT may reset host-owned ring pointers
        gpsCacheValid_ = false;       // first GPS reading after CIF_INIT should always log
        // Apply persisted CIF_PARM1 overrides on top of the just-init'd defaults.
        // No cifMutex_ needed: the bridge has not been started yet, so no other
        // thread can be touching CIF_PARM1 here.
        reapplyParam1Overrides();
        // Initial CIF_WPS so the first applicatieprogramma cycle does not see
        // undefined state. Operational status starts off (GEDOOFD); external
        // control axis starts at Offline (NIET_GEREED) until the TLC says otherwise.
        setProgControl(static_cast<int>(::tlcfi::tlcfi::ControlState::Offline));
        setProgStatus(CIF_STAT_GEDOOFD);

        buildMappingsFromController();

        // Warn about controller-only entries that never matched a controller item.
        // Almost always a typo in the operator's tlcfi.json.
        {
            std::vector<std::string> unmSg, unmOut, unmDet, unmIn;
            bridge_.getUnmatchedIgnoredItems(unmSg, unmOut, unmDet, unmIn);
            auto warn = [this](const char *label, const std::vector<std::string> &ids)
            {
                if (ids.empty())
                    return;
                std::string msg = std::string("CifHost: WARNING: controllerOnlyItems ") + label + " not found in controller config:";
                for (const auto &id : ids)
                    msg += " " + id;
                msg += " (likely typo)";
                log(::tlcfi::fi::LogLevel::Warning, msg);
            };
            warn("signalGroup", unmSg);
            warn("output", unmOut);
            warn("detector", unmDet);
            warn("input", unmIn);
        }

        // Controller-side coupling completeness (mapping phase — the TLC is not
        // connected yet, so only controller/config self-consistency is checked
        // here; the cross-side match happens later in validateMappings()). Two
        // fatal conditions:
        //   1. a manuallyCoupledItems entry whose cifName is absent from the
        //      controller (the CIF side of the couple does not exist);
        //   2. a controller item that ended up coupled to nothing — its name is
        //      shadowed by a manual couple's tlcfiName target, or it is a
        //      duplicate. The operator must resolve these explicitly (e.g. add
        //      the shadowed item to controllerOnlyItems).
        {
            std::ostringstream errs;

            std::vector<::cif::ManualCouple> umSg, umOut, umDet, umIn;
            bridge_.getUnmatchedManualCouples(umSg, umOut, umDet, umIn);
            auto reportCouples = [&errs](const char *label, const std::vector<::cif::ManualCouple> &cs)
            {
                if (cs.empty())
                    return;
                errs << "manuallyCoupledItems " << label << " with cifName not found in controller config:";
                for (const auto &c : cs)
                    errs << " {tlcfiName=" << c.tlcfiName << ", cifName=" << c.cifName << "}";
                errs << "\n";
            };
            reportCouples("signalGroup", umSg);
            reportCouples("output", umOut);
            reportCouples("detector", umDet);
            reportCouples("input", umIn);

            std::vector<std::string> orpSg, orpOut, orpDet, orpIn;
            bridge_.getUnmappedControllerItems(orpSg, orpOut, orpDet, orpIn);
            auto reportOrphans = [&errs](const char *label, const std::vector<std::string> &ids)
            {
                if (ids.empty())
                    return;
                errs << "controller " << label << "(s) coupled to nothing (shadowed by a manual couple or a duplicate name; declare as controllerOnlyItems if intentional):";
                for (const auto &id : ids)
                    errs << " " << id;
                errs << "\n";
            };
            reportOrphans("signalGroup", orpSg);
            reportOrphans("output", orpOut);
            reportOrphans("detector", orpDet);
            reportOrphans("input", orpIn);

            const std::string errStr = errs.str();
            if (!errStr.empty())
            {
                log(::tlcfi::fi::LogLevel::Fatal,
                    "FATAL: manual coupling configuration is invalid:\n" + errStr +
                        "Coupling must be complete and exact; declare every deviation explicitly.");
                std::exit(1);
            }
        }

        // Create and start message server if configured
        if (messageServerConfig_.has_value())
        {
            messageServer_ = std::make_unique<CifMessageServer>(messageServerConfig_.value());
            messageServer_->onLog = [this](auto level, const auto &msg)
            { log(level, msg); };
            messageServer_->start();
        }

        // Create and start logging server if configured
        if (loggingServerConfig_.has_value())
        {
            loggingServer_ = std::make_unique<CifMessageServer>(loggingServerConfig_.value());
            loggingServer_->onLog = [this](auto level, const auto &msg)
            { log(level, msg); };
            loggingServer_->start();
        }

        bridge_.start();

        // Hook meta received to validate mappings as soon as we know the TLC objects
        app_.onMetaReceived = [this](const std::vector<std::string> &,
                                     const std::vector<std::string> &,
                                     const std::vector<std::string> &,
                                     const std::vector<std::string> &)
        {
            log(::tlcfi::fi::LogLevel::Info, "CifHost: Meta received, validating mappings...");

            if (!validateMappings())
            {
                log(::tlcfi::fi::LogLevel::Fatal, "FATAL: " + getValidationErrors());
                std::exit(1);
            }

            log(::tlcfi::fi::LogLevel::Info, "CifHost: All TLC objects mapped successfully");
        };

        // Hook control state to (re)start the loop when we get control
        app_.onControlInControl = [this](const std::string &intersectionId)
        {
            log(::tlcfi::fi::LogLevel::Info,
                "CifHost: InControl for intersection " + intersectionId + ", re-initializing and starting control loop");
            {
                // Serialise CIF_INIT against TLC-FI update handlers writing CIF_WUS/CIF_IS.
                std::lock_guard<std::mutex> cifLock(bridge_.cifMutex());
                (void)applicatieprogramma(1); // CIF_INIT - reinitialize C controller for clean state
                initRingPointers();           // re-assert host-owned ring pointers after CIF_INIT
                resetKlokCounters();          // fresh CIF_KLOK[7]/[8] base for this control session
                gpsCacheValid_ = false;       // re-baseline GPS log on new control session
                // CIF_INIT clobbers CIF_PARM1 with fresh controller defaults;
                // re-apply persisted overrides so operator tuning survives every
                // InControl transition.
                reapplyParam1Overrides();
                // External axis: TLC says we are InControl.
                // Operational axis: default policy is "regulate while InControl"
                // (the operator has declared they always want control). A future
                // standby/blink mode would set PROG_STATUS to GEDOOFD/KP/AR
                // independently while leaving PROG_CONTROL at INCONTROL.
                setProgControl(static_cast<int>(::tlcfi::tlcfi::ControlState::InControl));
                setProgStatus(CIF_STAT_REG);
            }
            this->startLoop();
            if (messageServer_)
            {
                messageServer_->enableClients();
            }
            if (loggingServer_)
            {
                loggingServer_->enableClients();
            }
        };

        // EndControl: the TLC is taking control back. Tell the controller via
        // CIF_WPS, then stop the loop.
        app_.onControlEndControl = [this](const std::string &intersectionId)
        {
            log(::tlcfi::fi::LogLevel::Warning, "*** END CONTROL of intersection: " + intersectionId + " ***");
            log(::tlcfi::fi::LogLevel::Warning, "*** Stopping controller loop ***");
            {
                std::lock_guard<std::mutex> cifLock(bridge_.cifMutex());
                setProgControl(static_cast<int>(::tlcfi::tlcfi::ControlState::EndControl));
                setProgStatus(CIF_STAT_UITSCHAKELEN);
            }
            this->stopLoop();
        };

        // Hook connection lost to stop loop and reset bridge for clean reconnection
        app_.onConnectionLost = [this](const std::string &reason)
        {
            log(::tlcfi::fi::LogLevel::Warning, "CifHost: Connection lost (" + reason + "), stopping loop and resetting bridge");
            if (messageServer_)
            {
                messageServer_->disableClients();
            }
            if (loggingServer_)
            {
                loggingServer_->disableClients();
            }
            this->stopLoop();
            {
                std::lock_guard<std::mutex> cifLock(bridge_.cifMutex());
                setProgControl(static_cast<int>(::tlcfi::tlcfi::ControlState::Offline));
                setProgStatus(CIF_STAT_GEDOOFD);
            }
            this->resetBridge();
        };
    }

    bool CifHost::validateMappings() const
    {
        return bridge_.validateMappings();
    }

    std::string CifHost::getValidationErrors() const
    {
        std::vector<std::string> unmappedSg, unmappedOut, unmappedDet, unmappedIn;
        bridge_.getUnmappedObjects(unmappedSg, unmappedOut, unmappedDet, unmappedIn);

        std::vector<std::string> staleSg, staleOut, staleDet, staleIn;
        bridge_.getStaleObjects(staleSg, staleOut, staleDet, staleIn);

        bool hasUnmapped = !unmappedSg.empty() || !unmappedOut.empty() || !unmappedDet.empty() || !unmappedIn.empty();
        bool hasStale = !staleSg.empty() || !staleOut.empty() || !staleDet.empty() || !staleIn.empty();

        if (!hasUnmapped && !hasStale)
        {
            return {};
        }

        auto appendList = [](std::ostringstream &os, const std::string &label, const std::vector<std::string> &ids)
        {
            if (ids.empty())
                return;
            os << "  " << label << ": ";
            for (size_t i = 0; i < ids.size(); ++i)
            {
                if (i > 0)
                    os << ", ";
                os << ids[i];
            }
            os << "\n";
        };

        std::ostringstream oss;
        if (hasUnmapped)
        {
            oss << "TLC objects not in controller configuration:\n";
            appendList(oss, "Signal groups", unmappedSg);
            appendList(oss, "Outputs", unmappedOut);
            appendList(oss, "Detectors", unmappedDet);
            appendList(oss, "Inputs", unmappedIn);
        }
        if (hasStale)
        {
            oss << "Controller configuration objects not in TLC:\n";
            appendList(oss, "Signal groups", staleSg);
            appendList(oss, "Outputs", staleOut);
            appendList(oss, "Detectors", staleDet);
            appendList(oss, "Inputs", staleIn);
        }
        oss << "Controller configuration must match TLC objects exactly.";
        return oss.str();
    }

    void CifHost::startLoop()
    {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true))
            return; // already running
        loopThread_ = std::thread([this]
                                  { runLoop(); });
        drainThread_ = std::thread([this]
                                   { runDrainLoop(); });
    }

    void CifHost::stopLoop()
    {
        running_.store(false);
        if (loopThread_.joinable())
            loopThread_.join();
        // Drain thread must be joined before touching messageServer_ — it calls
        // pollDataOut() on the server, and disabling clients mid-call would race.
        if (drainThread_.joinable())
            drainThread_.join();
        if (messageServer_)
        {
            messageServer_->disableClients();
        }
        if (loggingServer_)
        {
            loggingServer_->disableClients();
        }
    }

    void CifHost::runDrainLoop()
    {
        // Tick fast enough that CIF_UBER never observes "full" for long.
        // 5 ms (200 Hz) is two orders of magnitude faster than the controller
        // cycle and cheap CPU-wise. Pointer-only writes to CIF_UBER_LEES /
        // CIF_MON_UBER_LEES are atomic on x86_64; CCOL only writes _SCHRIJF.
        const auto tick = std::chrono::milliseconds(5);
        while (running_.load())
        {
            if (messageServer_)
            {
                messageServer_->pollDataOut();
            }
            else
            {
                CIF_UBER_LEES = CIF_UBER_SCHRIJF;
            }
            if (loggingServer_)
            {
                loggingServer_->pollDataOut();
            }
            else
            {
                CIF_MON_UBER_LEES = CIF_MON_UBER_SCHRIJF;
            }
            std::this_thread::sleep_for(tick);
        }
    }

    void CifHost::setMessageServerConfig(unsigned short port, const std::string &bindAddress,
                                         bool echoInput, const std::string &welcomeMessage)
    {
        // Read sizes from the linked controller's CIF.INC at runtime. Hardcoding
        // these against a different controller's actual buffer size makes
        // pollDataOut wrap the read pointer at the wrong byte and spin forever.
        messageServerConfig_ = {
            .port = port,
            .bind_address = bindAddress,
            // The serial console is a bidirectional command channel: clients both
            // write into CIF_IBER and read from CIF_UBER. Two simultaneous clients
            // would interleave commands into CIF_IBER and split the CIF_UBER
            // response stream between them, corrupting both. Restrict to a single
            // client. (The logging server is a broadcast monitor and keeps its
            // multi-client default.)
            .max_sessions = 1,
            .data_in_size = CIF_PB_MAX_IBER,
            .data_out_size = CIF_PB_MAX_UBER,
            .data_in = CIF_IBER,
            .data_out = CIF_UBER,
            .data_in_read_ptr = &CIF_IBER_LEES,
            .data_in_write_ptr = &CIF_IBER_SCHRIJF,
            .data_out_read_ptr = &CIF_UBER_LEES,
            .data_out_write_ptr = &CIF_UBER_SCHRIJF,
            .cif_mutex = &bridge_.cifMutex(),
            .welcome_message = welcomeMessage,
            .echo_input = echoInput,
            .inactiveMessage = "CIF controller not running\r\n",
        };
    }

    void CifHost::setLoggingServerConfig(unsigned short port, const std::string &bindAddress)
    {
        loggingServerConfig_ = {
            .port = port,
            .bind_address = bindAddress,
            .data_in_size = CIF_PB_MAX_MON_IBER,
            .data_out_size = CIF_PB_MAX_MON_UBER,
            .data_in = CIF_MON_IBER,
            .data_out = CIF_MON_UBER,
            .data_in_read_ptr = &CIF_MON_IBER_LEES,
            .data_in_write_ptr = &CIF_MON_IBER_SCHRIJF,
            .data_out_read_ptr = &CIF_MON_UBER_LEES,
            .data_out_write_ptr = &CIF_MON_UBER_SCHRIJF,
            .cif_mutex = &bridge_.cifMutex(),
        };
    }

    void CifHost::resetBridge()
    {
        bridge_.reset();
    }

    void CifHost::setParam1StoreFile(std::filesystem::path file,
                                     std::chrono::milliseconds minInterval)
    {
        param1StorePath_ = std::move(file);
        param1StoreInterval_ = minInterval;
    }

    void CifHost::setIgnoredItems(IgnoredItems items)
    {
        bridge_.setIgnoredItems(std::move(items));
    }

    void CifHost::setManualCouples(ManualCouples couples)
    {
        bridge_.setManualCouples(std::move(couples));
    }

    void CifHost::setClockFromSystem(std::chrono::system_clock::time_point tp)
    {
        using namespace std::chrono;
        auto t = system_clock::to_time_t(tp);
#if defined(_WIN32)
        struct tm lt;
        localtime_s(&lt, &t);
#else
        struct tm lt;
        localtime_r(&t, &lt);
#endif
        auto ms = duration_cast<milliseconds>(tp.time_since_epoch()).count() % 1000;
        CIF_KLOK[CIF_JAAR] = static_cast<s_int16>(1900 + lt.tm_year);
        CIF_KLOK[CIF_MAAND] = static_cast<s_int16>(1 + lt.tm_mon);
        CIF_KLOK[CIF_DAG] = static_cast<s_int16>(lt.tm_mday);
        CIF_KLOK[CIF_DAGSOORT] = static_cast<s_int16>(lt.tm_wday);
        CIF_KLOK[CIF_UUR] = static_cast<s_int16>(lt.tm_hour);
        CIF_KLOK[CIF_MINUUT] = static_cast<s_int16>(lt.tm_min);
        CIF_KLOK[CIF_SECONDE] = static_cast<s_int16>(lt.tm_sec);

        // CIF_SEC_TELLER / CIF_TSEC_TELLER are contracted by CIF_INC.h as
        // unsigned counters in the range 0..32767 — they MUST wrap back to 0,
        // not overflow into negative s_int16. CCOL drives its phase timing from
        // these counters; if they ever go negative, time-delta math inside
        // applicatieprogramma() blows up and the intersection appears frozen
        // (CIF_SEC_TELLER hits the wrap after ~9h6m, which is the "overnight"
        // freeze we were chasing).
        if (++klokTenths_ == 10)
        {
            klokTenths_ = 0;
            CIF_KLOK[CIF_SEC_TELLER] = (CIF_KLOK[CIF_SEC_TELLER] >= 32767)
                                           ? 0
                                           : static_cast<s_int16>(CIF_KLOK[CIF_SEC_TELLER] + 1);
        }
        CIF_KLOK[CIF_TSEC_TELLER] = (CIF_KLOK[CIF_TSEC_TELLER] >= 32767)
                                        ? 0
                                        : static_cast<s_int16>(CIF_KLOK[CIF_TSEC_TELLER] + 1);
#if !defined(NO_CVN_50)
        CIF_KLOK[CIF_TSECONDE] = static_cast<s_int16>(ms / 100); // tenths
#endif
    }

    void CifHost::resetKlokCounters()
    {
        klokTenths_ = 0;
        CIF_KLOK[7] = 0;
        CIF_KLOK[8] = 0;
    }

    void CifHost::setProgControl(int tlcfiControlState)
    {
        // TLC-FI ControlState enum values match CIF_CONTROL_* constants 1:1.
        s_int16 progControl = CIF_CONTROL_NIET_GEREED; // safe default
        using ::tlcfi::tlcfi::ControlState;
        switch (static_cast<ControlState>(tlcfiControlState))
        {
        case ControlState::Error:
            progControl = CIF_CONTROL_FOUT;
            break;
        case ControlState::NotConfigured:
            progControl = CIF_CONTROL_NIET_GECONFIG;
            break;
        case ControlState::Offline:
            progControl = CIF_CONTROL_NIET_GEREED;
            break;
        case ControlState::ReadyToControl:
            progControl = CIF_CONTROL_GEREED;
            break;
        case ControlState::StartControl:
            progControl = CIF_CONTROL_START_CONTROL;
            break;
        case ControlState::InControl:
            progControl = CIF_CONTROL_INCONTROL;
            break;
        case ControlState::EndControl:
            progControl = CIF_CONTROL_EINDE_CONTROL;
            break;
        }
        CIF_WPS[CIF_PROG_CONTROL] = progControl;
        CIF_WPSWIJZ = CIF_GESCHREVEN;
        log(::tlcfi::fi::LogLevel::Debug,
            "CifHost: CIF_WPS[PROG_CONTROL]=" + std::to_string(progControl) + " (TLC-FI ControlState=" + std::to_string(tlcfiControlState) + ")");
    }

    void CifHost::setProgStatus(s_int16 progStatus)
    {
        CIF_WPS[CIF_PROG_STATUS] = progStatus;
        CIF_WPSWIJZ = CIF_GESCHREVEN;
        log(::tlcfi::fi::LogLevel::Debug, "CifHost: CIF_WPS[PROG_STATUS]=" + std::to_string(progStatus));
    }

    void CifHost::pollGpsStateRequest()
    {
        if (CIF_GPSWIJZ == 0)
            return;

        const s_int16 progStatus = CIF_GPS[CIF_PROG_STATUS];
        const s_int16 progControl = CIF_GPS[CIF_PROG_CONTROL];
        const s_int16 progFout = CIF_GPS[CIF_PROG_FOUT];

        // CCOL flips CIF_GPSWIJZ every cycle (it writes PROG_FOUT with the
        // fasebewaking result regardless), so only log on actual value change.
        const bool changed = !gpsCacheValid_ || progStatus != lastGpsStatus_ || progControl != lastGpsControl_ || progFout != lastGpsFout_;

        if (changed)
        {
            log(::tlcfi::fi::LogLevel::Debug,
                "CifHost: CIF_GPS changed: PROG_STATUS=" + std::to_string(progStatus) + " PROG_CONTROL=" + std::to_string(progControl) + " PROG_FOUT=" + std::to_string(progFout) + " (not propagated; operator policy = always want control)");
            lastGpsStatus_ = progStatus;
            lastGpsControl_ = progControl;
            lastGpsFout_ = progFout;
            gpsCacheValid_ = true;
        }
        CIF_GPSWIJZ = 0;
    }

    void CifHost::reapplyParam1Overrides()
    {
        if (!param1Store_)
            return;

        const int count = CIF_PB_AANT_PARM1;
        param1Store_->captureBaseline(count, CIF_PARM1);
        const auto result = param1Store_->loadInto(count, CIF_PARM1);
        switch (result)
        {
        case Param1Store::LoadResult::NoFile:
            log(::tlcfi::fi::LogLevel::Info, "CifHost: CIF_PARM1 persistence file not present; using controller defaults");
            break;
        case Param1Store::LoadResult::LoadedOverrides:
            log(::tlcfi::fi::LogLevel::Info, "CifHost: CIF_PARM1 overrides applied from '" + param1StorePath_.string() + "'");
            break;
        case Param1Store::LoadResult::Mismatched:
            log(::tlcfi::fi::LogLevel::Warning, "CifHost: CIF_PARM1 persistence file mismatched (count/schema); archived, using controller defaults");
            break;
        }

        // Either of CIF_INIT itself or the just-applied loadInto may have set the
        // WIJZ flags (some controllers initialise them to CIF_INIT_PARM in
        // CIF_INIT). Clear them so the very next poll does not interpret the
        // loaded values as fresh edits.
        CIF_PARM1WIJZAP = CIF_GEEN_PARMWIJZ;
        CIF_PARM1WIJZPB = CIF_GEEN_PARMWIJZ;
    }

    void CifHost::pollParam1Changes()
    {
        if (!param1Store_)
            return;

        const s_int16 wijAp = CIF_PARM1WIJZAP;
        const s_int16 wijPb = CIF_PARM1WIJZPB;

        auto handle = [this](s_int16 flag)
        {
            if (flag == CIF_GEEN_PARMWIJZ || flag == CIF_INIT_PARM)
            {
                return;
            }
            if (flag == CIF_MEER_PARMWIJZ)
            {
                param1Store_->recordMultiChange(CIF_PB_AANT_PARM1, CIF_PARM1);
                return;
            }
            if (flag >= 0 && flag < CIF_PB_AANT_PARM1)
            {
                param1Store_->recordChange(flag, CIF_PARM1[flag]);
            }
        };
        handle(wijAp);
        handle(wijPb);

        CIF_PARM1WIJZAP = CIF_GEEN_PARMWIJZ;
        CIF_PARM1WIJZPB = CIF_GEEN_PARMWIJZ;
    }

    void CifHost::runLoop()
    {
        // Initialize clock and application
        auto now = std::chrono::system_clock::now();
        setClockFromSystem(now);

        // Synchronize detector and input states from TLC to CIF_IS[] before starting
        // This ensures the C controller knows the current state of all inputs
        log(::tlcfi::fi::LogLevel::Info, "CifHost: Synchronizing detector and input states from TLC to CIF_IS[]");
        bridge_.synchronizeInputStates();

        const auto period = std::chrono::milliseconds(100);
        auto nextTick = std::chrono::steady_clock::now();

        log(::tlcfi::fi::LogLevel::Info, "CifHost::runLoop() started");

        auto t = 0;
        while (running_.load())
        {
            nextTick += period;

            auto nowSys = std::chrono::system_clock::now();
            setClockFromSystem(nowSys);

            ++t;
            if (t % 10 == 0)
            {
                log(::tlcfi::fi::LogLevel::Trace,
                    "Loop: " + std::to_string(t) + ", Klok: " + std::to_string(CIF_KLOK[0]) + "-" + std::to_string(CIF_KLOK[1]) + "-" + std::to_string(CIF_KLOK[2]) + " " + std::to_string(CIF_KLOK[4]) + ":" + std::to_string(CIF_KLOK[5]) + ":" + std::to_string(CIF_KLOK[6]));
            }

            bridge_.prepareDsiBuffer();
            {
                // Hold cifMutex_ while the legacy controller reads CIF_WUS[]/CIF_IS[]
                // so TLC-FI update handlers cannot mutate them mid-cycle.
                std::lock_guard<std::mutex> cifLock(bridge_.cifMutex());
                (void)applicatieprogramma(0);
                pollGpsStateRequest();
                pollParam1Changes();
            }
            bridge_.pollFileVlog();

            bridge_.processDesiredOutputChanges();

            // CIF_UBER / CIF_MON_UBER are drained on a separate thread (see
            // runDrainLoop) so that CIF_UBER_LEES can advance concurrently with
            // applicatieprogramma() — CCOL stalls inside its own uber_puts /
            // waitterm path if the ring stays full mid-cycle and we cannot
            // unblock it from this thread while we hold cifMutex_.

            auto nowSteady = std::chrono::steady_clock::now();
            if (nowSteady < nextTick)
            {
                std::this_thread::sleep_until(nextTick);
            }
            else
            {
                int backlog = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(nowSteady - nextTick).count() / 100);
                backlog = std::min(backlog, 3);
                for (int i = 0; i < backlog; ++i)
                {
                    nowSys = std::chrono::system_clock::now();
                    setClockFromSystem(nowSys);
                    bridge_.prepareDsiBuffer();
                    {
                        std::lock_guard<std::mutex> cifLock(bridge_.cifMutex());
                        (void)applicatieprogramma(0);
                        pollGpsStateRequest();
                        pollParam1Changes();
                    }
                    // A catch-up cycle is a full controller cycle: drain FILE_UBER
                    // and forward CIF_GUS[] changes, exactly as the normal path
                    // above does, otherwise state updates from these cycles are
                    // lost when the loop is running behind. CIF_UBER / CIF_MON_UBER
                    // are drained continuously by runDrainLoop on its own thread.
                    bridge_.pollFileVlog();
                    bridge_.processDesiredOutputChanges();
                }
                nextTick = std::chrono::steady_clock::now();
            }
        }
    }

} // namespace cif
