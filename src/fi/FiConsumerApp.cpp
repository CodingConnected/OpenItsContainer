#include "fi/FiConsumerApp.hpp"
#include "models/tlc-fi/Version.hpp"

#include <asio/post.hpp>
#include <algorithm>
#include <future>
#include <iostream>

#if defined(_WIN32)
#include <conio.h>
#endif

namespace tlcfi::fi {

FiConsumerApp::FiConsumerApp(const std::string& host, const std::string& port,
                              const std::string& username, const std::string& password,
                              const std::string& uri, const tcp::TlsConfig& tlsConfig)
	: host_(host), port_(port), username_(username), password_(password), uri_(uri),
	  tlsConfig_(tlsConfig), io_(), workGuard_(asio::make_work_guard(io_)) {
	
	// Set up default logging callback (prints to stdout/stderr)
	onLog = [](LogLevel level, const std::string& message) {
		switch (level) {
		case LogLevel::Trace:
		case LogLevel::Debug:
		case LogLevel::Info:
			std::cout << message << "\n";
			std::cout.flush();
			break;
		case LogLevel::Error:
		case LogLevel::Fatal:
			std::cerr << message << "\n";
			std::cerr.flush();
			break;
		}
	};
}

FiConsumerApp::~FiConsumerApp() { stop(); }

bool FiConsumerApp::connect(std::chrono::milliseconds timeout) {
	// Always clean up any previous connection attempt first (idempotent)
	cleanup();
	
	// Create new transport and session
	transport_ = std::make_shared<tcp::TcpClientTransport>(io_, host_.c_str(), port_.c_str(), tlsConfig_);
	framer_ = std::make_shared<jsonrpc::BalancedJsonFramer>();
	rpc_ = std::make_shared<jsonrpc::JsonRpcSession>(transport_, framer_);
	fi_ = std::make_unique<FiSession>(rpc_);

	//rpc_->onTrace = [](const char* dir, const std::string& text) {
	//	std::cout << "[" << dir << "] " << text << "\n";
	//};

	// Wire up transport error callbacks for TLS error reporting
	transport_->onError = [this](const std::string& reason) {
		// Transport errors (including TLS handshake failures) are reported as protocol errors
		this->onProtocolError(reason);
	};
	
	rpc_->events.onConnected = [this]() { this->onConnected(); };
	rpc_->events.onDisconnected = [this](const std::string& reason) { this->onDisconnected(reason); };
	rpc_->events.onProtocolError = [this](const std::string& reason) { this->onProtocolError(reason); };
	rpc_->events.onNotification = [this](const std::string& method, nlohmann::json params) { 
		this->onNotification(method, params); 
	};

	fi_->start();
	ioThread_ = std::thread([this]() { io_.run(); });

	for (int i = 0; i < static_cast<int>(timeout.count() / 50) && !connected_.load(); ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	
	// If connection failed, the next connect() call will clean up
	// No need to clean up here - it's redundant
	
	return connected_.load();
}

bool FiConsumerApp::doRegistration(genfi::ApplicationType type, std::chrono::milliseconds timeout) {
	if (!fi_) {
		log(LogLevel::Error, "FiSession not initialized. Call connect() first.");
		return false;
	}

	genfi::RegistrationRequest req;
	req.username = username_;
	req.password = password_;
	req.type = static_cast<int>(type);
	req.version = ::tlcfi::version::PROTOCOL_VERSION;
	req.uri = uri_;

	try {
		auto fut = fi_->registerApp(req, std::chrono::milliseconds(3000));
		if (fut.wait_for(timeout) != std::future_status::ready) {
			log(LogLevel::Error, "register timeout");
			return false;
		}
		auto rep = fut.get();
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			sessionId_ = rep.sessionid;
		}
		log(LogLevel::Info, "registered, session: " + sessionId_.value());
		return true;
	}
	catch (const std::exception& ex) {
		log(LogLevel::Error, "register failed: " + std::string(ex.what()));
		return false;
	}
}

bool FiConsumerApp::registerApp(std::chrono::milliseconds timeout) {
	if (!doRegistration(genfi::ApplicationType::Consumer, timeout)) {
		return false;
	}

	connectionState_ = ConnectionState::Registered;
	startAliveLoop();
	return true;
}

bool FiConsumerApp::readMeta(std::chrono::milliseconds timeout) {
	try {
		auto meta = fi_->readMeta(timeout).get();

		log(LogLevel::Debug, "FiConsumerApp::readMeta() meta.meta.size() = " + std::to_string(meta.meta.size()));
		
		std::string intersectionId;
		for (const auto& m : meta.meta) {
			if (m.contains("intersections") && m["intersections"].is_array() && !m["intersections"].empty()) {
				const auto& first = m["intersections"][0];
				if (first.is_string()) { intersectionId = first.get<std::string>(); break; }
			}
		}
		if (intersectionId.empty()) {
			for (const auto& m : meta.meta) {
				if (m.contains("id") && m["id"].is_string()) { intersectionId = m["id"].get<std::string>(); break; }
			}
		}

		std::vector<std::string> signalGroupIds;
		std::vector<std::string> outputIds;
		std::vector<std::string> detectorIds;
		std::vector<std::string> inputIds;
		if (!intersectionId.empty()) {
			genfi::ObjectReference iref; iref.type = 2; iref.ids = { intersectionId };
			auto imeta = fi_->readMeta(iref, std::chrono::milliseconds(5000)).get();
			for (const auto& m : imeta.meta) {
				if (m.contains("signalgroups") && m["signalgroups"].is_array()) {
					for (const auto& sgid : m["signalgroups"]) {
						if (sgid.is_string()) signalGroupIds.push_back(sgid.get<std::string>());
					}
				}
				if (m.contains("outputs") && m["outputs"].is_array()) {
					for (const auto& oid : m["outputs"]) {		
						if (oid.is_string()) outputIds.push_back(oid.get<std::string>());
					}
				}
				if (m.contains("detectors") && m["detectors"].is_array()) {
					for (const auto& did : m["detectors"]) {
						if (did.is_string()) detectorIds.push_back(did.get<std::string>());
					}
				}
				if (m.contains("inputs") && m["inputs"].is_array()) {
					for (const auto& iid : m["inputs"]) {
						if (iid.is_string()) inputIds.push_back(iid.get<std::string>());
					}
				}
			}
		}

		// Extract optional spvehgenerator ID from intersection meta
		// Note: spvehgenerator is a single string, not an array
		std::optional<std::string> spvehGeneratorId;
		if (!intersectionId.empty()) {
			genfi::ObjectReference svRef; svRef.type = 2; svRef.ids = { intersectionId };
			try {
				auto svMeta = fi_->readMeta(svRef, std::chrono::milliseconds(5000)).get();
				for (const auto& m : svMeta.meta) {
					if (m.contains("spvehgenerator") && m["spvehgenerator"].is_string()) {
						spvehGeneratorId = m["spvehgenerator"].get<std::string>();
						log(LogLevel::Info, "Discovered spvehgenerator: " + *spvehGeneratorId);
					}
				}
			}
			catch (const std::exception& ex) {
				log(LogLevel::Debug, "No spvehgenerator in intersection meta (optional): " + std::string(ex.what()));
			}
		}

		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			intersectionId_ = intersectionId;
			signalGroupIds_ = std::move(signalGroupIds);
			outputIds_ = std::move(outputIds);
			detectorIds_ = std::move(detectorIds);
			inputIds_ = std::move(inputIds);
			spvehGeneratorId_ = spvehGeneratorId;
		}
		if (onMetaReceived) {
			onMetaReceived(signalGroupIds_, outputIds_, detectorIds_, inputIds_);
		}
		return true;
	}
	catch (const std::exception& ex) {
		log(LogLevel::Error, "ReadMeta failed: " + std::string(ex.what()));
		return false;
	}
}

