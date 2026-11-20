#pragma once

namespace tlcfi::tlcfi {

enum class SwicoState : int { NoSwico = 0, SwicoOff = 1, SwicoOn = 2 };

enum class TLCObjectType : int {
	Session = 0,
	TLCFacilities = 1,
	Intersection = 2,
	SignalGroup = 3,
	Detector = 4,
	Input = 5,
	Output = 6,
	SpecialVehicleEventGenerator = 7,
	Variable = 8,
};

enum class HandoverCapability : int { Cleared = 0, PreDefined = 1, Direct = 2 };

enum class ControlState : int {
	Error = 0,
	NotConfigured = 1,
	Offline = 2,
	ReadyToControl = 3,
	StartControl = 4,
	InControl = 5,
	EndControl = 6,
};

enum class SessionEventCodeExt : int {
	UpdateStateFailedIncorrectControlState = 1000,
	UpdateStateFailedIncorrectApplicationType = 1001,
	UpdateStateFailedIncorrectIntersection = 1002,
};

enum class DetectorFaultState : int { None = 0, TooLongUnoccupied = 1, TooLongOccupied = 2, Flutter = 3, HardwareError = 4 };
enum class DetectorState : int { Unoccupied = 0, Occupied = 1 };
enum class DetectorDirection : int { Normal = 0, Reverse = 1 };

enum class InputFaultState : int { None = 0, HardwareError = 1 };

enum class IntersectionControlState : int {
	Error = 0,
	Dark = 1,
	Standby = 2,
	AlternativeStandby = 3,
	SwitchOn = 4,
	SwitchOff = 5,
	AllRed = 6,
	Control = 7,
};

enum class OutputFaultState : int { None = 0, HardwareError = 1 };

enum class SignalGroupState : int {
	Unavailable = 0,
	Dark = 1,
	StopThenProceed = 2,
	StopAndRemain = 3,
	PreMovement = 4,
	PermissiveMovementAllowed = 5,
	ProtectedMovementAllowed = 6,
	PermissiveClearance = 7,
	ProtectedClearance = 8,
	CautionConflictingTraffic = 9,
	PermissiveMovementPreClearance = 10,
	ProtectedMovementPreClearance = 11,
};

enum class DetectorClassification : int {
	Unknown = 0,
	Pedestrian = 1,
	Bicycle = 2,
	Motorcycle = 3,
	Car = 4,
	CarWithTrailer = 5,
	Lorry = 6,
	LorryWithTrailer = 7,
	Bus = 8,
	BusWithTrailer = 9,
	RoadTrain = 10,
};

enum class AnnouncementType : int { NoInformation = 0, Checkin = 1, Checkout = 2, PreCheckin = 3 };
enum class PriorityClass : int { NoInformation = 0, NoPriority = 1, Conditional = 2, Absolute = 3, AlarmLight = 4 };
enum class PunctualityClass : int { NoInformation = 0, Late = 1, OnTime = 2, Early = 3, OffSchedule = 4 };
enum class DetectorDirectionSG : int { Normal = 0, Reverse = 1 };

} // namespace tlcfi::tlcfi


