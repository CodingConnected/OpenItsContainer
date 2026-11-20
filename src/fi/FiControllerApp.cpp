#include "fi/FiControllerApp.hpp"

#include <algorithm>
#include <future>
#include <iostream>

namespace tlcfi::fi
{

	namespace
	{

		const nlohmann::json *stateForObject(const genfi::ObjectStateUpdate &update,
											 const std::string &objectId)
		{
			const auto id = std::find(update.objects.ids.begin(), update.objects.ids.end(), objectId);
			if (id == update.objects.ids.end())
			{
				return nullptr;
			}

			const auto index = static_cast<std::size_t>(std::distance(update.objects.ids.begin(), id));
			return index < update.states.size() ? &update.states[index] : nullptr;
		}

	} // namespace

	FiControllerApp::FiControllerApp(const std::string &host, const std::string &port,
									 const std::string &username, const std::string &password,
									 const std::string &uri,
									 ::tlcfi::tlcfi::HandoverCapability startHandoverCapability,
									 ::tlcfi::tlcfi::HandoverCapability endHandoverCapability)
		: FiControllerApp(host, port, username, password, uri, tcp::TlsConfig{},
						  startHandoverCapability, endHandoverCapability)
	{
	}

	FiControllerApp::FiControllerApp(const std::string &host, const std::string &port,
									 const std::string &username, const std::string &password,
									 const std::string &uri, const tcp::TlsConfig &tlsConfig,
									 ::tlcfi::tlcfi::HandoverCapability startHandoverCapability,
									 ::tlcfi::tlcfi::HandoverCapability endHandoverCapability)
		: FiConsumerApp(host, port, username, password, uri, tlsConfig), startHandoverCapability_(startHandoverCapability), endHandoverCapability_(endHandoverCapability)
	{
	}

	FiControllerApp::~FiControllerApp()
	{
		// Join the meta/subscribe worker (and run control-specific teardown) here,
		// while this object is still fully alive. The base destructor would only
		// dispatch to FiConsumerApp::cleanup() (virtual call during base
		// destruction), which cannot see metaSubscribeThread_.
		cleanup();
	}

	bool FiControllerApp::handlesInitInternally() const { return true; }

	void FiControllerApp::onDisconnected(const std::string &reason)
	{
		// Notify external logic (e.g., CifHost) before base class cleanup
		if (onConnectionLost)
		{
			onConnectionLost(reason);
		}
		// Call base class handler (resets connected_, connectionState_, fullyInitialized_)
		FiConsumerApp::onDisconnected(reason);
	}

	void FiControllerApp::cleanup()
	{
		// Reset control-specific flags before base class cleanup
		// These flags must ALL be reset to allow the control state machine
		// to restart properly after a disconnection/reconnection
		metaStarted_.store(false);
		sawValidControlState_.store(false);
		readyToControlRequested_.store(false);
		inControlRequested_.store(false);
		waitingForIntersectionControl_.store(false);
		intersectionSubscribed_.store(false);
		intersectionControlState_.store(-1);
		{
			std::lock_guard<std::mutex> lock(controlMutex_);
			pendingControlIntersectionId_.clear();
		}

		// Note: Handover capabilities are NOT reset here - they are configuration
		// values that should persist across reconnections. Only runtime state
		// flags are reset to allow the control flow to restart.

		// Join the meta/subscribe worker before the base class destroys the
		// FiSession and transport that the worker uses. Stop the session first so
		// any in-flight RPC the worker is blocked on fails fast (failAll), so the
		// join does not hang.
		if (metaSubscribeThread_.joinable())
		{
			if (auto &fi = getFiSession())
			{
				fi->stop();
			}
			metaSubscribeThread_.join();
		}

		// Call base class cleanup (resets session, subscriptions, object state)
		FiConsumerApp::cleanup();
	}

	void FiControllerApp::setStartHandoverCapability(::tlcfi::tlcfi::HandoverCapability capability)
	{
		startHandoverCapability_ = capability;
	}

	::tlcfi::tlcfi::HandoverCapability FiControllerApp::getStartHandoverCapability() const
	{
		return startHandoverCapability_;
	}

