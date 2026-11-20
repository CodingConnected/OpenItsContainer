#include "runtime/RuntimeApp.hpp"
#include "cif/ActuatedController.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

/**
 * @brief Read entire file contents into a string
 */
static std::string readFile(const std::string& path) {
    std::cout << "Reading file: " << path << "\n";
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + path);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

/**
 * @brief Example TLC-FI controller hosting the bundled actuated controller.
 *
 * This is a thin consumer of tlcfi::runtime::runContainer(): the generic
 * container lifecycle (connect, bridge, control loop, retry, logging) lives in
 * the runtime library. The only example-specific work is loading this
 * controller's intersection configuration from JSON before init() — done in
 * the pre-init hook below. The actuated controller (linked via
 * actuated_controller) provides the CIF buffers, applicatieprogramma, and
 * getCifItemName.
 *
 * The config path defaults to controller_config.json and can be overridden
 * with --config <path>. All other options are the runtime's (see RuntimeApp.hpp).
 *
 * @note The JSON config is solely for this example actuated controller.
 *       External controllers supply their object mapping via getCifItemName()
 *       and do not use any JSON config.
 */
int main(int argc, char* argv[]) {
    auto loadActuatedConfig = [](const tlcfi::runtime::RuntimeOptions& opts) {
        const std::string configPath =
            opts.configPath.empty() ? "controller_config.json" : opts.configPath;

        std::cout << "Loading controller configuration...\n";
        std::string configJson = readFile(configPath);
        auto config = cif::loadConfigFromJson(configJson);
        cif::setIntersectionConfig(config);

        std::cout << "Configured " << config.signalGroups.size() << " signal groups:\n";
        for (const auto& sg : config.signalGroups) {
            std::cout << "  - " << sg.id << " (index " << sg.index
                      << ", minG=" << sg.minGreen << ", maxG=" << sg.maxGreen
                      << ", amber=" << sg.amber << ", detectors=[";
            for (size_t i = 0; i < sg.detectorIds.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << sg.detectorIds[i];
            }
            std::cout << "])\n";
        }
        std::cout << "Configured " << config.detectors.size() << " detectors:\n";
        for (const auto& det : config.detectors) {
            std::cout << "  - " << det.id << " (index " << det.index << ")\n";
        }
        std::cout << "Configured " << config.inputs.size() << " inputs:\n";
        for (const auto& inp : config.inputs) {
            std::cout << "  - " << inp.id << " (index " << inp.index << ")\n";
        }
        std::cout << "Configured " << config.outputs.size() << " outputs:\n";
        for (const auto& out : config.outputs) {
            std::cout << "  - " << out.id << " (index " << out.index << ")\n";
        }
        std::cout << "\n";
        std::cout.flush();
    };

    return tlcfi::runtime::runContainer(argc, argv, loadActuatedConfig);
}