bool FiConsumerApp::subscribeSignalGroups(std::chrono::milliseconds timeout) {
	try {
		std::vector<std::string> signalGroupIds;
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			signalGroupIds = signalGroupIds_;
		}

		if (!signalGroupIds.empty()) {
			genfi::ObjectReference ref; ref.type = 3; ref.ids = signalGroupIds;
			auto snapshot = fi_->subscribe(ref, timeout).get();
			log(LogLevel::Info, "subscribed to " + std::to_string(snapshot.objects.ids.size()) + " signalgroups, initial data count=" + std::to_string(snapshot.data.size()));

			// Initialize local SignalGroup instances with initial data
			for (size_t i = 0; i < snapshot.objects.ids.size() && i < snapshot.data.size(); ++i) {
				const auto& id = snapshot.objects.ids[i];
				const auto& data = snapshot.data[i];
				log(LogLevel::Trace, "SignalGroup " + id + " initial data: " + data.dump());
				updateSignalGroupState(id, data, 0); // Use 0 for subscription snapshots (should contain stateticks)
			}
			return true;
		}
		else {
			log(LogLevel::Info, "no signalgroup ids found in meta; skipping subscribe");
			return true; // Not an error if there are no signal groups
		}
	}
	catch (const std::exception& ex) {
		log(LogLevel::Error, "SubscribeSignalGroups failed: " + std::string(ex.what()));
		return false;
	}
}

bool FiConsumerApp::subscribeOutputs(std::chrono::milliseconds timeout) {
	try {
		std::vector<std::string> outputIds;
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			outputIds = outputIds_;
		}

		if (!outputIds.empty()) {
			genfi::ObjectReference ref; ref.type = 6; ref.ids = outputIds;
			auto snapshot = fi_->subscribe(ref, timeout).get();
			log(LogLevel::Info, "subscribed to " + std::to_string(snapshot.objects.ids.size()) + " outputs, initial data count=" + std::to_string(snapshot.data.size()));
			
			// Initialize local Output instances with initial data
			for (size_t i = 0; i < snapshot.objects.ids.size() && i < snapshot.data.size(); ++i) {
				const auto& id = snapshot.objects.ids[i];
				const auto& data = snapshot.data[i];
				updateOutputState(id, data, 0); // Use 0 for subscription snapshots (should contain stateticks)
			}
			return true;
		}
		else {
			log(LogLevel::Info, "no output ids found in meta; skipping subscribe");
			return true; // Not an error if there are no outputs
		}
	}
	catch (const std::exception& ex) {
		log(LogLevel::Error, "SubscribeOutputs failed: " + std::string(ex.what()));
		return false;
	}
}