	void FiControllerApp::setEndHandoverCapability(::tlcfi::tlcfi::HandoverCapability capability)
	{
		endHandoverCapability_ = capability;
	}

	::tlcfi::tlcfi::HandoverCapability FiControllerApp::getEndHandoverCapability() const
	{
		return endHandoverCapability_;
	}

	bool FiControllerApp::registerApp(std::chrono::milliseconds timeout)
	{
		// Register as Control application (not Consumer)
		if (!doRegistration(genfi::ApplicationType::Control, timeout))
		{
			return false;
		}

		// Add Session subscription for control state monitoring
		auto sessionId = getSessionId();
		if (sessionId.has_value() && !sessionId->empty())
		{
			genfi::ObjectReference sref;
			sref.type = 0;
			sref.ids = {*sessionId};
			try
			{
				auto snapshot = getFiSession()->subscribe(sref, std::chrono::milliseconds(5000)).get();
				log(LogLevel::Info, "subscribed to Session for control state monitoring");

				// Process initial Session state from subscribe snapshot
				// This is critical - the controlState may only be sent here, not in subsequent UpdateState notifications
				if (!snapshot.data.empty())
				{
					log(LogLevel::Debug, "Session subscribe snapshot: " + snapshot.data[0].dump());
					processInitialSessionState(snapshot.data[0]);
				}
			}
			catch (const std::exception &ex)
			{
				log(LogLevel::Error, "Session subscription failed: " + std::string(ex.what()));
				return false;
			}
		}

		// Start alive loop (required for maintaining session)
		startAliveLoop();
		return true;
	}

	bool FiControllerApp::subscribeIntersection(std::chrono::milliseconds timeout)
	{
		// Skip if already subscribed (prevents double subscription when called
		// from both launchMetaAndSubscribe and subscribeAll)
		if (intersectionSubscribed_.load())
		{
			log(LogLevel::Debug, "Intersection already subscribed, skipping");
			return true;
		}

		try
		{
			std::optional<std::string> intersectionId = getIntersectionId();

			if (intersectionId.has_value() && !intersectionId->empty())
			{
				genfi::ObjectReference ref;
				ref.type = 2;
				ref.ids = {*intersectionId};
				auto snapshot = getFiSession()->subscribe(ref, timeout).get();
				log(LogLevel::Info, "subscribed to " + std::to_string(snapshot.objects.ids.size()) +
										" intersection, initial data count=" + std::to_string(snapshot.data.size()));

				// Process initial intersection state from snapshot
				if (!snapshot.data.empty())
				{
					const auto &stateJson = snapshot.data[0];
					log(LogLevel::Debug, "Intersection subscribe snapshot: " + stateJson.dump());

					if (stateJson.contains("state") && stateJson["state"].is_number_integer())
					{
						int initialState = stateJson["state"].get<int>();
						intersectionControlState_ = initialState;
						log(LogLevel::Info, "Initial intersection state: " + intersectionStateToString(initialState) +
												" (" + std::to_string(initialState) + ")");
					}
				}
				intersectionSubscribed_.store(true);
				return true;
			}
			else
			{
				log(LogLevel::Info, "no intersection id found in meta; skipping subscribe");
				return true; // Not an error if there is no intersection
			}
		}
		catch (const std::exception &ex)
		{
			log(LogLevel::Error, "SubscribeIntersection failed: " + std::string(ex.what()));
			return false;
		}
	}

	void FiControllerApp::processSessionUpdate(const genfi::ObjectStateUpdateGroup &g)
	{
		// Handle control-specific Session updates
		handleSessionControlState(g);
		processSessionControlFlow(g);
	}

