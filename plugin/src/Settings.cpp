#include "Settings.h"

namespace SDS
{
	namespace
	{
		Settings instance;

		std::string Trim(std::string s)
		{
			const auto ws = " \t\r\n";
			s.erase(0, s.find_first_not_of(ws));
			s.erase(s.find_last_not_of(ws) + 1);
			return s;
		}

		bool  ToBool(const std::string& v) { return v == "1" || v == "true" || v == "True" || v == "TRUE"; }
		float ToFloat(const std::string& v, float a_min) { return (std::max)(a_min, std::stof(v)); }

		void Apply(Settings& s, const std::string& section, const std::string& key, const std::string& value)
		{
			const auto k = section + "." + key;
			if (k == "General.bEnabled") s.enabled = ToBool(value);
			else if (k == "TargetLock.fTurnSpeed") s.turnSpeed = ToFloat(value, 0.0f);
			else if (k == "TargetLock.iTarget") s.target = std::clamp(std::stoi(value), 0, 2);
			else if (k == "TargetLock.fMaxDistance") s.maxDistance = ToFloat(value, 0.0f);
			else if (k == "TargetLock.iMethod") s.lockMethod = std::clamp(std::stoi(value), 0, 1);
			else if (k == "AnimMotion.bEnabled") s.animMotion = ToBool(value);
			else if (k == "AnimMotion.fMinStep") s.animMotionMinStep = ToFloat(value, 0.0f);
			else if (k == "Bump.bEnabled") s.bumpAnnotations = ToBool(value);
			else if (k == "ReachTarget.fMinScale") s.reachMinScale = ToFloat(value, 0.0f);
			else if (k == "ReachTarget.fMaxScale") s.reachMaxScale = ToFloat(value, 0.0f);
			else if (k == "TargetLock.bStraightRootMotion") s.straightRootMotion = ToBool(value);
			else if (k == "TargetLock.fStraightMaxAngle") s.straightMaxAngle = std::clamp(ToFloat(value, 0.0f), 0.0f, 180.0f);
			else if (k == "TargetLock.fMaxEngineTurnSpeed") s.maxEngineTurnSpeed = ToFloat(value, 10.0f);
			else if (k == "StopRotation.bZeroAIRotation") s.zeroAIRotation = ToBool(value);
			else if (k == "StopRotation.bHoldHeading") s.holdHeading = ToBool(value);
			else if (k == "Compatibility.bLegacyTCB") s.legacyTCB = ToBool(value);
			else if (k == "Log.bLogAnnotations") s.logAnnotations = ToBool(value);
			else if (k == "Log.bLogTransitions") s.logTransitions = ToBool(value);
			else if (k == "Debug.bDiagnostics") s.diagnostics = ToBool(value);
			else if (k == "Debug.bTestDraw") s.testDraw = ToBool(value);
			else if (k == "Debug.bFallbackDriver") s.fallbackDriver = ToBool(value);
			else if (k == "Debug.bMovementCsv") s.movementCsv = ToBool(value);
			else if (k == "LockDebug.bEnabled") s.lockDebug = ToBool(value);
			else if (k == "LockDebug.bCombat") s.combatDebug = ToBool(value);
			else if (k == "LockDebug.fArrowLength") s.lockArrowLength = ToFloat(value, 10.0f);
			else if (k == "LockDebug.fThickness") s.lockThickness = ToFloat(value, 0.5f);
			else if (k == "LockDebug.bDrawBody") s.lockDrawBody = ToBool(value);
			else if (k == "MotionFix.bEnabled") s.motionFix = ToBool(value);
			else if (k == "MotionFix.fMinTurn") s.motionFixMinTurn = ToFloat(value, 0.0f);
			else if (k == "MotionFix.fTolerance") s.motionFixTolerance = ToFloat(value, 1.0f);
			else if (k == "MotionFix.fMinStep") s.motionFixMinStep = ToFloat(value, 0.1f);
			else if (k == "MotionDebug.bEnabled") s.motionDebug = ToBool(value);
			else if (k == "MotionDebug.bOnlyFightingPlayer") s.motionOnlyFightingPlayer = ToBool(value);
			else if (k == "MotionDebug.fMaxDistance") s.motionMaxDistance = ToFloat(value, 0.0f);
			else if (k == "MotionDebug.bCsv") s.motionCsv = ToBool(value);
			else if (k == "MotionDebug.bDraw") s.motionDraw = ToBool(value);
			else if (k == "MotionDebug.fPersistSeconds") s.motionPersistSeconds = ToFloat(value, 0.0f);
			else if (k == "MotionDebug.fThickness") s.motionThickness = ToFloat(value, 0.5f);
			else if (k == "MotionDebug.fZOffset") s.motionZOffset = std::stof(value);
			else if (k == "MotionDebug.bDrawActual") s.motionDrawActual = ToBool(value);
			else if (k == "MotionDebug.bDrawClipFrame") s.motionDrawClipFrame = ToBool(value);
			else if (k == "MotionDebug.bDrawCurrentFrame") s.motionDrawCurrentFrame = ToBool(value);
			else if (k == "MotionDebug.bDrawRootFrame") s.motionDrawRootFrame = ToBool(value);
			else if (k == "MotionDebug.bDrawIdeal") s.motionDrawIdeal = ToBool(value);
			else if (k == "MotionDebug.bDrawArrows") s.motionDrawArrows = ToBool(value);
			else if (k == "MotionDebug.fAnimRotationSign") s.animRotationSign = std::stof(value) < 0.0f ? -1.0f : 1.0f;
			else logs::warn("unknown key [{}] {}", section, key);
		}
	}

