#include <gtest/gtest.h>

#include "cif/CifBridge.hpp"
#include "cif/CIF_INC.h"
#include "fi/FiControllerApp.hpp"
#include "models/tlc-fi/Enums.hpp"

#include <nlohmann/json.hpp>

#include <string>

// End-to-end tests for the CIF bridge: a TLC-FI UpdateState fed into
// FiControllerApp must flow through the registered onXxxUpdate callbacks into
// CifBridge and populate the shared CIF_WUS[] / CIF_IS[] arrays.
//
// The full path exercised is:
//   ObjectStateUpdateGroup
//     -> FiControllerApp::processObjectUpdates
//       -> FiConsumerApp::updateSignalGroupState/...   (parses JSON state)
//         -> app.onSignalGroupUpdate / ...             (bridge callbacks)
//           -> CifBridge::handleSignalGroupUpdate/...  (index translation)
//             -> CIF_WUS[] / CIF_IS[]

using nlohmann::json;
using tlcfi::fi::FiControllerApp;
namespace tlc = tlcfi::tlcfi;
namespace genfi = tlcfi::genfi;

namespace {

// Exposes the protected processObjectUpdates() so a test can inject an
// UpdateState exactly as the JSON-RPC notification path would.
class TestableControllerApp : public FiControllerApp {
public:
	using FiControllerApp::FiControllerApp;
	void injectUpdate(const genfi::ObjectStateUpdateGroup& g) {
		processObjectUpdates(g);
	}
};

genfi::ObjectStateUpdateGroup makeUpdate(tlc::TLCObjectType type,
                                         const std::string& id,
                                         const json& state) {
	genfi::ObjectStateUpdate upd;
	upd.objects.type = static_cast<int>(type);
	upd.objects.ids = {id};
	upd.states.push_back(state);
	genfi::ObjectStateUpdateGroup g;
	g.update.push_back(std::move(upd));
	return g;
}

// CIF_WUS[] / CIF_IS[] are process-global. Reset the slots and change flags the
// tests touch so each test starts from a known state.
class CifBridgeE2ETest : public ::testing::Test {
protected:
	void SetUp() override {
		for (int i = 0; i < 16; ++i) {
			CIF_WUS[i] = 0;
			CIF_IS[i] = 0;
		}
		CIF_WUSWIJZ = 0;
		CIF_ISWIJZ = 0;
		// CIF_IS_SWICO[] is CIF_AANT_IS_D + CIF_AANT_IS_OV (14) wide; stay in bounds.
		for (int i = 0; i < 14; ++i) {
			CIF_IS_SWICO[i] = CIF_IS_SWICO_GEEN;
		}
	}
};

} // namespace

TEST_F(CifBridgeE2ETest, SignalGroupGreenPopulatesCifWus) {
	TestableControllerApp app("127.0.0.1", "1", "u", "p", "uri");
	cif::CifBridge bridge(app);
	bridge.start();
	bridge.setSignalGroupCount(1);
	bridge.setSignalGroupMapping("SG1", 0);

	app.injectUpdate(makeUpdate(tlc::TLCObjectType::SignalGroup, "SG1",
		json{{"state", static_cast<int>(tlc::SignalGroupState::ProtectedMovementAllowed)}}));

	EXPECT_EQ(CIF_WUS[0], CIF_GROEN);
	EXPECT_EQ(CIF_WUSWIJZ, 1);
}

TEST_F(CifBridgeE2ETest, SignalGroupClearancePopulatesCifWusYellow) {
	TestableControllerApp app("127.0.0.1", "1", "u", "p", "uri");
	cif::CifBridge bridge(app);
	bridge.start();
	bridge.setSignalGroupCount(1);
	bridge.setSignalGroupMapping("SG1", 0);

	app.injectUpdate(makeUpdate(tlc::TLCObjectType::SignalGroup, "SG1",
		json{{"state", static_cast<int>(tlc::SignalGroupState::ProtectedClearance)}}));

	EXPECT_EQ(CIF_WUS[0], CIF_GEEL);
}

TEST_F(CifBridgeE2ETest, SignalGroupStopPopulatesCifWusRed) {
	TestableControllerApp app("127.0.0.1", "1", "u", "p", "uri");
	cif::CifBridge bridge(app);
	bridge.start();
	bridge.setSignalGroupCount(1);
	bridge.setSignalGroupMapping("SG1", 0);

	app.injectUpdate(makeUpdate(tlc::TLCObjectType::SignalGroup, "SG1",
		json{{"state", static_cast<int>(tlc::SignalGroupState::StopAndRemain)}}));

	EXPECT_EQ(CIF_WUS[0], CIF_ROOD);
}

