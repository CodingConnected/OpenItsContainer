#include "Objects.hpp"
#include "FacilitiesInfo.hpp"
#include "../gen-fi/Json.hpp"

namespace tlcfi::tlcfi {

// DateTime (from Types.hpp)
void to_json(nlohmann::json& j, const ControlApplication& v) {
	j = nlohmann::json{};
	// META
	j["sessionid"] = v.sessionid;
	j["type"] = v.type;
	// STATE
	if (v.startCapability) j["startCapability"] = static_cast<int>(*v.startCapability); else j["startCapability"] = nullptr;
	if (v.endCapability) j["endCapability"] = static_cast<int>(*v.endCapability); else j["endCapability"] = nullptr;
	if (v.reqIntersection) j["reqIntersection"] = *v.reqIntersection; else j["reqIntersection"] = nullptr;
	if (v.reqControlState) j["reqControlState"] = static_cast<int>(*v.reqControlState); else j["reqControlState"] = nullptr;
	j["controlState"] = static_cast<int>(v.controlState);
	if (v.reqHandover) j["reqHandover"] = static_cast<int>(*v.reqHandover); else j["reqHandover"] = nullptr;
}
void from_json(const nlohmann::json& j, ControlApplication& v) {
	if (j.contains("sessionid")) j.at("sessionid").get_to(v.sessionid);
	if (j.contains("type")) j.at("type").get_to(v.type);
	if (j.contains("startCapability") && !j["startCapability"].is_null()) { int tmp{}; j["startCapability"].get_to(tmp); v.startCapability = static_cast<HandoverCapability>(tmp);} else v.startCapability.reset();
	if (j.contains("endCapability") && !j["endCapability"].is_null()) { int tmp{}; j["endCapability"].get_to(tmp); v.endCapability = static_cast<HandoverCapability>(tmp);} else v.endCapability.reset();
	if (j.contains("reqIntersection") && !j["reqIntersection"].is_null()) v.reqIntersection = j["reqIntersection"].get<std::string>(); else v.reqIntersection.reset();
	if (j.contains("reqControlState") && !j["reqControlState"].is_null()) { int tmp{}; j["reqControlState"].get_to(tmp); v.reqControlState = static_cast<ControlState>(tmp);} else v.reqControlState.reset();
	if (j.contains("controlState")) { int tmp{}; j["controlState"].get_to(tmp); v.controlState = static_cast<ControlState>(tmp);} 
	if (j.contains("reqHandover") && !j["reqHandover"].is_null()) { int tmp{}; j["reqHandover"].get_to(tmp); v.reqHandover = static_cast<HandoverCapability>(tmp);} else v.reqHandover.reset();
}
void to_json(nlohmann::json& j, const DateTime& v) {
	j = nlohmann::json{};
	if (v.y) j["y"] = *v.y; else j["y"] = nullptr;
	if (v.m) j["m"] = *v.m; else j["m"] = nullptr;
	if (v.d) j["d"] = *v.d; else j["d"] = nullptr;
	if (v.h) j["h"] = *v.h; else j["h"] = nullptr;
	if (v.min) j["min"] = *v.min; else j["min"] = nullptr;
	if (v.s) j["s"] = *v.s; else j["s"] = nullptr;
	if (v.ms) j["ms"] = *v.ms; else j["ms"] = nullptr;
}
void from_json(const nlohmann::json& j, DateTime& v) {
	v.y = (j.contains("y") && !j["y"].is_null()) ? std::optional<int>(j["y"].get<int>()) : std::nullopt;
	v.m = (j.contains("m") && !j["m"].is_null()) ? std::optional<int>(j["m"].get<int>()) : std::nullopt;
	v.d = (j.contains("d") && !j["d"].is_null()) ? std::optional<int>(j["d"].get<int>()) : std::nullopt;
	v.h = (j.contains("h") && !j["h"].is_null()) ? std::optional<int>(j["h"].get<int>()) : std::nullopt;
	v.min = (j.contains("min") && !j["min"].is_null()) ? std::optional<int>(j["min"].get<int>()) : std::nullopt;
	v.s = (j.contains("s") && !j["s"].is_null()) ? std::optional<int>(j["s"].get<int>()) : std::nullopt;
	v.ms = (j.contains("ms") && !j["ms"].is_null()) ? std::optional<int>(j["ms"].get<int>()) : std::nullopt;
}

void to_json(nlohmann::json& j, const SignalConflict& v) {
	j = nlohmann::json{{"signalgroup", v.signalgroup}, {"intergreentime", v.intergreentime}};
}
void from_json(const nlohmann::json& j, SignalConflict& v) {
	j.at("signalgroup").get_to(v.signalgroup);
	j.at("intergreentime").get_to(v.intergreentime);
}

void to_json(nlohmann::json& j, const SignalTiming& v) {
	j = nlohmann::json{{"state", static_cast<int>(v.state)}, {"min", v.min.has_value() ? nlohmann::json(*v.min) : nlohmann::json(nullptr)}, {"max", v.max.has_value() ? nlohmann::json(*v.max) : nlohmann::json(nullptr)}};
}
void from_json(const nlohmann::json& j, SignalTiming& v) {
	int s{}; j.at("state").get_to(s); v.state = static_cast<SignalGroupState>(s);
	if (j.contains("min") && !j["min"].is_null()) v.min = j["min"].get<int>(); else v.min.reset();
	if (j.contains("max") && !j["max"].is_null()) v.max = j["max"].get<int>(); else v.max.reset();
}

#if defined(TLCFI_VERSION_1_1_0)
void to_json(nlohmann::json& j, const SignalGroupPrediction& v) {
	j = nlohmann::json{{"state", static_cast<int>(v.state)}, {"startTime", v.startTime.value_or(0)}, {"minEnd", v.minEnd}};
	if (v.maxEnd.has_value()) j["maxEnd"] = *v.maxEnd; else j["maxEnd"] = nullptr;
	if (v.likelyEnd.has_value()) j["likelyEnd"] = *v.likelyEnd; else j["likelyEnd"] = nullptr;
	if (v.confidence.has_value()) j["confidence"] = *v.confidence; else j["confidence"] = nullptr;
	if (v.next.has_value()) j["next"] = *v.next; else j["next"] = nullptr;
}
void from_json(const nlohmann::json& j, SignalGroupPrediction& v) {
	int s{}; j.at("state").get_to(s); v.state = static_cast<SignalGroupState>(s);
	if (j.contains("startTime") && !j["startTime"].is_null()) v.startTime = j["startTime"].get<genfi::Ticks>(); else v.startTime.reset();
	j.at("minEnd").get_to(v.minEnd);
	if (j.contains("maxEnd") && !j["maxEnd"].is_null()) v.maxEnd = j["maxEnd"].get<genfi::Ticks>(); else v.maxEnd.reset();
	if (j.contains("likelyEnd") && !j["likelyEnd"].is_null()) v.likelyEnd = j["likelyEnd"].get<genfi::Ticks>(); else v.likelyEnd.reset();
	if (j.contains("confidence") && !j["confidence"].is_null()) v.confidence = j["confidence"].get<int>(); else v.confidence.reset();
	if (j.contains("next") && !j["next"].is_null()) v.next = j["next"].get<genfi::Ticks>(); else v.next.reset();
}
#endif // TLCFI_VERSION_1_1_0

void to_json(nlohmann::json& j, const Intersection& v) {
	j = nlohmann::json{{"id", v.id}, {"outputs", v.outputs}, {"inputs", v.inputs}, {"signalgroups", v.signalgroups}, {"detectors", v.detectors}, {"stateticks", v.stateticks}, {"state", static_cast<int>(v.state)}};
	if (v.spvehgenerator.has_value()) j["spvehgenerator"] = *v.spvehgenerator; else j["spvehgenerator"] = nullptr;
	if (v.reqState.has_value()) j["reqState"] = static_cast<int>(*v.reqState); else j["reqState"] = nullptr;
}
// All object from_json functions tolerate partial JSON: a field absent from
// the object leaves the corresponding struct member unchanged (no throw), so
// partial UpdateState notifications are applied rather than silently dropped.
// A field present-but-null clears an optional member.
void from_json(const nlohmann::json& j, Intersection& v) {
	if (j.contains("id")) j.at("id").get_to(v.id);
	if (j.contains("outputs")) j.at("outputs").get_to(v.outputs);
	if (j.contains("inputs")) j.at("inputs").get_to(v.inputs);
	if (j.contains("signalgroups")) j.at("signalgroups").get_to(v.signalgroups);
	if (j.contains("detectors")) j.at("detectors").get_to(v.detectors);
	if (j.contains("spvehgenerator")) { if (j["spvehgenerator"].is_null()) v.spvehgenerator.reset(); else v.spvehgenerator = j["spvehgenerator"].get<std::string>(); }
	if (j.contains("stateticks")) j.at("stateticks").get_to(v.stateticks);
	if (j.contains("state")) { int s{}; j.at("state").get_to(s); v.state = static_cast<IntersectionControlState>(s); }
	if (j.contains("reqState")) { if (j["reqState"].is_null()) v.reqState.reset(); else { int tmp{}; j["reqState"].get_to(tmp); v.reqState = static_cast<IntersectionControlState>(tmp); } }
}

void to_json(nlohmann::json& j, const Detector& v) {
	j = nlohmann::json{{"id", v.id}, {"generatesEvents", v.generatesEvents}, {"stateticks", v.stateticks}, {"state", static_cast<int>(v.state)}, {"faultstate", static_cast<int>(v.faultstate)}, {"swico", static_cast<int>(v.swico)}};
}
void from_json(const nlohmann::json& j, Detector& v) {
	if (j.contains("id")) j.at("id").get_to(v.id);
	if (j.contains("generatesEvents")) j.at("generatesEvents").get_to(v.generatesEvents);
	if (j.contains("stateticks")) j.at("stateticks").get_to(v.stateticks);
	if (j.contains("state")) { int s{}; j.at("state").get_to(s); v.state = static_cast<DetectorState>(s); }
	if (j.contains("faultstate")) { int f{}; j.at("faultstate").get_to(f); v.faultstate = static_cast<DetectorFaultState>(f); }
	if (j.contains("swico")) { int w{}; j.at("swico").get_to(w); v.swico = static_cast<SwicoState>(w); }
}

void to_json(nlohmann::json& j, const Input& v) {
	j = nlohmann::json{{"id", v.id}, {"stateticks", v.stateticks}, {"state", v.state}, {"faultstate", static_cast<int>(v.faultstate)}, {"swico", static_cast<int>(v.swico)}};
}
void from_json(const nlohmann::json& j, Input& v) {
	if (j.contains("id")) j.at("id").get_to(v.id);
	if (j.contains("stateticks")) j.at("stateticks").get_to(v.stateticks);
	if (j.contains("state")) j.at("state").get_to(v.state);
	if (j.contains("faultstate")) { int f{}; j.at("faultstate").get_to(f); v.faultstate = static_cast<InputFaultState>(f); }
	if (j.contains("swico")) { int w{}; j.at("swico").get_to(w); v.swico = static_cast<SwicoState>(w); }
}

void to_json(nlohmann::json& j, const Output& v) {
	j = nlohmann::json{{"id", v.id}, {"intersection", v.intersection}, {"stateticks", v.stateticks}};
	if (v.reqState.has_value()) j["reqState"] = *v.reqState; else j["reqState"] = nullptr;
	if (v.state.has_value()) j["state"] = *v.state; else j["state"] = nullptr;
	j["faultstate"] = static_cast<int>(v.faultstate);
}
void from_json(const nlohmann::json& j, Output& v) {
	if (j.contains("id")) j.at("id").get_to(v.id);
	if (j.contains("intersection")) j.at("intersection").get_to(v.intersection);
	if (j.contains("stateticks")) j.at("stateticks").get_to(v.stateticks);
	if (j.contains("reqState")) { if (j["reqState"].is_null()) v.reqState.reset(); else v.reqState = j["reqState"].get<int>(); }
	if (j.contains("state")) { if (j["state"].is_null()) v.state.reset(); else v.state = j["state"].get<int>(); }
	if (j.contains("faultstate")) { int f{}; j.at("faultstate").get_to(f); v.faultstate = static_cast<OutputFaultState>(f); }
}

void to_json(nlohmann::json& j, const SignalGroup& v) {
	j = nlohmann::json{{"id", v.id}, {"intersection", v.intersection}, {"intergreen", v.intergreen}, {"timing", v.timing}, {"stateticks", v.stateticks}, {"state", static_cast<int>(v.state)}};
	if (v.reqState.has_value()) j["reqState"] = static_cast<int>(*v.reqState); else j["reqState"] = nullptr;
#if defined(TLCFI_VERSION_1_1_0)
	j["predictions"] = v.predictions;
	j["reqPredictions"] = v.reqPredictions;
#endif // TLCFI_VERSION_1_1_0
}
void from_json(const nlohmann::json& j, SignalGroup& v) {
	if (j.contains("id")) j.at("id").get_to(v.id);
	if (j.contains("intersection")) j.at("intersection").get_to(v.intersection);
	if (j.contains("intergreen")) j.at("intergreen").get_to(v.intergreen);
	if (j.contains("timing")) j.at("timing").get_to(v.timing);
	if (j.contains("stateticks")) j.at("stateticks").get_to(v.stateticks);
	if (j.contains("state")) { int s{}; j.at("state").get_to(s); v.state = static_cast<SignalGroupState>(s); }
	if (j.contains("reqState")) { if (j["reqState"].is_null()) v.reqState.reset(); else { int rs{}; j["reqState"].get_to(rs); v.reqState = static_cast<SignalGroupState>(rs); } }
#if defined(TLCFI_VERSION_1_1_0)
	if (j.contains("reqPredictions")) j.at("reqPredictions").get_to(v.reqPredictions);
	if (j.contains("predictions")) j.at("predictions").get_to(v.predictions);
#endif // TLCFI_VERSION_1_1_0
}

void to_json(nlohmann::json& j, const DetectorEvent& v) {
	j = nlohmann::json{{"objectspeed", v.objectspeed}, {"objectlength", v.objectlength}};
	if (v.objectheight.has_value()) j["objectheight"] = *v.objectheight; else j["objectheight"] = nullptr;
	if (v.objectwidth.has_value()) j["objectwidth"] = *v.objectwidth; else j["objectwidth"] = nullptr;
	j["classification"] = static_cast<int>(v.classification);
	j["direction"] = static_cast<int>(v.direction);
}
void from_json(const nlohmann::json& j, DetectorEvent& v) {
	j.at("objectspeed").get_to(v.objectspeed);
	j.at("objectlength").get_to(v.objectlength);
	if (j.contains("objectheight") && !j["objectheight"].is_null()) v.objectheight = j["objectheight"].get<genfi::Length>(); else v.objectheight.reset();
	if (j.contains("objectwidth") && !j["objectwidth"].is_null()) v.objectwidth = j["objectwidth"].get<genfi::Length>(); else v.objectwidth.reset();
	int c{}; j.at("classification").get_to(c); v.classification = static_cast<DetectorClassification>(c);
	int d{}; j.at("direction").get_to(d); v.direction = static_cast<DetectorDirection>(d);
}

void to_json(nlohmann::json& j, const SpecialVehicleEvent& v) {
	j = nlohmann::json{};
	#define SET_OPT(name) j[ #name ] = (v.name ? nlohmann::json(*v.name) : nlohmann::json(nullptr))
	SET_OPT(virtualLoop);
	SET_OPT(vehType);
	SET_OPT(lineNr);
	SET_OPT(serviceNr);
	SET_OPT(companyNr);
	SET_OPT(vehId);
	SET_OPT(directionSG);
	SET_OPT(status);
	if (v.priorityClass) j["priorityClass"] = static_cast<int>(*v.priorityClass); else j["priorityClass"] = nullptr;
	if (v.punctuality) j["punctuality"] = static_cast<int>(*v.punctuality); else j["punctuality"] = nullptr;
	SET_OPT(punctualityTime);
	SET_OPT(length);
	SET_OPT(speed);
	SET_OPT(distToStopLine);
	SET_OPT(timeToStopLine);
	SET_OPT(journeyNr);
	SET_OPT(journeyCat);
	SET_OPT(routePT);
	if (v.type) j["type"] = static_cast<int>(*v.type); else j["type"] = nullptr;
	SET_OPT(activationPointNr);
	if (v.location) j["location"] = *v.location; else j["location"] = nullptr;
	if (v.dateTime) j["dateTime"] = *v.dateTime; else j["dateTime"] = nullptr;
	SET_OPT(reserve23);
	SET_OPT(reserve24);
	#undef SET_OPT
}
void from_json(const nlohmann::json& j, SpecialVehicleEvent& v) {
	if (j.contains("virtualLoop") && !j["virtualLoop"].is_null()) v.virtualLoop = j["virtualLoop"].get<int>(); else v.virtualLoop.reset();
	if (j.contains("vehType") && !j["vehType"].is_null()) v.vehType = j["vehType"].get<int>(); else v.vehType.reset();
	if (j.contains("lineNr") && !j["lineNr"].is_null()) v.lineNr = j["lineNr"].get<LineNumber>(); else v.lineNr.reset();
	if (j.contains("serviceNr") && !j["serviceNr"].is_null()) v.serviceNr = j["serviceNr"].get<ServiceNumber>(); else v.serviceNr.reset();
	if (j.contains("companyNr") && !j["companyNr"].is_null()) v.companyNr = j["companyNr"].get<CompanyNumber>(); else v.companyNr.reset();
	if (j.contains("vehId") && !j["vehId"].is_null()) v.vehId = j["vehId"].get<VehicleId>(); else v.vehId.reset();
	if (j.contains("directionSG") && !j["directionSG"].is_null()) v.directionSG = j["directionSG"].get<DirectionSG>(); else v.directionSG.reset();
	if (j.contains("status") && !j["status"].is_null()) v.status = j["status"].get<int>(); else v.status.reset();
	if (j.contains("priorityClass") && !j["priorityClass"].is_null()) { int tmp{}; j["priorityClass"].get_to(tmp); v.priorityClass = static_cast<PriorityClass>(tmp);} else v.priorityClass.reset();
	if (j.contains("punctuality") && !j["punctuality"].is_null()) { int tmp{}; j["punctuality"].get_to(tmp); v.punctuality = static_cast<PunctualityClass>(tmp);} else v.punctuality.reset();
	if (j.contains("punctualityTime") && !j["punctualityTime"].is_null()) v.punctualityTime = j["punctualityTime"].get<PunctualityTime>(); else v.punctualityTime.reset();
	if (j.contains("length") && !j["length"].is_null()) v.length = j["length"].get<genfi::Length>(); else v.length.reset();
	if (j.contains("speed") && !j["speed"].is_null()) v.speed = j["speed"].get<genfi::Speed>(); else v.speed.reset();
	if (j.contains("distToStopLine") && !j["distToStopLine"].is_null()) v.distToStopLine = j["distToStopLine"].get<DistanceToStopline>(); else v.distToStopLine.reset();
	if (j.contains("timeToStopLine") && !j["timeToStopLine"].is_null()) v.timeToStopLine = j["timeToStopLine"].get<int>(); else v.timeToStopLine.reset();
	if (j.contains("journeyNr") && !j["journeyNr"].is_null()) v.journeyNr = j["journeyNr"].get<JourneyNumber>(); else v.journeyNr.reset();
	if (j.contains("journeyCat") && !j["journeyCat"].is_null()) v.journeyCat = j["journeyCat"].get<JourneyCategory>(); else v.journeyCat.reset();
	if (j.contains("routePT") && !j["routePT"].is_null()) v.routePT = j["routePT"].get<RoutePublicTransport>(); else v.routePT.reset();
	if (j.contains("type") && !j["type"].is_null()) { int tmp{}; j["type"].get_to(tmp); v.type = static_cast<AnnouncementType>(tmp);} else v.type.reset();
	if (j.contains("activationPointNr") && !j["activationPointNr"].is_null()) v.activationPointNr = j["activationPointNr"].get<ActivationPointNr>(); else v.activationPointNr.reset();
	if (j.contains("location") && !j["location"].is_null()) v.location = j["location"].get<genfi::Location>(); else v.location.reset();
	if (j.contains("dateTime") && !j["dateTime"].is_null()) v.dateTime = j["dateTime"].get<DateTime>(); else v.dateTime.reset();
	if (j.contains("reserve23") && !j["reserve23"].is_null()) v.reserve23 = j["reserve23"].get<int>(); else v.reserve23.reset();
	if (j.contains("reserve24") && !j["reserve24"].is_null()) v.reserve24 = j["reserve24"].get<int>(); else v.reserve24.reset();
}

} // namespace tlcfi::tlcfi

