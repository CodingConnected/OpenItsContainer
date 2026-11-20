#pragma once

#include "../gen-fi/GenFiTypes.hpp"
#include <optional>

namespace tlcfi::tlcfi {

// Special Vehicle auxiliary types
using ActivationPointNr = int;     // 0..32767
using CompanyNumber = int;         // 0..255
using DirectionSG = int;           // 0..255 (signal group number)
using DistanceToStopline = int;    // -99..9999 meters
using JourneyCategory = int;       // 0..99
using JourneyNumber = int;         // 0..9999
using LineNumber = int;            // 0..9999
using PunctualityTime = int;       // -3600..3600 seconds
using RoutePublicTransport = int;  // 0..99
using ServiceNumber = int;         // 0..9999
using VehicleId = int;             // 0..32767

struct DateTime {
	std::optional<int> y;
	std::optional<int> m;
	std::optional<int> d;
	std::optional<int> h;
	std::optional<int> min;
	std::optional<int> s;
	std::optional<int> ms;
};

} // namespace tlcfi::tlcfi


