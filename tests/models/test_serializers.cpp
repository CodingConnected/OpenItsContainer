#include <gtest/gtest.h>

#include "models/gen-fi/Json.hpp"
#include "models/tlc-fi/Json.hpp"
#include "models/tlc-fi/Objects.hpp"
#include "models/tlc-fi/Types.hpp"

#include <nlohmann/json.hpp>

// Serializer round-trip tests for the gen-FI and TLC-FI model types.
//
// Round-trip strategy: the model structs have no operator==, so each test
// serializes an object, deserializes it, re-serializes the copy and asserts
// the two JSON documents are equal. This proves to_json/from_json are mutual
// inverses without needing struct equality.

using nlohmann::json;
namespace genfi = tlcfi::genfi;
namespace tlc = tlcfi::tlcfi;

namespace {

// Serialize -> deserialize -> re-serialize and require both JSON docs equal.
template <typename T>
void expectRoundTrip(const T& original) {
	json j1 = original;
	T copy = j1.get<T>();
	json j2 = copy;
	EXPECT_EQ(j1, j2);
}

} // namespace

// ============================================================================
// gen-FI types
// ============================================================================

TEST(GenFiSerializers, ProtocolVersionRoundTrip) {
	genfi::ProtocolVersion v;
	v.major = 2; v.minor = 0; v.revision = 1;
	expectRoundTrip(v);
}

TEST(GenFiSerializers, LocationRoundTripWithElevation) {
	genfi::Location l;
	l.latitude = 52.1; l.longitude = 4.3; l.elevation = 12.5;
	expectRoundTrip(l);
}

TEST(GenFiSerializers, LocationRoundTripWithoutElevation) {
	genfi::Location l;
	l.latitude = 52.1; l.longitude = 4.3;  // elevation left as nullopt
	expectRoundTrip(l);

	json j = l;
	EXPECT_TRUE(j["elevation"].is_null());
}

TEST(GenFiSerializers, ObjectReferenceIntTypeRoundTrip) {
	genfi::ObjectReference r;
	r.type = 3;  // integer enum form
	r.ids = {"SG1", "SG2"};
	expectRoundTrip(r);

	json j = r;
	EXPECT_TRUE(j["type"].is_number_integer());
}

TEST(GenFiSerializers, ObjectReferenceStringTypeRoundTrip) {
	genfi::ObjectReference r;
	r.type = std::string("Session");  // string type-name form
	r.ids = {"sess-1"};
	expectRoundTrip(r);

	json j = r;
	EXPECT_TRUE(j["type"].is_string());
}

TEST(GenFiSerializers, AliveObjectRoundTrip) {
	genfi::AliveObject a;
	a.ticks = 123456u;
	a.time = 1700000000000ull;
	expectRoundTrip(a);
}

TEST(GenFiSerializers, RegistrationRequestRoundTrip) {
	genfi::RegistrationRequest r;
	r.username = "ctrl"; r.password = "secret"; r.type = 2;
	r.version.major = 2; r.version.minor = 0; r.version.revision = 1;
	r.uri = "urn:test";
	expectRoundTrip(r);
}

TEST(GenFiSerializers, RegistrationReplyRoundTrip) {
	genfi::RegistrationReply r;
	r.sessionid = "sess-42";
	r.facilities.type = 0;
	r.facilities.ids = {"fac-1"};
	r.version.major = 2;
	expectRoundTrip(r);
}

TEST(GenFiSerializers, ObjectStateUpdateGroupRoundTrip) {
	genfi::ObjectStateUpdate upd;
	upd.objects.type = 3;
	upd.objects.ids = {"SG1"};
	upd.states.push_back(json{{"reqState", 6}});

	genfi::ObjectStateUpdateGroup g;
	g.update.push_back(upd);
	g.ticks = 999u;
	expectRoundTrip(g);
}