// ProductVersion and FacilitiesInformation are in namespace tlcfi (not tlcfi::tlcfi)
namespace tlcfi {

// ProductVersion (v2.0.1)
void to_json(nlohmann::json& j, const ProductVersion& v) {
	j = nlohmann::json{{"major", v.major}, {"minor", v.minor}, {"revision", v.revision}};
}
void from_json(const nlohmann::json& j, ProductVersion& v) {
	j.at("major").get_to(v.major);
	j.at("minor").get_to(v.minor);
	j.at("revision").get_to(v.revision);
}

// FacilitiesInformation (version-specific)
void to_json(nlohmann::json& j, const FacilitiesInformation& v) {
	j = nlohmann::json{{"companyname", v.companyname}};

#if defined(TLCFI_VERSION_2_0_1)
	j["certifiedName"] = v.certifiedName;
	j["certifiedVersion"] = v.certifiedVersion;
	j["version"] = v.version;
#elif defined(TLCFI_VERSION_1_1_0)
	j["fiVersion"] = v.fiVersion;
	j["facilitiesVersion"] = v.facilitiesVersion;
#endif
}
void from_json(const nlohmann::json& j, FacilitiesInformation& v) {
	j.at("companyname").get_to(v.companyname);

#if defined(TLCFI_VERSION_2_0_1)
	if (j.contains("certifiedName")) j.at("certifiedName").get_to(v.certifiedName);
	if (j.contains("certifiedVersion")) j.at("certifiedVersion").get_to(v.certifiedVersion);
	if (j.contains("version")) j.at("version").get_to(v.version);
#elif defined(TLCFI_VERSION_1_1_0)
	if (j.contains("fiVersion")) j.at("fiVersion").get_to(v.fiVersion);
	if (j.contains("facilitiesVersion")) j.at("facilitiesVersion").get_to(v.facilitiesVersion);
#endif
}

} // namespace tlcfi


