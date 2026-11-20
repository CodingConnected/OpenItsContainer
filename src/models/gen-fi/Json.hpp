#pragma once

#include "GenFiTypes.hpp"
#include "Enums.hpp"
#include "Structures.hpp"

namespace tlcfi::genfi {

void to_json(nlohmann::json& j, const ProtocolVersion& v);
void from_json(const nlohmann::json& j, ProtocolVersion& v);

void to_json(nlohmann::json& j, const Location& l);
void from_json(const nlohmann::json& j, Location& l);

void to_json(nlohmann::json& j, const ObjectReference& r);
void from_json(const nlohmann::json& j, ObjectReference& r);

void to_json(nlohmann::json& j, const AliveObject& a);
void from_json(const nlohmann::json& j, AliveObject& a);

void to_json(nlohmann::json& j, const ObjectMeta& v);
void from_json(const nlohmann::json& j, ObjectMeta& v);

void to_json(nlohmann::json& j, const ObjectData& v);
void from_json(const nlohmann::json& j, ObjectData& v);

void to_json(nlohmann::json& j, const ObjectStateUpdate& v);
void from_json(const nlohmann::json& j, ObjectStateUpdate& v);

void to_json(nlohmann::json& j, const ObjectStateUpdateGroup& v);
void from_json(const nlohmann::json& j, ObjectStateUpdateGroup& v);

void to_json(nlohmann::json& j, const NotifyEventGroup& v);
void from_json(const nlohmann::json& j, NotifyEventGroup& v);

void to_json(nlohmann::json& j, const SessionEventInformation& v);
void from_json(const nlohmann::json& j, SessionEventInformation& v);

void to_json(nlohmann::json& j, const SessionEvent& v);
void from_json(const nlohmann::json& j, SessionEvent& v);

void to_json(nlohmann::json& j, const RegistrationRequest& r);
void from_json(const nlohmann::json& j, RegistrationRequest& r);

void to_json(nlohmann::json& j, const RegistrationReply& r);
void from_json(const nlohmann::json& j, RegistrationReply& r);

} // namespace tlcfi::genfi