	void FiControllerApp::processSessionControlFlow(const genfi::ObjectStateUpdateGroup &g)
	{
		std::optional<std::string> mySession = getSessionId();
		std::optional<std::string> myIntersection = getIntersectionId();

		if (!mySession.has_value())
			return;

		for (const auto &upd : g.update)
		{
			int typeValue = -1;
			if (std::holds_alternative<int>(upd.objects.type))
			{
				typeValue = std::get<int>(upd.objects.type);
			}
			else if (std::holds_alternative<std::string>(upd.objects.type))
			{
				// Handle string type (e.g., "ControlApplication" or "Session")
				std::string typeStr = std::get<std::string>(upd.objects.type);
				if (typeStr == "ControlApplication" || typeStr == "Session")
				{
					typeValue = static_cast<int>(::tlcfi::tlcfi::TLCObjectType::Session);
				}
				else
				{
					continue;
				}
			}
			else
			{
				continue;
			}
			if (typeValue != static_cast<int>(::tlcfi::tlcfi::TLCObjectType::Session))
				continue;

			const auto *sessionState = stateForObject(upd, *mySession);
			if (sessionState == nullptr)
				continue;
			const auto &s = *sessionState;
			int cs = -1;
			try
			{
				if (s.contains("controlState") && s["controlState"].is_number_integer())
				{
					cs = s["controlState"].get<int>();
				}
				else
				{
					auto ca = s.get<::tlcfi::tlcfi::ControlApplication>();
					cs = static_cast<int>(ca.controlState);
				}
			}
			catch (...)
			{
				continue;
			}

			// When Offline confirmed by TLC, request ReadyToControl
			if (cs == static_cast<int>(::tlcfi::tlcfi::ControlState::Offline))
			{
				bool expected = false;
				if (readyToControlRequested_.compare_exchange_strong(expected, true))
				{
					std::optional<std::string> useIntersection = myIntersection;
					// If not known, try to read from this state payload
					if ((!useIntersection.has_value() || useIntersection->empty()) && s.contains("reqIntersection") && s["reqIntersection"].is_string())
					{
						useIntersection = s["reqIntersection"].get<std::string>();
					}
					if (useIntersection.has_value() && !useIntersection->empty())
					{
						getFiSession()->control_setReqState(::tlcfi::tlcfi::ControlState::ReadyToControl, *useIntersection,
															startHandoverCapability_, endHandoverCapability_);
						log(LogLevel::Info, "sent Session.reqControlState=ReadyToControl, reqIntersection=" + *useIntersection);
					}
				}
			}

			// When StartControl received, acknowledge with InControl, then handle intersection state
			if (cs == static_cast<int>(::tlcfi::tlcfi::ControlState::StartControl))
			{
				bool expected = false;
				if (inControlRequested_.compare_exchange_strong(expected, true))
				{
					std::optional<std::string> useIntersection = myIntersection;
					if ((!useIntersection.has_value() || useIntersection->empty()) && s.contains("reqIntersection") && s["reqIntersection"].is_string())
					{
						useIntersection = s["reqIntersection"].get<std::string>();
					}
					if (useIntersection.has_value() && !useIntersection->empty())
					{
						{
							std::lock_guard<std::mutex> lock(controlMutex_);
							pendingControlIntersectionId_ = *useIntersection;
						}

						// First, send InControl acknowledgment - we must be InControl before we can write to intersection
						getFiSession()->control_setReqState(::tlcfi::tlcfi::ControlState::InControl, *useIntersection,
															startHandoverCapability_, endHandoverCapability_);
						log(LogLevel::Info, "sent Session.reqControlState=InControl, reqIntersection=" + *useIntersection);

						// Check if intersection is already in Control state
						int currentIntersectionState = intersectionControlState_.load();
						log(LogLevel::Info, "StartControl received - intersection state: " + intersectionStateToString(currentIntersectionState) + " (" + std::to_string(currentIntersectionState) + ")");

						if (currentIntersectionState == static_cast<int>(::tlcfi::tlcfi::IntersectionControlState::Control))
						{
							// Intersection already in Control - start controller immediately
							log(LogLevel::Info, "Intersection already in Control state - starting controller");
							if (onControlInControl)
							{
								onControlInControl(*useIntersection);
							}
						}
						else
						{
							// Intersection not in Control - request it and wait
							// Note: We're now InControl, so we CAN write to intersection
							log(LogLevel::Info, "Intersection not in Control state - requesting Control state");
							waitingForIntersectionControl_ = true;
							requestIntersectionControlState(*useIntersection);
						}
					}
				}
			}

			// Handle EndControl - TLC is taking back control
			if (cs == static_cast<int>(::tlcfi::tlcfi::ControlState::EndControl))
			{
				log(LogLevel::Info, "EndControl received - TLC is taking back control");
				std::optional<std::string> useIntersection = myIntersection;
				if ((!useIntersection.has_value() || useIntersection->empty()) && s.contains("reqIntersection") && s["reqIntersection"].is_string())
				{
					useIntersection = s["reqIntersection"].get<std::string>();
				}
				// Notify callback
				if (useIntersection.has_value() && !useIntersection->empty() && onControlEndControl)
				{
					onControlEndControl(*useIntersection);
				}
				// Reset control flags for potential re-entry. readyToControlRequested_
				// must be cleared too, otherwise a later Offline transition cannot
				// re-arm the ReadyToControl request and the controller is stuck
				// until a full reconnect.
				waitingForIntersectionControl_ = false;
				inControlRequested_ = false;
				readyToControlRequested_ = false;
			}

			// Handle Error during control flow - abort pending operations
			if (cs == static_cast<int>(::tlcfi::tlcfi::ControlState::Error))
			{
				if (waitingForIntersectionControl_.load())
				{
					log(LogLevel::Error, "Session entered Error state while waiting for intersection Control - aborting");
					waitingForIntersectionControl_ = false;
					// Don't reset inControlRequested_ - we'll need a full reconnect
				}
			}
		}
	}