bool FiConsumerApp::subscribeDetectors(std::chrono::milliseconds timeout) {
	try {
		std::vector<std::string> detectorIds;
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			detectorIds = detectorIds_;
		}

		if (!detectorIds.empty()) {
			genfi::ObjectReference ref; ref.type = 4; ref.ids = detectorIds;
			auto snapshot = fi_->subscribe(ref, timeout).get();
			log(LogLevel::Info, "subscribed to " + std::to_string(snapshot.objects.ids.size()) + " detectors, initial data count=" + std::to_string(snapshot.data.size()));
			
			// Initialize local Detector instances with initial data
			for (size_t i = 0; i < snapshot.objects.ids.size() && i < snapshot.data.size(); ++i) {
				const auto& id = snapshot.objects.ids[i];
				const auto& data = snapshot.data[i];
				updateDetectorState(id, data, 0); // Use 0 for subscription snapshots (should contain stateticks)
			}
			return true;
		}
		else {
			log(LogLevel::Info, "no detector ids found in meta; skipping subscribe");
			return true; // Not an error if there are no detectors
		}
	}
	catch (const std::exception& ex) {
		log(LogLevel::Error, "SubscribeDetectors failed: " + std::string(ex.what()));
		return false;
	}
}

bool FiConsumerApp::subscribeInputs(std::chrono::milliseconds timeout) {
	try {
		std::vector<std::string> inputIds;
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			inputIds = inputIds_;
		}

		if (!inputIds.empty()) {
			genfi::ObjectReference ref; ref.type = 5; ref.ids = inputIds;
			auto snapshot = fi_->subscribe(ref, timeout).get();
			log(LogLevel::Info, "subscribed to " + std::to_string(snapshot.objects.ids.size()) + " inputs, initial data count=" + std::to_string(snapshot.data.size()));
			
			// Initialize local Input instances with initial data
			for (size_t i = 0; i < snapshot.objects.ids.size() && i < snapshot.data.size(); ++i) {
				const auto& id = snapshot.objects.ids[i];
				const auto& data = snapshot.data[i];
				updateInputState(id, data, 0); // Use 0 for subscription snapshots (should contain stateticks)
			}
			return true;
		}
		else {
			log(LogLevel::Info, "no input ids found in meta; skipping subscribe");
			return true; // Not an error if there are no inputs
		}
	}
	catch (const std::exception& ex) {
		log(LogLevel::Error, "SubscribeInputs failed: " + std::string(ex.what()));
		return false;
	}
}

bool FiConsumerApp::subscribeIntersection(std::chrono::milliseconds timeout) {
	try {
		std::optional<std::string> intersectionId;
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			intersectionId = intersectionId_;
		}

		if (intersectionId.has_value() && !intersectionId->empty()) {
			genfi::ObjectReference ref; ref.type = 2; ref.ids = { *intersectionId };
			auto snapshot = fi_->subscribe(ref, timeout).get();
			log(LogLevel::Info, "subscribed to " + std::to_string(snapshot.objects.ids.size()) + " intersection, initial data count=" + std::to_string(snapshot.data.size()));
			return true;
		}
		else {
			log(LogLevel::Info, "no intersection id found in meta; skipping subscribe");
			return true; // Not an error if there is no intersection
		}
	}
	catch (const std::exception& ex) {
		log(LogLevel::Error, "SubscribeIntersection failed: " + std::string(ex.what()));
		return false;
	}
}

bool FiConsumerApp::subscribeSpecialVehicleEventGenerator(std::chrono::milliseconds timeout) {
	try {
		std::optional<std::string> spvehId;
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			spvehId = spvehGeneratorId_;
		}

		if (spvehId.has_value() && !spvehId->empty()) {
			genfi::ObjectReference ref;
			ref.type = static_cast<int>(::tlcfi::tlcfi::TLCObjectType::SpecialVehicleEventGenerator);
			ref.ids = { *spvehId };
			auto snapshot = fi_->subscribe(ref, timeout).get();
			log(LogLevel::Info, "subscribed to spvehgenerator " + *spvehId);
			// No initial state to parse - events are transient, not persistent state
			return true;
		}
		else {
			log(LogLevel::Info, "no spvehgenerator id found in meta; skipping subscribe");
			return true; // Not an error if there is no spvehgenerator
		}
	}
	catch (const std::exception& ex) {
		log(LogLevel::Error, "SubscribeSpecialVehicleEventGenerator failed: " + std::string(ex.what()));
		return false;
	}
}

