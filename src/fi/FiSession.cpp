#include "FiSession.hpp"
#include <iostream>

namespace tlcfi::fi {

FiSession::FiSession(std::shared_ptr<jsonrpc::JsonRpcSession> rpc)
	: rpc_(std::move(rpc)) {}

// Ensure we can respond to Facilities-initiated Alive calls
// by echoing the AliveObject back (per GEN-FI 7.3 Alive)
// This registers when start() is called to ensure rpc_ exists.

void FiSession::start() {
    rpc_->registerHandler("Alive", [](nlohmann::json params) -> nlohmann::json {
        genfi::AliveObject a = params.get<genfi::AliveObject>();
        return nlohmann::json(a);
    });
    rpc_->start();
}

void FiSession::stop() { rpc_->stop(); }

jsonrpc::JsonRpcEvents& FiSession::events() { return rpc_->events; }

std::future<genfi::RegistrationReply> FiSession::registerApp(const genfi::RegistrationRequest& req,
	std::chrono::milliseconds timeout) {
	nlohmann::json params = req;
	auto fut = rpc_->sendRequest("Register", params, timeout);
	return std::async(std::launch::async, [this, f = std::move(fut)]() mutable {
		auto j = f.get();
		auto rep = j.get<genfi::RegistrationReply>();
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			facilitiesRef_ = rep.facilities;
			sessionId_ = rep.sessionid;
		}
		return rep;
	});
}

std::future<void> FiSession::deregisterApp(std::chrono::milliseconds timeout) {
	nlohmann::json params = nlohmann::json::object();
	auto fut = rpc_->sendRequest("Deregister", params, timeout);
	return std::async(std::launch::async, [f = std::move(fut)]() mutable {
		(void)f.get();
	});
}

std::future<genfi::AliveObject> FiSession::alive(const genfi::AliveObject& a, std::chrono::milliseconds timeout) {
	nlohmann::json params = a;
	auto fut = rpc_->sendRequest("Alive", params, timeout);
	return std::async(std::launch::async, [f = std::move(fut)]() mutable {
		auto j = f.get();
		return j.get<genfi::AliveObject>();
	});
}

std::future<genfi::ObjectMeta> FiSession::readMeta(const genfi::ObjectReference& objects,
    std::chrono::milliseconds timeout) {
	nlohmann::json params = nlohmann::json::object();
	// TLC-FI 6.4 ReadMeta expects flat params: type and ids
	if (std::holds_alternative<int>(objects.type)) {
		params["type"] = std::get<int>(objects.type);
	} else {
		params["type"] = std::get<std::string>(objects.type);
	}
	params["ids"] = objects.ids;
	auto fut = rpc_->sendRequest("ReadMeta", params, timeout);
	return std::async(std::launch::async, [f = std::move(fut)]() mutable {
		auto j = f.get();
		return j.get<genfi::ObjectMeta>();
	});
}

std::future<genfi::ObjectMeta> FiSession::readMeta(std::chrono::milliseconds timeout) {
	genfi::ObjectReference ref;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		if (!facilitiesRef_) {
			return std::async(std::launch::deferred, [](){ throw std::runtime_error("No facilities reference stored (not registered yet)"); return genfi::ObjectMeta{}; });
		}
		ref = *facilitiesRef_;
	}
	return readMeta(ref, timeout);
}

std::future<genfi::ObjectData> FiSession::subscribe(const genfi::ObjectReference& objects,
    std::chrono::milliseconds timeout) {
	nlohmann::json params = nlohmann::json::object();
	if (std::holds_alternative<int>(objects.type)) params["type"] = std::get<int>(objects.type);
	else params["type"] = std::get<std::string>(objects.type);
	params["ids"] = objects.ids;
	auto fut = rpc_->sendRequest("Subscribe", params, timeout);
	return std::async(std::launch::async, [f = std::move(fut)]() mutable {
		auto j = f.get();
		return j.get<genfi::ObjectData>();
	});
}

bool FiSession::updateState(const genfi::ObjectStateUpdateGroup& group) noexcept {
	try {
		// As client, UpdateState is a notification per spec.
		rpc_->sendNotification("UpdateState", nlohmann::json(group));
		return true;
	} catch (...) {
		// A disconnect can race a fire-and-forget control update. Let the
		// connection lifecycle report the failure and recover instead of
		// allowing an exception to terminate the controller worker thread.
		return false;
	}
}