	std::string FiControllerApp::controlStateToString(int cs)
	{
		switch (static_cast<::tlcfi::tlcfi::ControlState>(cs))
		{
		case ::tlcfi::tlcfi::ControlState::Error:
			return "Error";
		case ::tlcfi::tlcfi::ControlState::NotConfigured:
			return "NotConfigured";
		case ::tlcfi::tlcfi::ControlState::Offline:
			return "Offline";
		case ::tlcfi::tlcfi::ControlState::ReadyToControl:
			return "ReadyToControl";
		case ::tlcfi::tlcfi::ControlState::StartControl:
			return "StartControl";
		case ::tlcfi::tlcfi::ControlState::InControl:
			return "InControl";
		case ::tlcfi::tlcfi::ControlState::EndControl:
			return "EndControl";
		default:
			return "Unknown(" + std::to_string(cs) + ")";
		}
	}

	void FiControllerApp::processInitialSessionState(const nlohmann::json &stateJson)
	{
		// Extract controlState from the initial subscribe snapshot
		int cs = -1;
		try
		{
			if (stateJson.contains("controlState") && stateJson["controlState"].is_number_integer())
			{
				cs = stateJson["controlState"].get<int>();
			}
			else
			{
				auto ca = stateJson.get<::tlcfi::tlcfi::ControlApplication>();
				cs = static_cast<int>(ca.controlState);
			}
		}
		catch (const std::exception &ex)
		{
			log(LogLevel::Warning, "Failed to parse controlState from Session snapshot: " + std::string(ex.what()));
			return;
		}

		log(LogLevel::Info, "Initial Session controlState: " + controlStateToString(cs) + " (" + std::to_string(cs) + ")");
		handleControlStateTransition(cs, stateJson);
	}