bool FiConsumerApp::subscribeAll(std::chrono::milliseconds timeout) {
	bool success = true;
	success = subscribeSignalGroups(timeout) && success;
	success = subscribeOutputs(timeout) && success;
	success = subscribeDetectors(timeout) && success;
	success = subscribeInputs(timeout) && success;
	success = subscribeIntersection(timeout) && success;
	success = subscribeSpecialVehicleEventGenerator(timeout) && success;
	if (success) {
		connectionState_ = ConnectionState::Subscribed;
		fullyInitialized_ = true;
	}
	return success;
}

void FiConsumerApp::monitorUntilKeyPress() {
	log(LogLevel::Info, "Press any key to stop...");
#if defined(_WIN32)
	(void)_getch();
#else
	(void)std::cin.get();
#endif
}

void FiConsumerApp::stop() {
	// Make stop() idempotent - safe to call multiple times
	if (stopped_.exchange(true)) {
		return; // Already stopped
	}
	
	// Try to deregister gracefully before cleanup
	if (fi_) {
		try { 
			(void)fi_->deregisterApp(std::chrono::milliseconds(3000)).get(); 
			log(LogLevel::Info, "deregistered");
		}
		catch (const std::exception& ex) { 
			log(LogLevel::Error, "deregister failed: " + std::string(ex.what()));
		}
	}
	
	// Perform complete cleanup (I/O thread, transport, state)
	cleanup();
}

void FiConsumerApp::cleanup() {
	// 1. Stop alive loop
	stopAliveLoop();
	
	// 2. Clean up I/O thread
	if (ioThread_.joinable()) {
		workGuard_.reset();
		io_.stop();
		try {
			ioThread_.join();
		} catch (const std::exception& ex) {
			// Thread may have already been joined - ignore and continue cleanup
		}
		// Reset the I/O context for reuse
		io_.restart();
		// Re-arm the work guard so the I/O context is ready for the next
		// connect(). Using std::optional avoids the undefined behaviour of
		// placement-new over a still-live, non-trivially-destructible object.
		workGuard_.emplace(asio::make_work_guard(io_));
	}
	
	// 3. Clean up transport/RPC session
	if (fi_) {
		fi_->stop();
		fi_.reset();
	}
	if (transport_) {
		transport_->stop();
		transport_.reset();
	}
	rpc_.reset();
	framer_.reset();
	
	// 4. Clear all state
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		sessionId_.reset();
		intersectionId_.reset();
		signalGroupIds_.clear();
		outputIds_.clear();
		detectorIds_.clear();
		inputIds_.clear();
		spvehGeneratorId_.reset();
		signalGroups_.clear();
		outputs_.clear();
		detectors_.clear();
		inputs_.clear();
	}
	
	// 5. Reset all flags
	connected_ = false;
	connectionState_ = ConnectionState::Disconnected;
	fullyInitialized_ = false;
}

::tlcfi::tlcfi::SignalGroupState FiConsumerApp::getSignalGroupState(const std::string& id) {
	std::lock_guard<std::mutex> lock(stateMutex_);
	auto it = signalGroups_.find(id);
	if (it != signalGroups_.end()) {
		// Return the actual current state from the TLC, not the requested state
		return it->second.state;
	}
	return ::tlcfi::tlcfi::SignalGroupState::Unavailable;
}

std::optional<std::string> FiConsumerApp::getSessionId() {
	std::lock_guard<std::mutex> lock(stateMutex_);
	return sessionId_;
}

std::optional<std::string> FiConsumerApp::getIntersectionId() {
	std::lock_guard<std::mutex> lock(stateMutex_);
	return intersectionId_;
}

std::vector<std::string> FiConsumerApp::getSignalGroupIds() {
	std::lock_guard<std::mutex> lock(stateMutex_);
	return signalGroupIds_;
}

std::vector<std::string> FiConsumerApp::getOutputIds() {
	std::lock_guard<std::mutex> lock(stateMutex_);
	return outputIds_;
}

std::vector<std::string> FiConsumerApp::getDetectorIds() {
	std::lock_guard<std::mutex> lock(stateMutex_);
	return detectorIds_;
}

std::vector<std::string> FiConsumerApp::getInputIds() {
	std::lock_guard<std::mutex> lock(stateMutex_);
	return inputIds_;
}

::tlcfi::tlcfi::Detector FiConsumerApp::getDetector(const std::string& id) {
	std::lock_guard<std::mutex> lock(stateMutex_);
	auto it = detectors_.find(id);
	if (it != detectors_.end()) {
		return it->second;
	}
	return {};
}

::tlcfi::tlcfi::Input FiConsumerApp::getInput(const std::string& id) {
	std::lock_guard<std::mutex> lock(stateMutex_);
	auto it = inputs_.find(id);
	if (it != inputs_.end()) {
		return it->second;
	}
	return {};
}

::tlcfi::tlcfi::Output FiConsumerApp::getOutput(const std::string& id) {
	std::lock_guard<std::mutex> lock(stateMutex_);
	auto it = outputs_.find(id);
	if (it != outputs_.end()) {
		return it->second;
	}
	return {};
}