TEST(GenFiSerializers, ObjectStateUpdateGroupTicksOptionalOnParse) {
	// from_json must not throw when "ticks" is absent.
	json j = json{{"update", json::array()}};
	genfi::ObjectStateUpdateGroup g = j.get<genfi::ObjectStateUpdateGroup>();
	EXPECT_EQ(g.ticks, 0u);
}

TEST(GenFiSerializers, NotifyEventGroupRoundTrip) {
	genfi::NotifyEventGroup g;
	g.objects.type = 1;
	g.objects.ids = {"D01"};
	g.events.push_back(json{{"objectspeed", 12.0}});
	g.ticks = 7u;
	expectRoundTrip(g);
}

TEST(GenFiSerializers, ObjectMetaRoundTrip) {
	genfi::ObjectMeta m;
	m.objects.type = 3;
	m.objects.ids = {"SG1"};
	m.meta.push_back(json{{"id", "SG1"}});
	m.ticks = 5u;
	expectRoundTrip(m);
}

// ============================================================================
// TLC-FI types
// ============================================================================

TEST(TlcFiSerializers, ControlApplicationRoundTrip) {
	tlc::ControlApplication c;
	c.sessionid = "sess-1";
	c.type = 2;
	c.startCapability = tlc::HandoverCapability::PreDefined;
	c.endCapability = tlc::HandoverCapability::Direct;
	c.reqIntersection = "X1";
	c.reqControlState = tlc::ControlState::ReadyToControl;
	c.controlState = tlc::ControlState::Offline;
	expectRoundTrip(c);
}

TEST(TlcFiSerializers, ControlApplicationMinimalRoundTrip) {
	tlc::ControlApplication c;  // all optionals left empty
	c.sessionid = "sess-1";
	expectRoundTrip(c);

	json j = c;
	EXPECT_TRUE(j["reqIntersection"].is_null());
	EXPECT_TRUE(j["startCapability"].is_null());
}

TEST(TlcFiSerializers, DateTimeRoundTrip) {
	tlc::DateTime d;
	d.y = 2026; d.m = 5; d.d = 19; d.h = 14; d.min = 30; d.s = 0; d.ms = 500;
	expectRoundTrip(d);
}

TEST(TlcFiSerializers, SignalConflictRoundTrip) {
	tlc::SignalConflict s;
	s.signalgroup = "SG7";
	s.intergreentime = 45;
	expectRoundTrip(s);
}

TEST(TlcFiSerializers, SignalTimingRoundTrip) {
	tlc::SignalTiming s;
	s.state = tlc::SignalGroupState::ProtectedMovementAllowed;
	s.min = 30;
	s.max = 600;
	expectRoundTrip(s);
}

TEST(TlcFiSerializers, IntersectionRoundTrip) {
	tlc::Intersection x;
	x.id = "X1";
	x.outputs = {"O1", "O2"};
	x.inputs = {"I1"};
	x.signalgroups = {"SG1", "SG2"};
	x.detectors = {"D1"};
	x.spvehgenerator = "SPV1";
	x.stateticks = 100u;
	x.state = tlc::IntersectionControlState::Control;
	x.reqState = tlc::IntersectionControlState::Control;
	expectRoundTrip(x);
}

TEST(TlcFiSerializers, DetectorRoundTrip) {
	tlc::Detector d;
	d.id = "D1";
	d.generatesEvents = true;
	d.stateticks = 50u;
	d.state = tlc::DetectorState::Occupied;
	expectRoundTrip(d);
}

TEST(TlcFiSerializers, InputRoundTrip) {
	tlc::Input i;
	i.id = "I1";
	i.stateticks = 22u;
	i.state = 1;
	expectRoundTrip(i);
}

TEST(TlcFiSerializers, OutputRoundTrip) {
	tlc::Output o;
	o.id = "O1";
	o.intersection = "X1";
	o.stateticks = 11u;
	o.reqState = 123;
	o.state = 123;
	expectRoundTrip(o);
}