	void FiControllerApp::handleControlStateTransition(int cs, const nlohmann::json &stateJson)
	{
		// Handle INITIALIZATION states only - control flow states (StartControl, InControl, EndControl)
		// are handled separately by processSessionControlFlow()
		//
		// This function ensures meta/subscribe is triggered when needed during initialization.
		// Once meta has been started, we don't re-trigger it even on Error state transitions.
		//
		// IMPORTANT: When connecting to a TLC that's still booting, we may initially receive
		// controlState = -1 (Unknown). We must wait for a valid state transition before
		// triggering meta/subscribe.
		//
		// Intersection state checking (SwitchOn/SwitchOff/Error) is done inside
		// launchMetaAndSubscribe() after subscribing to the intersection, so we don't
		// need to gate on it here.

		// Check if this is a valid control state (not Unknown/-1)
		if (cs < 0)
		{
			log(LogLevel::Debug, "Received Unknown controlState, waiting for TLC to finish booting");
			return;
		}

		// This is a valid state - mark that we've seen one
		sawValidControlState_.exchange(true);

		switch (static_cast<::tlcfi::tlcfi::ControlState>(cs))
		{
		case ::tlcfi::tlcfi::ControlState::NotConfigured:
			if (!metaStarted_.load())
			{
				log(LogLevel::Info, "controlState is NotConfigured - triggering meta/subscribe");
				launchMetaAndSubscribe();
			}
			break;

		case ::tlcfi::tlcfi::ControlState::Error:
			if (!metaStarted_.load())
			{
				log(LogLevel::Warning, "Session in Error state at initialization - attempting meta/subscribe");
				launchMetaAndSubscribe();
			}
			else
			{
				log(LogLevel::Warning, "Session transitioned to Error state during control flow");
			}
			break;

		case ::tlcfi::tlcfi::ControlState::Offline:
		case ::tlcfi::tlcfi::ControlState::ReadyToControl:
			if (!metaStarted_.load())
			{
				log(LogLevel::Info, "Session in " + controlStateToString(cs) + " state - performing meta/subscribe");
				launchMetaAndSubscribe();
			}
			break;

		case ::tlcfi::tlcfi::ControlState::StartControl:
		case ::tlcfi::tlcfi::ControlState::InControl:
			if (!metaStarted_.load())
			{
				log(LogLevel::Warning, "Unexpected initial controlState: " + controlStateToString(cs) +
										   " - performing meta/subscribe");
				launchMetaAndSubscribe();
			}
			break;

		case ::tlcfi::tlcfi::ControlState::EndControl:
			break;

		default:
			log(LogLevel::Warning, "Unknown controlState value: " + std::to_string(cs));
			break;
		}
	}

	void FiControllerApp::handleSessionControlState(const genfi::ObjectStateUpdateGroup &g)
	{
		std::optional<std::string> mySession = getSessionId();
		if (!mySession.has_value())
			return;

		for (const auto &upd : g.update)
		{
			int typeValue = -1;
			if (std::holds_alternative<int>(upd.objects.type))
			{
				typeValue = std::get<int>(upd.objects.type);
			}
			else if (std::holds_alternative<std::string>(upd.objects.type))
			{
				// Handle string type (e.g., "ControlApplication" or "Session")
				std::string typeStr = std::get<std::string>(upd.objects.type);
				if (typeStr == "ControlApplication" || typeStr == "Session")
				{
					typeValue = static_cast<int>(::tlcfi::tlcfi::TLCObjectType::Session);
				}
				else
				{
					continue;
				}
			}
			else
			{
				continue;
			}
			if (typeValue != static_cast<int>(::tlcfi::tlcfi::TLCObjectType::Session))
				continue;

			const auto *sessionState = stateForObject(upd, *mySession);
			if (sessionState == nullptr)
				continue;
			const auto &s = *sessionState;
			int cs = -1;
			try
			{
				// Prefer robust check: read controlState directly as enum value
				if (s.contains("controlState") && s["controlState"].is_number_integer())
				{
					cs = s["controlState"].get<int>();
				}
				else
				{
					// Fallback: try full object parse
					auto ca = s.get<::tlcfi::tlcfi::ControlApplication>();
					cs = static_cast<int>(ca.controlState);
				}
			}
			catch (...)
			{
				// ignore parse issues per state element
				continue;
			}

			// Always log the received controlState for diagnostics
			log(LogLevel::Debug, "Session UpdateState controlState: " + controlStateToString(cs) + " (" + std::to_string(cs) + ") - JSON: " + s.dump());

			// Handle the state transition
			handleControlStateTransition(cs, s);
		}
	}

	void FiControllerApp::launchMetaAndSubscribe()
	{
		// Guard against being called from an UpdateState notification that races
		// doRegistration: if the session ID isn't stored yet the meta/subscribe
		// thread would fail immediately and permanently block retries via the CAS.
		if (!getSessionId().has_value())
			return;
		bool expected = false;
		if (!metaStarted_.compare_exchange_strong(expected, true))
			return; // already started
		// Defensive: a prior worker is always joined by cleanup() (which also
		// resets metaStarted_), so the member should not be joinable here. Join
		// anyway rather than risk std::terminate on move-assignment.
		if (metaSubscribeThread_.joinable())
		{
			metaSubscribeThread_.join();
		}
		// Run in a tracked (non-detached) thread so cleanup()/the destructor can
		// join it; a detached thread could touch this object after destruction.
		metaSubscribeThread_ = std::thread([this]()
										   {
		if (this->readMeta(std::chrono::milliseconds(5000))) {
			if (this->subscribeAll(std::chrono::milliseconds(5000))) {
				this->sendSessionReqControlStateOffline();
			}
		} });
	}

