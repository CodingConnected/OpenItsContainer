#include "fi/FiAppHost.hpp"

#include <iostream>
#include <thread>

namespace tlcfi::fi {

FiAppHost::FiAppHost(FiConsumerApp& app, const Config& config)
	: app_(app), config_(config), currentRetryDelay_(config.initialRetryDelay), firstConnection_(true) {
	
	// Set up default logging callback (prints to stdout/stderr)
	onLog = [](LogLevel level, const std::string& message) {
		switch (level) {
		case LogLevel::Trace:
			std::cout << "[TRACE] " << message << "\n";
			break;
		case LogLevel::Debug:
			std::cout << "[DEBUG] " << message << "\n";
			break;
		case LogLevel::Info:
			std::cout << "[INFO] " << message << "\n";
			break;
		case LogLevel::Error:
			std::cerr << "[ERROR] " << message << "\n";
			break;
		case LogLevel::Fatal:
			std::cerr << "[FATAL] " << message << "\n";
			break;
		}
		std::cout.flush();
		std::cerr.flush();
	};

	// Forward FiConsumerApp logs to this host's log method
	app_.onLog = [this](LogLevel level, const std::string& message) {
		this->log(level, message);
	};

	// Set up default callbacks if enabled
	if (config_.enableDefaultCallbacks) {
		setupDefaultCallbacks();
	}
}

FiAppHost::~FiAppHost() {
	// Cleanup is handled by the application's destructor
}

void FiAppHost::run(std::atomic<bool>& shouldRun) {
	log(LogLevel::Info, "=== Starting Connection Retry Loop ===");
	log(LogLevel::Info, "Press Ctrl+C to stop...");

	while (shouldRun.load()) {
		if (attemptConnection()) {
			// Success! Reset retry delay
			currentRetryDelay_ = config_.initialRetryDelay;

			if (firstConnection_) {
				if (app_.handlesInitInternally()) {
					log(LogLevel::Info, "Registered successfully, waiting for state machine...");
				} else {
					log(LogLevel::Info, "Subscribed successfully!");
				}
				log(LogLevel::Info, "=== Monitoring State Updates ===");
				firstConnection_ = false;
			} else {
				log(LogLevel::Info, "Reconnected successfully!");
			}

			// Monitor for disconnections
			// Apps that handle init internally stay connected while the state machine runs;
			// use isConnected() instead of isFullyInitialized() to avoid premature exit.
			if (app_.handlesInitInternally()) {
				while (shouldRun.load() && app_.isConnected()) {
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
				}
			} else {
				while (shouldRun.load() && app_.isFullyInitialized()) {
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
				}
			}

			// If we exited due to disconnection (not shutdown), restart connection sequence
			if (!app_.isConnected() && shouldRun.load()) {
				log(LogLevel::Info, "Connection lost. Reconnecting...");
				// The next connect() call will handle full cleanup via cleanup()
				currentRetryDelay_ = config_.initialRetryDelay; // Reset delay on disconnect
			}
		} else {
			// Connection attempt failed - wait with exponential backoff
			log(LogLevel::Error, "Connection attempt failed. Retrying in " + 
				std::to_string(currentRetryDelay_.count()) + "ms...");
			// The next connect() call will handle full cleanup via cleanup()
			waitWithBackoff(currentRetryDelay_, shouldRun);
			
			// Double the delay for next time, cap at max
			currentRetryDelay_ = std::min(currentRetryDelay_ * 2, config_.maxRetryDelay);
		}
	}

	log(LogLevel::Info, "Stopping application...");
	app_.stop();
	log(LogLevel::Info, "Shutdown complete.");
}

bool FiAppHost::attemptConnection() {
	// Step 1: Connect to Facilities
	log(LogLevel::Info, "Connecting...");
	if (!app_.connect(config_.connectTimeout)) {
		log(LogLevel::Error, "Failed to connect to Facilities");
		return false;
	}
	log(LogLevel::Info, "Connected successfully!");

	// Step 2: Register application
	log(LogLevel::Info, "Registering application...");
	if (!app_.registerApp(config_.operationTimeout)) {
		log(LogLevel::Error, "Failed to register with Facilities");
		return false;
	}
	log(LogLevel::Info, "Registered successfully!");

	if (!app_.handlesInitInternally()) {
		// Step 3: Read meta data to discover objects
		log(LogLevel::Info, "Reading meta data...");
		if (!app_.readMeta(config_.operationTimeout)) {
			log(LogLevel::Error, "Failed to read meta data");
			return false;
		}

		// Step 4: Subscribe to all discovered objects
		log(LogLevel::Info, "Subscribing to all objects...");
		if (!app_.subscribeAll(config_.operationTimeout)) {
			log(LogLevel::Error, "Failed to subscribe to objects");
			return false;
		}
	}
	// else: app handles meta/subscribe via its own state machine

	return true;
}

void FiAppHost::waitWithBackoff(std::chrono::milliseconds delay, std::atomic<bool>& shouldRun) {
	auto start = std::chrono::steady_clock::now();
	while (shouldRun.load() && 
	       (std::chrono::steady_clock::now() - start) < delay) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

void FiAppHost::log(LogLevel level, const std::string& message) {
	if (onLog) {
		onLog(level, message);
	}
}

void FiAppHost::setupDefaultCallbacks() {
	// Set up default callbacks that use the logging system
	app_.onSignalGroupUpdate = [this](const ::tlcfi::tlcfi::SignalGroup& sg) {
		std::string msg = "[SignalGroup] " + sg.id + " - state: " + 
			std::to_string(static_cast<int>(sg.state)) + 
			" (ticks: " + std::to_string(sg.stateticks) + ")";
		log(LogLevel::Debug, msg);
	};

	app_.onOutputUpdate = [this](const ::tlcfi::tlcfi::Output& out) {
		std::string msg = "[Output] " + out.id + " - state: ";
		if (out.state.has_value()) {
			msg += std::to_string(*out.state);
		} else {
			msg += "N/A";
		}
		msg += " (ticks: " + std::to_string(out.stateticks) + ")";
		log(LogLevel::Debug, msg);
	};

	app_.onDetectorUpdate = [this](const ::tlcfi::tlcfi::Detector& det) {
		std::string msg = "[Detector] " + det.id + " - state: " + 
			std::to_string(static_cast<int>(det.state)) + 
			" (ticks: " + std::to_string(det.stateticks) + ")";
		log(LogLevel::Debug, msg);
	};

	app_.onInputUpdate = [this](const ::tlcfi::tlcfi::Input& inp) {
		std::string msg = "[Input] " + inp.id + " - state: " + 
			std::to_string(inp.state) + 
			" (ticks: " + std::to_string(inp.stateticks) + ")";
		log(LogLevel::Debug, msg);
	};

	app_.onMetaReceived = [this](const std::vector<std::string>& sgIds,
	                             const std::vector<std::string>& outIds,
	                             const std::vector<std::string>& detIds,
	                             const std::vector<std::string>& inIds) {
		std::string msg = "\n=== Meta Data Received ===\n";
		msg += "Signal Groups: " + std::to_string(sgIds.size()) + " (";
		for (size_t i = 0; i < sgIds.size() && i < 5; ++i) {
			if (i > 0) msg += ", ";
			msg += sgIds[i];
		}
		if (sgIds.size() > 5) msg += ", ...";
		msg += ")\n";

		msg += "Outputs: " + std::to_string(outIds.size()) + " (";
		for (size_t i = 0; i < outIds.size() && i < 5; ++i) {
			if (i > 0) msg += ", ";
			msg += outIds[i];
		}
		if (outIds.size() > 5) msg += ", ...";
		msg += ")\n";

		msg += "Detectors: " + std::to_string(detIds.size()) + " (";
		for (size_t i = 0; i < detIds.size() && i < 5; ++i) {
			if (i > 0) msg += ", ";
			msg += detIds[i];
		}
		if (detIds.size() > 5) msg += ", ...";
		msg += ")\n";

		msg += "Inputs: " + std::to_string(inIds.size()) + " (";
		for (size_t i = 0; i < inIds.size() && i < 5; ++i) {
			if (i > 0) msg += ", ";
			msg += inIds[i];
		}
		if (inIds.size() > 5) msg += ", ...";
		msg += ")\n";

		log(LogLevel::Info, msg);
	};
}

} // namespace tlcfi::fi

