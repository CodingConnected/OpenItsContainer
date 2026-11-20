#pragma once

#include "Objects.hpp"

namespace tlcfi::tlcfi {

// Declarations for nlohmann::json conversions

void to_json(nlohmann::json& j, const DateTime& v);
void from_json(const nlohmann::json& j, DateTime& v);

void to_json(nlohmann::json& j, const ControlApplication& v);
void from_json(const nlohmann::json& j, ControlApplication& v);

void to_json(nlohmann::json& j, const SignalConflict& v);
void from_json(const nlohmann::json& j, SignalConflict& v);

void to_json(nlohmann::json& j, const SignalTiming& v);
void from_json(const nlohmann::json& j, SignalTiming& v);

#if defined(TLCFI_VERSION_1_1_0)
void to_json(nlohmann::json& j, const SignalGroupPrediction& v);
void from_json(const nlohmann::json& j, SignalGroupPrediction& v);
#endif // TLCFI_VERSION_1_1_0

void to_json(nlohmann::json& j, const Intersection& v);
void from_json(const nlohmann::json& j, Intersection& v);

void to_json(nlohmann::json& j, const Detector& v);
void from_json(const nlohmann::json& j, Detector& v);

void to_json(nlohmann::json& j, const Input& v);
void from_json(const nlohmann::json& j, Input& v);

void to_json(nlohmann::json& j, const Output& v);
void from_json(const nlohmann::json& j, Output& v);

void to_json(nlohmann::json& j, const SignalGroup& v);
void from_json(const nlohmann::json& j, SignalGroup& v);

void to_json(nlohmann::json& j, const DetectorEvent& v);
void from_json(const nlohmann::json& j, DetectorEvent& v);

void to_json(nlohmann::json& j, const SpecialVehicleEvent& v);
void from_json(const nlohmann::json& j, SpecialVehicleEvent& v);

} // namespace tlcfi::tlcfi