	void FiControllerApp::sendSessionReqControlStateOffline()
	{
		std::optional<std::string> mySession = getSessionId();
		std::optional<std::string> myIntersection = getIntersectionId();

		if (!mySession.has_value() || mySession->empty())
			return;

		genfi::ObjectStateUpdate upd;
		upd.objects.type = static_cast<int>(::tlcfi::tlcfi::TLCObjectType::Session);
		upd.objects.ids = {*mySession};
		nlohmann::json writeState = nlohmann::json::object();
		writeState["reqControlState"] = static_cast<int>(::tlcfi::tlcfi::ControlState::Offline);
		if (myIntersection.has_value() && !myIntersection->empty())
		{
			writeState["reqIntersection"] = *myIntersection;
		}
		upd.states.push_back(std::move(writeState));

		genfi::ObjectStateUpdateGroup group;
		group.update.push_back(std::move(upd));
		getFiSession()->updateState(group);
		std::string msg = "sent Session.reqControlState=Offline";
		if (myIntersection.has_value() && !myIntersection->empty())
		{
			msg += ", reqIntersection=" + *myIntersection;
		}
		log(LogLevel::Info, msg);
	}

	void FiControllerApp::requestSignalGroupStates(const std::vector<std::pair<std::string, ::tlcfi::tlcfi::SignalGroupState>> &updates)
	{
		if (!getFiSession())
			return;
		log(LogLevel::Debug, "FiControllerApp::requestSignalGroupStates() called");

		// Note: Signal group state management is in base class
		// We just send the update request
		sendSignalGroupUpdates(updates);
	}

	void FiControllerApp::requestOutputStates(const std::vector<std::pair<std::string, int>> &updates)
	{
		if (!getFiSession())
			return;
		log(LogLevel::Debug, "FiControllerApp::requestOutputStates() called");

		// Note: Output state management is in base class
		// We just send the update request
		sendOutputUpdates(updates);
	}

	// Helper methods for sending updates with only changed fields
	void FiControllerApp::sendSignalGroupUpdates(const std::vector<std::pair<std::string, ::tlcfi::tlcfi::SignalGroupState>> &updates)
	{
		if (!getFiSession() || updates.empty())
			return;

		genfi::ObjectStateUpdate upd;
		upd.objects.type = static_cast<int>(::tlcfi::tlcfi::TLCObjectType::SignalGroup);
		upd.objects.ids.reserve(updates.size());

		// Collect IDs and create minimal state updates with only changed fields
		for (const auto &[id, newState] : updates)
		{
			upd.objects.ids.push_back(id);

			// Create minimal JSON with only the reqState field (writable field)
			nlohmann::json stateUpdate = nlohmann::json::object();
			stateUpdate["reqState"] = static_cast<int>(newState);
			upd.states.push_back(std::move(stateUpdate));
		}

		// Debug: print what we're sending
		std::string debugMsg = "Sending SignalGroup update with " + std::to_string(updates.size()) + " objects:";
		for (size_t i = 0; i < updates.size(); ++i)
		{
			if (i > 0)
				debugMsg += ";";
			debugMsg += " id=" + upd.objects.ids[i] + " state=" + std::to_string(static_cast<int>(updates[i].second));
		}
		log(LogLevel::Debug, debugMsg);

		// Send the update group
		genfi::ObjectStateUpdateGroup group;
		group.update.push_back(std::move(upd));
		getFiSession()->updateState(group);

		log(LogLevel::Debug, "Sent SignalGroup updates for " + std::to_string(updates.size()) + " objects");
	}