void FiConsumerApp::onConnected() {
	connected_ = true;
	connectionState_ = ConnectionState::Connected;
	log(LogLevel::Info, "connected");
	// Invoke public callback if set
	if (onConnectedEvent) {
		onConnectedEvent();
	}
}

void FiConsumerApp::onDisconnected(const std::string& reason) { 
	connected_ = false;
	connectionState_ = ConnectionState::Disconnected;
	fullyInitialized_ = false;
	// Note: Don't call cleanup() here - let the retry loop handle cleanup via connect()
	// This prevents race conditions when cleanup happens from multiple threads
	log(LogLevel::Info, "disconnected: " + reason);
}

void FiConsumerApp::onProtocolError(const std::string& reason) { 
	log(LogLevel::Error, "protocol error: " + reason);
}

void FiConsumerApp::onNotification(const std::string& method, nlohmann::json params) {
	if (method == "UpdateState") {
		try {
			genfi::ObjectStateUpdateGroup g = params.get<genfi::ObjectStateUpdateGroup>();
			log(LogLevel::Trace, "UpdateState ticks=" + std::to_string(g.ticks) + " items=" + std::to_string(g.update.size()));
			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				lastStateTicks_ = g.ticks;
			}

			// Let derived classes process Session updates first (e.g., for control flow)
			processSessionUpdate(g);

			// Process object-specific updates
			processObjectUpdates(g);
		}
		catch (const std::exception& ex) {
			log(LogLevel::Error, "UpdateState parse error: " + std::string(ex.what()));
		}
	}
	else if (method == "NotifyEvent") {
		try {
			genfi::NotifyEventGroup g = params.get<genfi::NotifyEventGroup>();
			log(LogLevel::Debug, "NotifyEvent ticks=" + std::to_string(g.ticks) +
				" objects=" + std::to_string(g.objects.ids.size()) +
				" events=" + std::to_string(g.events.size()));
			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				lastStateTicks_ = g.ticks;
			}

			// Process event notifications
			processEventUpdates(g);
		}
		catch (const std::exception& ex) {
			log(LogLevel::Error, "NotifyEvent parse error: " + std::string(ex.what()));
		}
	}
}

void FiConsumerApp::processSessionUpdate(const genfi::ObjectStateUpdateGroup& g) {
	// Base implementation is empty. Derived classes can override to handle Session updates.
	(void)g; // Suppress unused parameter warning
}

void FiConsumerApp::processObjectUpdates(const genfi::ObjectStateUpdateGroup& g) {
	// Fan-out object-specific updates for external logic
	for (const auto& upd : g.update) {
		int typeValue = -1;
		if (std::holds_alternative<int>(upd.objects.type)) typeValue = std::get<int>(upd.objects.type);
		else
		{
			log(LogLevel::Error, "FiConsumerApp::processObjectUpdates() typeValue is not an int");
			continue;
		}
		if (upd.objects.ids.size() != upd.states.size()) {
			log(LogLevel::Error, "FiConsumerApp::processObjectUpdates() object/state count mismatch");
		}
		const auto updateCount = std::min(upd.objects.ids.size(), upd.states.size());
		for (std::size_t i = 0; i < updateCount; ++i) {
			const auto& id = upd.objects.ids[i];
			const auto& st = upd.states[i];
			try {
				log(LogLevel::Trace, "Processing typeValue: " + std::to_string(typeValue));
				switch (typeValue) {
				case static_cast<int>(::tlcfi::tlcfi::TLCObjectType::SignalGroup): {
					log(LogLevel::Trace, "FiConsumerApp::processObjectUpdates() SignalGroup - JSON: " + st.dump());
					log(LogLevel::Trace, "Object ID: " + id);
					updateSignalGroupState(id, st, g.ticks);
					break; }
				case static_cast<int>(::tlcfi::tlcfi::TLCObjectType::Output): {
					updateOutputState(id, st, g.ticks);
					break; }
				case static_cast<int>(::tlcfi::tlcfi::TLCObjectType::Detector): {
					updateDetectorState(id, st, g.ticks);
					break; }
				case static_cast<int>(::tlcfi::tlcfi::TLCObjectType::Input): {
					updateInputState(id, st, g.ticks);
					break; }
				case static_cast<int>(::tlcfi::tlcfi::TLCObjectType::Session):
					// Session updates are handled by processSessionUpdate() in derived classes
					// Silently ignore here to avoid warning spam
					break;
				case static_cast<int>(::tlcfi::tlcfi::TLCObjectType::SpecialVehicleEventGenerator):
					// Special vehicle events are delivered via NotifyEvent
					// (see processEventUpdates), not via UpdateState. An
					// UpdateState for this object type carries no event payload,
					// so do not attempt to parse one here.
					log(LogLevel::Trace, "SpecialVehicleEventGenerator UpdateState ignored (events arrive via NotifyEvent)");
					break;
				case static_cast<int>(::tlcfi::tlcfi::TLCObjectType::Intersection):
					// Intersection updates may come through but are informational
					// Could be extended to track intersection control state
					log(LogLevel::Trace, "Intersection state update received (not processed)");
					break;
				default:
					log(LogLevel::Debug, "FiConsumerApp::processObjectUpdates() unhandled typeValue=" + std::to_string(typeValue));
					break;
				}
			} catch (...) { /* ignore individual parse issues */ }
		}
	}
}