	const Settings& Settings::Get() { return instance; }

	void Settings::Load()
	{
		Settings loaded;

		std::ifstream in{ std::string(Path) };
		if (!in) {
			logs::warn("{} not found, using defaults", Path);
		}

		std::string line;
		std::string section;
		bool        first = true;
		while (in && std::getline(in, line)) {
			if (first) {  // tolerate a UTF-8 BOM
				if (line.rfind("\xEF\xBB\xBF", 0) == 0) {
					line.erase(0, 3);
				}
				first = false;
			}
			line = Trim(line);
			if (line.empty() || line[0] == ';' || line[0] == '#') {
				continue;
			}
			if (line[0] == '[') {
				section = Trim(line.substr(1, line.find(']') - 1));
				continue;
			}
			const auto eq = line.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			const auto key = Trim(line.substr(0, eq));
			auto       value = Trim(line.substr(eq + 1));
			if (const auto sc = value.find(';'); sc != std::string::npos) {
				value = Trim(value.substr(0, sc));
			}
			try {
				Apply(loaded, section, key, value);
			} catch (const std::exception&) {
				logs::warn("bad value for [{}] {}: '{}'", section, key, value);
			}
		}

		instance = loaded;
		logs::info("settings: lock method={} ({}) maxEngineTurnSpeed={}", loaded.lockMethod,
			loaded.lockMethod == 1 ? "AI turn speed" : "SetHeading", loaded.maxEngineTurnSpeed);
		logs::info("settings: enabled={} | lock turnSpeed={} target={} maxDistance={} | stop zeroAI={} hold={} | legacyTCB={} | "
				   "log annotations={} transitions={} | lockDebug={} | motionDebug={} draw={} onlyFightingPlayer={}",
			loaded.enabled, loaded.turnSpeed, loaded.target, loaded.maxDistance, loaded.zeroAIRotation, loaded.holdHeading,
			loaded.legacyTCB, loaded.logAnnotations, loaded.logTransitions, loaded.lockDebug, loaded.motionDebug, loaded.motionDraw,
			loaded.motionOnlyFightingPlayer);
	}
}