	void FiControllerApp::sendOutputUpdates(const std::vector<std::pair<std::string, int>> &updates)
	{
		if (!getFiSession() || updates.empty())
			return;

		genfi::ObjectStateUpdate upd;
		upd.objects.type = static_cast<int>(::tlcfi::tlcfi::TLCObjectType::Output);
		upd.objects.ids.reserve(updates.size());

		// Collect IDs and create minimal state updates with only changed fields
		for (const auto &[id, newState] : updates)
		{
			upd.objects.ids.push_back(id);

			// Create minimal JSON with only the reqState field (writable field)
			nlohmann::json stateUpdate = nlohmann::json::object();
			stateUpdate["reqState"] = newState;
			upd.states.push_back(std::move(stateUpdate));
		}

		// Debug: print what we're sending
		std::string debugMsg = "Sending Output update with " + std::to_string(updates.size()) + " objects:";
		for (size_t i = 0; i < updates.size(); ++i)
		{
			if (i > 0)
				debugMsg += ";";
			debugMsg += " id=" + upd.objects.ids[i] + " state=" + std::to_string(static_cast<int>(updates[i].second));
		}
		log(LogLevel::Debug, debugMsg);

		// Send the update group
		genfi::ObjectStateUpdateGroup group;
		group.update.push_back(std::move(upd));
		getFiSession()->updateState(group);

		log(LogLevel::Debug, "Sent Output updates for " + std::to_string(updates.size()) + " objects");
	}

	void FiControllerApp::processObjectUpdates(const genfi::ObjectStateUpdateGroup &g)
	{
		// First call base class to handle SignalGroup, Detector, etc.
		FiConsumerApp::processObjectUpdates(g);

		// Then handle intersection state updates for control flow
		for (const auto &upd : g.update)
		{
			int typeValue = -1;
			if (std::holds_alternative<int>(upd.objects.type))
			{
				typeValue = std::get<int>(upd.objects.type);
			}
			if (typeValue != static_cast<int>(::tlcfi::tlcfi::TLCObjectType::Intersection))
				continue;

			const auto intersection = getIntersectionId();
			const auto updateCount = std::min(upd.objects.ids.size(), upd.states.size());
			for (std::size_t i = 0; i < updateCount; ++i)
			{
				if (intersection.has_value() && upd.objects.ids[i] != *intersection)
				{
					continue;
				}
				const auto &s = upd.states[i];
				int intersectionState = -1;
				try
				{
					if (s.contains("state") && s["state"].is_number_integer())
					{
						intersectionState = s["state"].get<int>();
					}
				}
				catch (...)
				{
					continue;
				}

				// Update tracked intersection state
				int previousState = intersectionControlState_.exchange(intersectionState);
				if (previousState != intersectionState)
				{
					log(LogLevel::Info, "Intersection state changed: " + intersectionStateToString(previousState) +
											" -> " + intersectionStateToString(intersectionState));
				}

				// Check if we were waiting for intersection to reach Control state
				if (waitingForIntersectionControl_.load() &&
					intersectionState == static_cast<int>(::tlcfi::tlcfi::IntersectionControlState::Control))
				{
					log(LogLevel::Info, "Intersection reached Control state - completing InControl transition");
					waitingForIntersectionControl_ = false;
					completeInControlTransition();
				}

				// Check if we need to trigger meta/subscribe because intersection became ready
				// This handles the case where controlState transitioned first (e.g., to NotConfigured)
				// but intersection was still in SwitchOn/SwitchOff/Error. Now that intersection
				// is in a stable state, we can proceed with meta/subscribe.
				if (!metaStarted_.load() && sawValidControlState_.load() && isIntersectionReadyForMeta())
				{
					log(LogLevel::Info, "Intersection now in stable state (" + intersectionStateToString(intersectionState) +
											") - triggering deferred meta/subscribe");
					launchMetaAndSubscribe();
				}
			}
		}
	}

	void FiControllerApp::handleSessionEvent(const genfi::SessionEvent &ev)
	{
		// Failure codes (>= 1000) accompany the Facilities setting controlState=Error and
		// closing the connection (TLC-FI §7.7/§7.9). The base class already logged the event
		// and fired onSessionError; here we reset the control-flow guards so a subsequent
		// reconnect (driven by FiAppHost) re-arms the control sequence cleanly. cleanup()
		// also resets these on disconnect; doing it here makes the reaction immediate and
		// explicit for the control failure.
		if (ev.code >= 1000)
		{
			log(LogLevel::Warning, "Session failure event (" + sessionEventCodeToString(ev.code) +
									   ") during control - resetting control-flow guards");
			readyToControlRequested_ = false;
			inControlRequested_ = false;
			waitingForIntersectionControl_ = false;
		}
	}