TEST_F(CifBridgeE2ETest, UnmappedSignalGroupLeavesCifWusUntouched) {
	TestableControllerApp app("127.0.0.1", "1", "u", "p", "uri");
	cif::CifBridge bridge(app);
	bridge.start();
	// No mapping registered for "SGX".

	app.injectUpdate(makeUpdate(tlc::TLCObjectType::SignalGroup, "SGX",
		json{{"state", static_cast<int>(tlc::SignalGroupState::ProtectedMovementAllowed)}}));

	EXPECT_EQ(CIF_WUS[0], 0);
	EXPECT_EQ(CIF_WUSWIJZ, 0) << "an unmapped object must not flag CIF_WUS as changed";
}

TEST_F(CifBridgeE2ETest, OutputPopulatesCifWusAtSignalGroupOffset) {
	TestableControllerApp app("127.0.0.1", "1", "u", "p", "uri");
	cif::CifBridge bridge(app);
	bridge.start();
	bridge.setSignalGroupCount(2);   // outputs start at CIF_WUS[2]
	bridge.setOutputMapping("O1", 0);

	app.injectUpdate(makeUpdate(tlc::TLCObjectType::Output, "O1",
		json{{"state", 137}}));

	EXPECT_EQ(CIF_WUS[2], 137) << "output index must be offset by the signal-group count";
	EXPECT_EQ(CIF_WUSWIJZ, 1);
}

TEST_F(CifBridgeE2ETest, DetectorOccupiedPopulatesCifIs) {
	TestableControllerApp app("127.0.0.1", "1", "u", "p", "uri");
	cif::CifBridge bridge(app);
	bridge.start();
	bridge.setDetectorCount(1);
	bridge.setDetectorMapping("D1", 0);

	app.injectUpdate(makeUpdate(tlc::TLCObjectType::Detector, "D1",
		json{{"state", static_cast<int>(tlc::DetectorState::Occupied)}}));

	EXPECT_EQ(CIF_IS[0], CIF_DET_BEZET);
	EXPECT_EQ(CIF_ISWIJZ, 1);
}

TEST_F(CifBridgeE2ETest, DetectorUnoccupiedClearsCifIs) {
	TestableControllerApp app("127.0.0.1", "1", "u", "p", "uri");
	cif::CifBridge bridge(app);
	bridge.start();
	bridge.setDetectorCount(1);
	bridge.setDetectorMapping("D1", 0);
	CIF_IS[0] = CIF_DET_BEZET;  // pretend it was occupied

	app.injectUpdate(makeUpdate(tlc::TLCObjectType::Detector, "D1",
		json{{"state", static_cast<int>(tlc::DetectorState::Unoccupied)}}));

	EXPECT_EQ(CIF_IS[0], 0);
}

TEST_F(CifBridgeE2ETest, InputPopulatesCifIsAtDetectorOffset) {
	TestableControllerApp app("127.0.0.1", "1", "u", "p", "uri");
	cif::CifBridge bridge(app);
	bridge.start();
	bridge.setDetectorCount(3);   // inputs start at CIF_IS[3]
	bridge.setInputMapping("I1", 0);

	app.injectUpdate(makeUpdate(tlc::TLCObjectType::Input, "I1",
		json{{"state", 42}}));

	EXPECT_EQ(CIF_IS[3], 42) << "input index must be offset by the detector count";
	EXPECT_EQ(CIF_ISWIJZ, 1);
}

// --- SWICO propagation (TLC-FI -> CIF_IS_SWICO[]) ---
// A detector/input STATE update carrying a `swico` field must flow through the
// same path as `state` and land in CIF_IS_SWICO[] at the mapped index. This is
// the C-controller-visible SWICO channel; without it the controller always
// reads CIF_IS_SWICO_GEEN regardless of what the TLC reports.

TEST_F(CifBridgeE2ETest, DetectorSwicoPopulatesCifIsSwico) {
	TestableControllerApp app("127.0.0.1", "1", "u", "p", "uri");
	cif::CifBridge bridge(app);
	bridge.start();
	bridge.setDetectorCount(1);
	bridge.setDetectorMapping("D1", 0);

	app.injectUpdate(makeUpdate(tlc::TLCObjectType::Detector, "D1",
		json{{"state", static_cast<int>(tlc::DetectorState::Occupied)},
		     {"swico", static_cast<int>(tlc::SwicoState::SwicoOn)}}));

	EXPECT_EQ(CIF_IS[0], CIF_DET_BEZET);
	EXPECT_EQ(CIF_IS_SWICO[0], CIF_IS_SWICO_OP)
		<< "detector SWICO must propagate into CIF_IS_SWICO[] at the mapped index";
}