void FiConsumerApp::processEventUpdates(const genfi::NotifyEventGroup& g) {
	// Get the object type
	int typeValue = -1;
	if (std::holds_alternative<int>(g.objects.type)) {
		typeValue = std::get<int>(g.objects.type);
	}
	else if (std::holds_alternative<std::string>(g.objects.type)) {
		// String type - could map to enum, but for now handle directly
		const std::string& typeStr = std::get<std::string>(g.objects.type);
		log(LogLevel::Trace, "NotifyEvent with string type: " + typeStr);
	}

	log(LogLevel::Trace, "Processing NotifyEvent typeValue: " + std::to_string(typeValue));

	// Process events based on object type
	switch (typeValue) {
	case static_cast<int>(::tlcfi::tlcfi::TLCObjectType::SpecialVehicleEventGenerator): {
		// Parse and dispatch special vehicle events
		for (const auto& evtJson : g.events) {
			try {
				auto sve = evtJson.get<::tlcfi::tlcfi::SpecialVehicleEvent>();
				log(LogLevel::Debug, "SpecialVehicleEvent received: type=" +
					std::to_string(sve.type.has_value() ? static_cast<int>(*sve.type) : -1) +
					" vehType=" + std::to_string(sve.vehType.has_value() ? static_cast<int>(*sve.vehType) : -1) +
					" lineNr=" + std::to_string(sve.lineNr.has_value() ? *sve.lineNr : -1) +
					" directionSG=" + std::to_string(sve.directionSG.has_value() ? *sve.directionSG : -1));
				if (onSpecialVehicleEvent) {
					onSpecialVehicleEvent(sve);
				}
			}
			catch (const std::exception& ex) {
				log(LogLevel::Error, "SpecialVehicleEvent parse error: " + std::string(ex.what()));
			}
		}
		break; }
	case static_cast<int>(::tlcfi::tlcfi::TLCObjectType::Session): {
		// Session events (lifecycle + failure) are conveyed via NotifyEvent with
		// objects.type = Session. See GEN-FI §6.5 and TLC-FI §5.2.6 / §7.7 / §7.9.
		for (const auto& evtJson : g.events) {
			try {
				auto ev = evtJson.get<genfi::SessionEvent>();
				std::string reason = sessionEventCodeToString(ev.code);
				if (ev.info.has_value()) {
					reason += " [type=" + std::to_string(ev.info->type) + " id=" + ev.info->id +
					          " attr=" + ev.info->attribute + "]";
				}
				log(LogLevel::Info, "SessionEvent code=" + std::to_string(ev.code) + " (" + reason + ")");
				if (onSessionEvent) {
					onSessionEvent(ev);
				}
				// Codes >= 1000 are TLC-FI/RIS-FI failure codes; lifecycle codes
				// (Deregistered, FacilitiesStopping) are not errors.
				if (ev.code >= 1000) {
					log(LogLevel::Error, "Facilities reported SessionEvent failure: " + reason);
					if (onSessionError) {
						onSessionError(ev.code, reason);
					}
				}
				handleSessionEvent(ev);
			}
			catch (const std::exception& ex) {
				log(LogLevel::Error, "SessionEvent parse error: " + std::string(ex.what()));
			}
		}
		break; }
	default:
		log(LogLevel::Debug, "FiConsumerApp::processEventUpdates() unhandled typeValue=" + std::to_string(typeValue));
		break;
	}
}

std::string FiConsumerApp::sessionEventCodeToString(int code) {
	switch (code) {
	case static_cast<int>(genfi::SessionEventCode::Deregistered): return "Deregistered";
	case static_cast<int>(genfi::SessionEventCode::FacilitiesStopping): return "FacilitiesStopping";
	case static_cast<int>(::tlcfi::tlcfi::SessionEventCodeExt::UpdateStateFailedIncorrectControlState):
		return "UpdateStateFailedIncorrectControlState";
	case static_cast<int>(::tlcfi::tlcfi::SessionEventCodeExt::UpdateStateFailedIncorrectApplicationType):
		return "UpdateStateFailedIncorrectApplicationType";
	case static_cast<int>(::tlcfi::tlcfi::SessionEventCodeExt::UpdateStateFailedIncorrectIntersection):
		return "UpdateStateFailedIncorrectIntersection";
	default: return "Unknown(" + std::to_string(code) + ")";
	}
}

void FiConsumerApp::handleSessionEvent(const genfi::SessionEvent& ev) {
	// Base implementation is a no-op; derived classes react (e.g. FiControllerApp
	// resets control-flow guards on a failure code).
	(void)ev;
}