	void FiControllerApp::requestIntersectionControlState(const std::string &intersectionId)
	{
		if (!getFiSession())
			return;

		genfi::ObjectStateUpdate upd;
		upd.objects.type = static_cast<int>(::tlcfi::tlcfi::TLCObjectType::Intersection);
		upd.objects.ids = {intersectionId};

		nlohmann::json stateUpdate = nlohmann::json::object();
		stateUpdate["reqState"] = static_cast<int>(::tlcfi::tlcfi::IntersectionControlState::Control);
		upd.states.push_back(std::move(stateUpdate));

		genfi::ObjectStateUpdateGroup group;
		group.update.push_back(std::move(upd));
		getFiSession()->updateState(group);

		log(LogLevel::Info, "Sent Intersection.reqState=Control for intersection: " + intersectionId);
	}

	void FiControllerApp::completeInControlTransition()
	{
		// Called when intersection reaches Control state (after we've already sent InControl)
		// Now we can start the controller
		std::string intersectionId;
		{
			std::lock_guard<std::mutex> lock(controlMutex_);
			intersectionId = pendingControlIntersectionId_;
		}

		if (intersectionId.empty())
		{
			log(LogLevel::Error, "completeInControlTransition called but no pending intersection ID");
			return;
		}

		log(LogLevel::Info, "Intersection is in Control state - starting controller for " + intersectionId);

		// Notify external host that we're ready to start control
		if (onControlInControl)
		{
			onControlInControl(intersectionId);
		}
	}

	std::string FiControllerApp::intersectionStateToString(int state)
	{
		switch (static_cast<::tlcfi::tlcfi::IntersectionControlState>(state))
		{
		case ::tlcfi::tlcfi::IntersectionControlState::Error:
			return "Error";
		case ::tlcfi::tlcfi::IntersectionControlState::Dark:
			return "Dark";
		case ::tlcfi::tlcfi::IntersectionControlState::Standby:
			return "Standby";
		case ::tlcfi::tlcfi::IntersectionControlState::AlternativeStandby:
			return "AlternativeStandby";
		case ::tlcfi::tlcfi::IntersectionControlState::SwitchOn:
			return "SwitchOn";
		case ::tlcfi::tlcfi::IntersectionControlState::SwitchOff:
			return "SwitchOff";
		case ::tlcfi::tlcfi::IntersectionControlState::AllRed:
			return "AllRed";
		case ::tlcfi::tlcfi::IntersectionControlState::Control:
			return "Control";
		default:
			return "Unknown(" + std::to_string(state) + ")";
		}
	}

	bool FiControllerApp::isIntersectionReadyForMeta() const
	{
		int state = intersectionControlState_.load();

		// Unknown state (-1) - we haven't received intersection state yet
		// This can happen if the Session subscription snapshot doesn't include intersection state
		// In this case, we proceed with meta/subscribe and hope for the best
		if (state < 0)
		{
			return true;
		}

		// Check if the intersection is in a stable operational state
		// These states indicate the TLC has valid configuration data
		switch (static_cast<::tlcfi::tlcfi::IntersectionControlState>(state))
		{
		case ::tlcfi::tlcfi::IntersectionControlState::Control:
		case ::tlcfi::tlcfi::IntersectionControlState::AllRed:
		case ::tlcfi::tlcfi::IntersectionControlState::Standby:
		case ::tlcfi::tlcfi::IntersectionControlState::AlternativeStandby:
		case ::tlcfi::tlcfi::IntersectionControlState::Dark:
			// Stable states - safe to read meta
			return true;

		case ::tlcfi::tlcfi::IntersectionControlState::SwitchOn:
		case ::tlcfi::tlcfi::IntersectionControlState::SwitchOff:
		case ::tlcfi::tlcfi::IntersectionControlState::Error:
			// Transient or error states - wait for stable state
			// TLC is still booting or in error, meta may be incomplete
			return false;

		default:
			// Unknown state - proceed with caution
			return true;
		}
	}

} // namespace tlcfi::fi