TEST(TlcFiSerializers, SignalGroupRoundTrip) {
	tlc::SignalGroup sg;
	sg.id = "SG1";
	sg.intersection = "X1";
	tlc::SignalConflict conf; conf.signalgroup = "SG2"; conf.intergreentime = 30;
	sg.intergreen.push_back(conf);
	sg.stateticks = 9u;
	sg.state = tlc::SignalGroupState::PermissiveMovementAllowed;
	sg.reqState = tlc::SignalGroupState::StopAndRemain;
	expectRoundTrip(sg);
}

TEST(TlcFiSerializers, DetectorEventRoundTrip) {
	tlc::DetectorEvent e;
	e.objectspeed = 13.5;
	e.objectlength = 4.2;
	e.objectheight = 1.8;
	e.classification = tlc::DetectorClassification::Unknown;
	expectRoundTrip(e);
}

TEST(TlcFiSerializers, SpecialVehicleEventRoundTrip) {
	tlc::SpecialVehicleEvent e;
	e.lineNr = 12;
	e.companyNr = 3;
	e.priorityClass = tlc::PriorityClass::Conditional;
	e.speed = 8.0;
	expectRoundTrip(e);
}

TEST(TlcFiSerializers, SpecialVehicleEventEmptyRoundTrip) {
	tlc::SpecialVehicleEvent e;  // every field nullopt
	expectRoundTrip(e);
}

// ============================================================================
// Tolerant / partial deserialization (Phase 3.3)
// ============================================================================

TEST(TolerantDeserialization, PartialSignalGroupLeavesAbsentFieldsUnchanged) {
	// A partial UpdateState carrying only "state" must not throw and must
	// leave id / intersection untouched.
	tlc::SignalGroup sg;
	sg.id = "SG1";
	sg.intersection = "X1";
	sg.state = tlc::SignalGroupState::Unavailable;

	json partial = json{{"state", static_cast<int>(tlc::SignalGroupState::ProtectedMovementAllowed)}};
	tlc::from_json(partial, sg);

	EXPECT_EQ(sg.id, "SG1");
	EXPECT_EQ(sg.intersection, "X1");
	EXPECT_EQ(sg.state, tlc::SignalGroupState::ProtectedMovementAllowed);
}

TEST(TolerantDeserialization, PartialDetectorWithFaultstateOnly) {
	tlc::Detector d;
	d.id = "D1";
	json partial = json{{"faultstate", 1}};
	tlc::from_json(partial, d);  // must not throw
	EXPECT_EQ(d.id, "D1");
}

TEST(TolerantDeserialization, PresentButNullClearsOutputOptional) {
	tlc::Output o;
	o.reqState = 99;
	json j = json{{"reqState", nullptr}};
	tlc::from_json(j, o);
	EXPECT_FALSE(o.reqState.has_value());
}

TEST(TolerantDeserialization, EmptyObjectLeavesIntersectionUnchanged) {
	tlc::Intersection x;
	x.id = "X1";
	x.state = tlc::IntersectionControlState::Control;
	tlc::from_json(json::object(), x);  // must not throw
	EXPECT_EQ(x.id, "X1");
	EXPECT_EQ(x.state, tlc::IntersectionControlState::Control);
}

// ============================================================================
// Malformed-input handling
// ============================================================================

TEST(MalformedInput, RegistrationRequestMissingFieldThrows) {
	// gen-FI RegistrationRequest uses j.at(): a missing required field throws.
	json j = json{{"username", "u"}};  // missing password/type/version/uri
	genfi::RegistrationRequest r;
	EXPECT_THROW(genfi::from_json(j, r), json::exception);
}

TEST(MalformedInput, SignalConflictMissingFieldThrows) {
	json j = json{{"signalgroup", "SG1"}};  // missing intergreentime
	tlc::SignalConflict s;
	EXPECT_THROW(tlc::from_json(j, s), json::exception);
}

TEST(MalformedInput, DetectorEventMissingFieldThrows) {
	json j = json{{"objectspeed", 1.0}};  // missing objectlength/classification/direction
	tlc::DetectorEvent e;
	EXPECT_THROW(tlc::from_json(j, e), json::exception);
}