void FiConsumerApp::startAliveLoop() {
	aliveKeepRunning_.store(true);
	aliveThread_ = std::thread([this]() {
		auto startTime = std::chrono::steady_clock::now();
		constexpr genfi::Ticks MAX_TICKS = 4294967295ULL; // 2^32 - 1 (per GEN-FI spec)
		constexpr auto ALIVE_INTERVAL = std::chrono::seconds(2);
		constexpr unsigned int MAX_CONSECUTIVE_FAILURES = 2;
		unsigned int consecutiveFailures = 0;
		
		while (aliveKeepRunning_.load()) {
			const auto now = std::chrono::steady_clock::now();
			const auto nextSend = now + ALIVE_INTERVAL;
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				now - startTime).count();
			
			// Wrap ticks to the specified range (0 to 4294967295) per GEN-FI spec
			// This ensures proper wraparound and interval calculations work correctly
			genfi::Ticks ticks = static_cast<genfi::Ticks>(elapsed % (MAX_TICKS + 1));
			
			genfi::AliveObject a;
			a.ticks = ticks;
			a.time = static_cast<genfi::Timestamp>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::system_clock::now().time_since_epoch()).count());
			
			// Check again before sending to avoid sending during shutdown
			if (!aliveKeepRunning_.load()) break;
			
			bool aliveSucceeded = false;
			try {
				auto fut = fi_->alive(a, std::chrono::milliseconds(3000));
				const auto status = fut.wait_until(nextSend);
				if (!aliveKeepRunning_.load()) {
					break;
				}
				if (status != std::future_status::ready) {
					log(LogLevel::Error, "alive timeout");
				}
				else {
					auto echo = fut.get();
					aliveSucceeded = true;
					log(LogLevel::Trace, "alive echo: ticks=" + std::to_string(echo.ticks) + " time=" + std::to_string(echo.time));
				}
			}
			catch (const std::exception& ex) {
				log(LogLevel::Error, "alive failed: " + std::string(ex.what()));
			}

			if (aliveSucceeded) {
				consecutiveFailures = 0;
			} else if (++consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
				log(LogLevel::Error, "alive watchdog failed twice; disconnecting for recovery");
				aliveKeepRunning_.store(false);
				auto transport = transport_;
				if (transport) {
					asio::post(io_, [transport]() { transport->stop(); });
				}
				break;
			}

			// The 2-second cadence is measured from send to send. Response
			// latency therefore consumes the current interval rather than being
			// added to it (GEN-FI §5.6/§5.7).
			while (aliveKeepRunning_.load() &&
			       std::chrono::steady_clock::now() < nextSend) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
		}
	});
}

void FiConsumerApp::stopAliveLoop() {
	aliveKeepRunning_.store(false);
	if (aliveThread_.joinable()) {
		try {
			aliveThread_.join();
		} catch (const std::exception& ex) {
			// Thread may have already been joined or doesn't exist - ignore
			log(LogLevel::Error, "Warning: alive thread join failed: " + std::string(ex.what()));
		}
	}
}


// Helper methods for object state management
void FiConsumerApp::updateSignalGroupState(const std::string& id, const nlohmann::json& stateUpdate, genfi::Ticks groupTicks) {
	std::function<void(const ::tlcfi::tlcfi::SignalGroup&)> callback;
	std::optional<::tlcfi::tlcfi::SignalGroup> updated;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
	
		// Get or create the SignalGroup instance
		auto& sg = signalGroups_[id];
		sg.id = id;
	
		// Update intersection if provided
		if (intersectionId_.has_value()) {
			sg.intersection = *intersectionId_;
		}
	
		// Track if state changed
		bool stateChanged = false;
	
		// Apply state updates
		if (stateUpdate.contains("state") && stateUpdate["state"].is_number_integer()) {
			auto newState = static_cast<::tlcfi::tlcfi::SignalGroupState>(stateUpdate["state"].get<int>());
			stateChanged = (sg.state != newState);
			sg.state = newState;
		}
	
		// Update stateticks: use provided value, or group ticks if state changed but stateticks not provided
		if (stateUpdate.contains("stateticks") && stateUpdate["stateticks"].is_number_integer()) {
			sg.stateticks = stateUpdate["stateticks"].get<genfi::Ticks>();
		} else if (stateChanged) {
			// State changed but stateticks not provided - use group ticks as fallback
			sg.stateticks = groupTicks;
		}
	
		if (stateUpdate.contains("reqState") && stateUpdate["reqState"].is_number_integer()) {
			sg.reqState = static_cast<::tlcfi::tlcfi::SignalGroupState>(stateUpdate["reqState"].get<int>());
		}
	
		if (onSignalGroupUpdate) {
			callback = onSignalGroupUpdate;
			updated = sg;
		}
	}
	if (callback && updated.has_value()) {
		callback(*updated);
	}
}

