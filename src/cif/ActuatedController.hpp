#pragma once

#include <string>
#include <vector>

namespace cif {

/**
 * @brief Configuration for a single detector
 */
struct DetectorConfig {
    std::string id;    ///< TLC-FI detector ID (e.g., "d1")
    int index;         ///< CIF_IS[] array index
};

/**
 * @brief Configuration for a single input
 */
struct InputConfig {
    std::string id;    ///< TLC-FI input ID (e.g., "in1")
    int index;         ///< CIF_IS[] array index (offset by detector count)
};

/**
 * @brief Configuration for a single output
 */
struct OutputConfig {
    std::string id;    ///< TLC-FI output ID (e.g., "out1")
    int index;         ///< CIF_GUS[] array index (offset by signal group count)
};

/**
 * @brief Configuration for a conflict between two signal groups
 *
 * Defines an intergreen time from one signal group to another.
 * The intergreen time starts when the "from" signal group ends green.
 * While the intergreen timer is running OR while the "from" signal group
 * is green, the "to" signal group may not become green.
 */
struct ConflictConfig {
    std::string fromId;    ///< Signal group ID that triggers the intergreen
    std::string toId;      ///< Signal group ID that is blocked
    int intergreen;        ///< Intergreen time in tenths of seconds
};

/**
 * @brief Configuration for a single signal group
 */
struct SignalGroupConfig {
    std::string id;                        ///< TLC-FI signal group ID (e.g., "sg1")
    int index;                             ///< CIF_GUS[]/CIF_WUS[] array index
    int minGreen;                          ///< Minimum green time in tenths of seconds
    int maxGreen;                          ///< Maximum green time in tenths of seconds
    int amber;                             ///< Fixed amber time in tenths of seconds
    int minRed = 20;                       ///< Minimum red time in tenths of seconds (default: 20)
    std::vector<std::string> detectorIds;  ///< IDs of detectors belonging to this signal group
    int direction = -1;                    ///< DSI direction number (-1 = no priority support)
    int priorityMaxGreen = 180;            ///< Max green when priority vehicles waiting (tenths of sec)
};

/**
 * @brief Full intersection configuration
 *
 * Describes all signal groups, detectors, inputs, outputs, and conflicts
 * for an intersection. Can be loaded from JSON.
 */
struct IntersectionConfig {
    std::vector<SignalGroupConfig> signalGroups;
    std::vector<DetectorConfig> detectors;
    std::vector<InputConfig> inputs;
    std::vector<OutputConfig> outputs;
    std::vector<ConflictConfig> conflicts;  ///< Intergreen conflicts between signal groups
};

/**
 * @brief Load intersection configuration from a JSON string.
 *
 * Expected format:
 * @code
 * {
 *   "signalGroups": [
 *     {
 *       "id": "sg1",
 *       "index": 0,
 *       "minGreen": 50,
 *       "maxGreen": 150,
 *       "amber": 30,
 *       "detectors": ["d1"]
 *     }
 *   ],
 *   "detectors": [
 *     { "id": "d1", "index": 0 }
 *   ],
 *   "inputs": [
 *     { "id": "in1", "index": 0 }
 *   ],
 *   "outputs": [
 *     { "id": "out1", "index": 0 }
 *   ],
 *   "conflicts": [
 *     { "from": "sg1", "to": "sg2", "intergreen": 40 }
 *   ]
 * }
 * @endcode
 *
 * Times are in tenths of seconds (50 = 5.0 seconds).
 *
 * Conflicts define intergreen times between signal groups. The intergreen time
 * starts when the "from" signal group ends green (transitions to amber).
 * While the intergreen timer is running OR while the "from" signal group is
 * green, the "to" signal group may not become green.
 *
 * IDs must match exactly (case-sensitive) with IDs from the TLC.
 *
 * @param json JSON string to parse
 * @return Parsed intersection configuration
 * @throws std::runtime_error on parse errors
 */
IntersectionConfig loadConfigFromJson(const std::string& json);

/**
 * @brief Set the active intersection configuration.
 *
 * Must be called before applicatieprogramma(CIF_INIT) to configure the
 * controller's signal groups, detectors, and timing parameters.
 *
 * @param config The intersection configuration to use
 */
void setIntersectionConfig(const IntersectionConfig& config);

/**
 * @brief Get the current intersection configuration.
 * @return Reference to the active configuration
 */
const IntersectionConfig& getIntersectionConfig();

} // namespace cif
