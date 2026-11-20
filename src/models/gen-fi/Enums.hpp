#pragma once

namespace tlcfi::genfi {

enum class ApplicationType : int {
	Consumer = 0,
	Provider = 1,
	Control = 2,
};

enum class SessionEventCode : int {
	Deregistered = 0,
	FacilitiesStopping = 1,
};

} // namespace tlcfi::genfi