void FiConsumerApp::updateOutputState(const std::string& id, const nlohmann::json& stateUpdate, genfi::Ticks groupTicks) {
	std::function<void(const ::tlcfi::tlcfi::Output&)> callback;
	std::optional<::tlcfi::tlcfi::Output> updated;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
	
		// Get or create the Output instance
		auto& output = outputs_[id];
		output.id = id;
	
		// Update intersection if provided
		if (intersectionId_.has_value()) {
			output.intersection = *intersectionId_;
		}
	
		// Track if state changed
		bool stateChanged = false;
	
		// Apply state updates
		if (stateUpdate.contains("state") && stateUpdate["state"].is_number_integer()) {
			auto newState = stateUpdate["state"].get<int>();
			stateChanged = (output.state != std::optional<int>(newState));
			output.state = newState;
		}
	
		// Update stateticks: use provided value, or group ticks if state changed but stateticks not provided
		if (stateUpdate.contains("stateticks") && stateUpdate["stateticks"].is_number_integer()) {
			output.stateticks = stateUpdate["stateticks"].get<genfi::Ticks>();
		} else if (stateChanged) {
			// State changed but stateticks not provided - use group ticks as fallback
			output.stateticks = groupTicks;
		}
	
		if (stateUpdate.contains("reqState") && stateUpdate["reqState"].is_number_integer()) {
			output.reqState = stateUpdate["reqState"].get<int>();
		}
	
		if (onOutputUpdate) {
			callback = onOutputUpdate;
			updated = output;
		}
	}
	if (callback && updated.has_value()) {
		callback(*updated);
	}
}

void FiConsumerApp::updateDetectorState(const std::string& id, const nlohmann::json& stateUpdate, genfi::Ticks groupTicks) {
	std::function<void(const ::tlcfi::tlcfi::Detector&)> callback;
	std::optional<::tlcfi::tlcfi::Detector> updated;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
	
		// Get or create the Detector instance
		auto& detector = detectors_[id];
		detector.id = id;
	
		// Track if state changed
		bool stateChanged = false;
	
		// Apply state updates
		if (stateUpdate.contains("state") && stateUpdate["state"].is_number_integer()) {
			auto newState = static_cast<::tlcfi::tlcfi::DetectorState>(stateUpdate["state"].get<int>());
			stateChanged = (detector.state != newState);
			detector.state = newState;
		}
	
		// Update stateticks: use provided value, or group ticks if state changed but stateticks not provided
		if (stateUpdate.contains("stateticks") && stateUpdate["stateticks"].is_number_integer()) {
			detector.stateticks = stateUpdate["stateticks"].get<genfi::Ticks>();
		} else if (stateChanged) {
			// State changed but stateticks not provided - use group ticks as fallback
			detector.stateticks = groupTicks;
		}

		// SWICO (software input commando) is forwarded to the C controller via
		// CIF_IS_SWICO[]; without parsing it here the field is dropped at the JSON
		// boundary and the controller always reads CIF_IS_SWICO_GEEN.
		if (stateUpdate.contains("swico") && stateUpdate["swico"].is_number_integer()) {
			detector.swico = static_cast<::tlcfi::tlcfi::SwicoState>(stateUpdate["swico"].get<int>());
		}

		if (onDetectorUpdate) {
			callback = onDetectorUpdate;
			updated = detector;
		}
	}
	if (callback && updated.has_value()) {
		callback(*updated);
	}
}

void FiConsumerApp::updateInputState(const std::string& id, const nlohmann::json& stateUpdate, genfi::Ticks groupTicks) {
	std::function<void(const ::tlcfi::tlcfi::Input&)> callback;
	std::optional<::tlcfi::tlcfi::Input> updated;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
	
		// Get or create the Input instance
		auto& input = inputs_[id];
		input.id = id;
	
		// Track if state changed
		bool stateChanged = false;
	
		// Apply state updates
		if (stateUpdate.contains("state") && stateUpdate["state"].is_number_integer()) {
			auto newState = stateUpdate["state"].get<int>();
			stateChanged = (input.state != newState);
			input.state = newState;
		}
	
		// Update stateticks: use provided value, or group ticks if state changed but stateticks not provided
		if (stateUpdate.contains("stateticks") && stateUpdate["stateticks"].is_number_integer()) {
			input.stateticks = stateUpdate["stateticks"].get<genfi::Ticks>();
		} else if (stateChanged) {
			// State changed but stateticks not provided - use group ticks as fallback
			input.stateticks = groupTicks;
		}

		// SWICO (software input commando) is forwarded to the C controller via
		// CIF_IS_SWICO[]; without parsing it here the field is dropped at the JSON
		// boundary and the controller always reads CIF_IS_SWICO_GEEN.
		if (stateUpdate.contains("swico") && stateUpdate["swico"].is_number_integer()) {
			input.swico = static_cast<::tlcfi::tlcfi::SwicoState>(stateUpdate["swico"].get<int>());
		}

		if (onInputUpdate) {
			callback = onInputUpdate;
			updated = input;
		}
	}
	if (callback && updated.has_value()) {
		callback(*updated);
	}
}

void FiConsumerApp::log(LogLevel level, const std::string& message) {
	if (onLog) {
		onLog(level, message);
	}
}

bool FiConsumerApp::handlesInitInternally() const { return false; }

void FiConsumerApp::log(LogLevel level, const char* message) {
	log(level, std::string(message));
}

} // namespace tlcfi::fi