void FiSession::control_setReqState(::tlcfi::tlcfi::ControlState state,
										const std::string& intersectionId,
										std::optional<::tlcfi::tlcfi::HandoverCapability> startCap,
										std::optional<::tlcfi::tlcfi::HandoverCapability> endCap) {
	std::string sess;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		if (!sessionId_) throw std::runtime_error("No session id (not registered)");
		sess = *sessionId_;
	}
	genfi::ObjectStateUpdate upd;
	upd.objects.type = static_cast<int>(::tlcfi::tlcfi::TLCObjectType::Session);
	upd.objects.ids = { sess };

	// Create a minimal JSON object with only the writable fields
	// (META fields like sessionid and type are read-only and should not be included)
	nlohmann::json ca = nlohmann::json::object();
	if (intersectionId.empty()) {
		ca["reqIntersection"] = nullptr;
	} else {
		ca["reqIntersection"] = intersectionId;
	}
	ca["reqControlState"] = static_cast<int>(state);
	if (startCap) ca["startCapability"] = static_cast<int>(*startCap); else ca["startCapability"] = nullptr;
	if (endCap) ca["endCapability"] = static_cast<int>(*endCap); else ca["endCapability"] = nullptr;

	upd.states.push_back(nlohmann::json(ca));

	genfi::ObjectStateUpdateGroup group;
	group.update.push_back(std::move(upd));
	updateState(group);
}

void FiSession::intersection_setReqState(const std::string& intersectionId,
											  ::tlcfi::tlcfi::IntersectionControlState state) {
	genfi::ObjectStateUpdate upd;
	upd.objects.type = static_cast<int>(::tlcfi::tlcfi::TLCObjectType::Intersection);
	upd.objects.ids = { intersectionId };
	::tlcfi::tlcfi::Intersection i;
	i.id = intersectionId;
	i.reqState = state;
	upd.states.push_back(nlohmann::json(i));
	genfi::ObjectStateUpdateGroup group;
	group.update.push_back(std::move(upd));
	updateState(group);
}

void FiSession::control_ackStartAndSetIntersectionControl(const std::string& intersectionId) {
	// 1) Set ControlApplication.reqControlState = InControl (acknowledge StartControl)
	control_setReqState(::tlcfi::tlcfi::ControlState::InControl, intersectionId);
	// 2) Request Intersection Control state
	intersection_setReqState(intersectionId, ::tlcfi::tlcfi::IntersectionControlState::Control);
}

void FiSession::signalgroups_setReqStates(const std::vector<std::pair<std::string, ::tlcfi::tlcfi::SignalGroupState>>& updates) {
	if (updates.empty()) return;
	genfi::ObjectStateUpdate upd;
	upd.objects.type = static_cast<int>(::tlcfi::tlcfi::TLCObjectType::SignalGroup);
	upd.objects.ids.reserve(updates.size());
	for (const auto& [id, state] : updates) {
		upd.objects.ids.push_back(id);
	}
	for (const auto& [id, state] : updates) {
		// Only the writable reqState attribute is part of the update; META and
		// read-only State fields are not sent by an ITS-A (TLC-FI §4.11.3).
		nlohmann::json sg = nlohmann::json::object();
		sg["reqState"] = static_cast<int>(state);
		upd.states.push_back(std::move(sg));
	}
	genfi::ObjectStateUpdateGroup group;
	group.update.push_back(std::move(upd));
	updateState(group);
}

void FiSession::outputs_setReqStates(const std::vector<std::pair<std::string, int>>& updates) {
	if (updates.empty()) return;
	genfi::ObjectStateUpdate upd;
	upd.objects.type = static_cast<int>(::tlcfi::tlcfi::TLCObjectType::Output);
	upd.objects.ids.reserve(updates.size());
	for (const auto& [id, state] : updates) {
		upd.objects.ids.push_back(id);
	}
	for (const auto& [id, state] : updates) {
		::tlcfi::tlcfi::Output o;
		o.id = id;
		o.reqState = state;
		upd.states.push_back(nlohmann::json(o));
	}
	genfi::ObjectStateUpdateGroup group;
	group.update.push_back(std::move(upd));
	updateState(group);
}

} // namespace tlcfi::fi


