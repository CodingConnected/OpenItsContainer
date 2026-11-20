#include "GenFiTypes.hpp"
#include "Enums.hpp"
#include "Structures.hpp"

namespace tlcfi::genfi {

// Nlohmann JSON conversions in the tlcfi::genfi namespace

void to_json(nlohmann::json& j, const ProtocolVersion& v) {
	j = nlohmann::json{{"major", v.major}, {"minor", v.minor}, {"revision", v.revision}};
}
void from_json(const nlohmann::json& j, ProtocolVersion& v) {
	j.at("major").get_to(v.major);
	j.at("minor").get_to(v.minor);
	j.at("revision").get_to(v.revision);
}

void to_json(nlohmann::json& j, const Location& l) {
	j = nlohmann::json{{"latitude", l.latitude}, {"longitude", l.longitude}};
	if (l.elevation.has_value()) j["elevation"] = *l.elevation; else j["elevation"] = nullptr;
}
void from_json(const nlohmann::json& j, Location& l) {
	j.at("latitude").get_to(l.latitude);
	j.at("longitude").get_to(l.longitude);
	if (j.contains("elevation") && !j["elevation"].is_null()) l.elevation = j["elevation"].get<double>(); else l.elevation.reset();
}

void to_json(nlohmann::json& j, const ObjectReference& r) {
	j = nlohmann::json::object();
	if (std::holds_alternative<int>(r.type)) {
		j["type"] = std::get<int>(r.type);
	} else {
		j["type"] = std::get<std::string>(r.type);
	}
	j["ids"] = r.ids;
}
void from_json(const nlohmann::json& j, ObjectReference& r) {
	const auto& t = j.at("type");
	if (t.is_number_integer()) r.type = t.get<int>();
	else r.type = t.get<std::string>();
	j.at("ids").get_to(r.ids);
}

void to_json(nlohmann::json& j, const AliveObject& a) {
	j = nlohmann::json{{"ticks", a.ticks}, {"time", a.time}};
}
void from_json(const nlohmann::json& j, AliveObject& a) {
	j.at("ticks").get_to(a.ticks);
	j.at("time").get_to(a.time);
}

void to_json(nlohmann::json& j, const ObjectMeta& v) {
	j = nlohmann::json::object();
	j["objects"] = v.objects;
	j["meta"] = v.meta;
	j["ticks"] = v.ticks;
}
void from_json(const nlohmann::json& j, ObjectMeta& v) {
	j.at("objects").get_to(v.objects);
	j.at("meta").get_to(v.meta);
	j.at("ticks").get_to(v.ticks);
}

void to_json(nlohmann::json& j, const RegistrationRequest& r) {
	j = nlohmann::json{{"username", r.username}, {"password", r.password}, {"type", r.type}, {"version", r.version}, {"uri", r.uri}};
}
void from_json(const nlohmann::json& j, RegistrationRequest& r) {
	j.at("username").get_to(r.username);
	j.at("password").get_to(r.password);
	j.at("type").get_to(r.type);
	j.at("version").get_to(r.version);
	j.at("uri").get_to(r.uri);
}

void to_json(nlohmann::json& j, const RegistrationReply& r) {
	j = nlohmann::json{{"sessionid", r.sessionid}, {"facilities", r.facilities}, {"version", r.version}};
}
void from_json(const nlohmann::json& j, RegistrationReply& r) {
	j.at("sessionid").get_to(r.sessionid);
	j.at("facilities").get_to(r.facilities);
	j.at("version").get_to(r.version);
}

void to_json(nlohmann::json& j, const ObjectData& v) {
	j = nlohmann::json::object();
	j["objects"] = v.objects;
	j["data"] = v.data;
	j["ticks"] = v.ticks;
}
void from_json(const nlohmann::json& j, ObjectData& v) {
	j.at("objects").get_to(v.objects);
	j.at("data").get_to(v.data);
	j.at("ticks").get_to(v.ticks);
}

void to_json(nlohmann::json& j, const ObjectStateUpdate& v) {
	j = nlohmann::json::object();
	j["objects"] = v.objects;
	j["states"] = v.states;
}
void from_json(const nlohmann::json& j, ObjectStateUpdate& v) {
	j.at("objects").get_to(v.objects);
	j.at("states").get_to(v.states);
}

void to_json(nlohmann::json& j, const ObjectStateUpdateGroup& v) {
	j = nlohmann::json::object();
	j["update"] = v.update;
	j["ticks"] = v.ticks;
}
void from_json(const nlohmann::json& j, ObjectStateUpdateGroup& v) {
	j.at("update").get_to(v.update);
	if (j.contains("ticks")) j.at("ticks").get_to(v.ticks);
}

void to_json(nlohmann::json& j, const NotifyEventGroup& v) {
	j = nlohmann::json::object();
	j["events"] = v.events;
	j["objects"] = v.objects;
	j["ticks"] = v.ticks;
}
void from_json(const nlohmann::json& j, NotifyEventGroup& v) {
	j.at("events").get_to(v.events);
	j.at("objects").get_to(v.objects);
	if (j.contains("ticks")) j.at("ticks").get_to(v.ticks);
}

void to_json(nlohmann::json& j, const SessionEventInformation& v) {
	j = nlohmann::json::object();
	j["type"] = v.type;
	j["id"] = v.id;
	j["attribute"] = v.attribute;
}
void from_json(const nlohmann::json& j, SessionEventInformation& v) {
	if (j.contains("type")) j.at("type").get_to(v.type);
	if (j.contains("id")) j.at("id").get_to(v.id);
	if (j.contains("attribute")) j.at("attribute").get_to(v.attribute);
}

void to_json(nlohmann::json& j, const SessionEvent& v) {
	j = nlohmann::json::object();
	j["code"] = v.code;
	if (v.info.has_value()) j["info"] = *v.info; else j["info"] = nullptr;
}
void from_json(const nlohmann::json& j, SessionEvent& v) {
	if (j.contains("code")) j.at("code").get_to(v.code);
	if (j.contains("info") && !j["info"].is_null()) v.info = j["info"].get<SessionEventInformation>(); else v.info.reset();
}

} // namespace tlcfi::genfi