TEST_F(CifBridgeE2ETest, InputSwicoPopulatesCifIsSwicoAtDetectorOffset) {
	TestableControllerApp app("127.0.0.1", "1", "u", "p", "uri");
	cif::CifBridge bridge(app);
	bridge.start();
	bridge.setDetectorCount(3);   // inputs (and their SWICO) start at index 3
	bridge.setInputMapping("I1", 0);

	app.injectUpdate(makeUpdate(tlc::TLCObjectType::Input, "I1",
		json{{"state", 1},
		     {"swico", static_cast<int>(tlc::SwicoState::SwicoOff)}}));

	EXPECT_EQ(CIF_IS_SWICO[3], CIF_IS_SWICO_AF)
		<< "input SWICO index must be offset by the detector count, like CIF_IS[]";
}

TEST_F(CifBridgeE2ETest, DetectorSwicoClearsBackToNoSwico) {
	TestableControllerApp app("127.0.0.1", "1", "u", "p", "uri");
	cif::CifBridge bridge(app);
	bridge.start();
	bridge.setDetectorCount(1);
	bridge.setDetectorMapping("D1", 0);

	// First set a real SWICO via the update path, then clear it — this proves the
	// clear is driven by the incoming value, not by swico simply never being set.
	app.injectUpdate(makeUpdate(tlc::TLCObjectType::Detector, "D1",
		json{{"state", static_cast<int>(tlc::DetectorState::Occupied)},
		     {"swico", static_cast<int>(tlc::SwicoState::SwicoOn)}}));
	ASSERT_EQ(CIF_IS_SWICO[0], CIF_IS_SWICO_OP);

	app.injectUpdate(makeUpdate(tlc::TLCObjectType::Detector, "D1",
		json{{"state", static_cast<int>(tlc::DetectorState::Occupied)},
		     {"swico", static_cast<int>(tlc::SwicoState::NoSwico)}}));

	EXPECT_EQ(CIF_IS_SWICO[0], CIF_IS_SWICO_GEEN)
		<< "clearing SWICO on the TLC side must clear CIF_IS_SWICO[]";
}

TEST_F(CifBridgeE2ETest, UnmappedDetectorLeavesCifIsSwicoUntouched) {
	TestableControllerApp app("127.0.0.1", "1", "u", "p", "uri");
	cif::CifBridge bridge(app);
	bridge.start();
	// No mapping registered for "DX".

	app.injectUpdate(makeUpdate(tlc::TLCObjectType::Detector, "DX",
		json{{"state", static_cast<int>(tlc::DetectorState::Occupied)},
		     {"swico", static_cast<int>(tlc::SwicoState::SwicoOn)}}));

	EXPECT_EQ(CIF_IS_SWICO[0], CIF_IS_SWICO_GEEN)
		<< "an unmapped object must not write CIF_IS_SWICO[]";
}

TEST_F(CifBridgeE2ETest, AtomicGroupUpdatesMultipleObjects) {
	// A single UpdateState group carrying several object updates must populate
	// every mapped slot.
	TestableControllerApp app("127.0.0.1", "1", "u", "p", "uri");
	cif::CifBridge bridge(app);
	bridge.start();
	bridge.setSignalGroupCount(3);
	bridge.setSignalGroupMapping("SG1", 0);
	bridge.setSignalGroupMapping("SG2", 1);

	genfi::ObjectStateUpdate u;
	u.objects.type = static_cast<int>(tlc::TLCObjectType::SignalGroup);
	u.objects.ids = {"SG1", "SG2"};
	u.states.push_back(json{{"state", static_cast<int>(tlc::SignalGroupState::ProtectedMovementAllowed)}});
	u.states.push_back(json{{"state", static_cast<int>(tlc::SignalGroupState::ProtectedMovementAllowed)}});

	genfi::ObjectStateUpdateGroup g;
	g.update.push_back(std::move(u));
	app.injectUpdate(g);

	EXPECT_EQ(CIF_WUS[0], CIF_GROEN);
	EXPECT_EQ(CIF_WUS[1], CIF_GROEN);
}
