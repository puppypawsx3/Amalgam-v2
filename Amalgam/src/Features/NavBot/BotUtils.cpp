#include "BotUtils.h"
#include "../Simulation/MovementSimulation/MovementSimulation.h"
#include "../Players/PlayerUtils.h"
#include "../Misc/Misc.h"
#include "../Aimbot/AimbotGlobal/AimbotGlobal.h"
#include "../Misc/NamedPipe/NamedPipe.h"
#include "../Ticks/Ticks.h"
#include "../../SDK/Helpers/TraceFilters/TraceFilters.h"
#include "../../Utils/Math/Math.h"
#include "../../SDK/SDK.h"
#include <algorithm>
#include <cmath>

namespace
{
	float ApproachAngle(float target, float value, float step)
	{
		float delta = Math::NormalizeAngle(target - value);
		if (delta > step)
			delta = step;
		else if (delta < -step)
			delta = -step;
		float result = value + delta;
		return Math::NormalizeAngle(result);
	}

	float AngleDistance(float a, float b)
	{
		return fabsf(Math::NormalizeAngle(a - b));
	}

	float ApproachFloat(float target, float value, float step)
	{
		float delta = target - value;
		if (delta > step)
			delta = step;
		else if (delta < -step)
			delta = -step;
		return value + delta;
	}

}

bool CBotUtils::IsLineVisible(const Vec3& from, const Vec3& to) const
{
	CGameTrace trace = {};
	CTraceFilterWorldAndPropsOnly filter = {};
	SDK::Trace(from, to, MASK_SHOT | CONTENTS_GRATE, &filter, &trace);
	return trace.fraction > 0.97f;
}

void CBotUtils::ResetLookState()
{
	m_tLookState = {};
	m_tLookState.scanCooldown = SDK::RandomFloat(1.2f, 2.4f);
	m_tLookState.scanDuration = SDK::RandomFloat(0.6f, 1.2f);
	m_tLookState.pathOffsetInterval = SDK::RandomFloat(1.1f, 2.1f);
	m_tLookState.maxYawSpeed = SDK::RandomFloat(195.f, 255.f);
	m_tLookState.maxPitchSpeed = SDK::RandomFloat(125.f, 170.f);
	m_tLookState.speedVariance = SDK::RandomFloat(0.9f, 1.12f);
	m_tLookState.speedVarianceInterval = SDK::RandomFloat(0.45f, 1.2f);
	m_tLookState.curiosityCooldown = SDK::RandomFloat(6.f, 11.f);
	m_tLookState.curiosityDuration = SDK::RandomFloat(0.9f, 1.5f);
	m_tLookState.reactionTimer.Update();
	m_tLookState.scanCooldownTimer.Update();
	m_tLookState.pathOffsetTimer.Update();
	m_tLookState.scanHoldTimer.Update();
	m_tLookState.curiosityTimer.Update();
	m_tLookState.curiosityCooldownTimer.Update();
	m_tLookState.speedVarianceTimer.Update();
	m_tLookState.curiosityTurnSign = SDK::RandomInt(0, 1) ? 1.f : -1.f;
	m_tLookState.curiosityAggressive = false;
	m_tLookState.externalAngles = {};
	m_tLookState.externalRelease = 0.f;
	m_tLookState.externalActive = false;
	m_tLookState.assistActive = false;
	m_tLookState.assistAngles = {};
	m_tLookState.assistExpiry = 0.f;
	m_tLookState.assistSmooth = false;
	m_bPendingAssist = false;
	m_vPendingAssistAngles = {};
	m_flPendingAssistExpiry = 0.f;
	m_bPendingAssistSmooth = false;
}

void CBotUtils::RunLegitBot(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd)
{
	if (!Vars::Misc::Movement::LegitBot::Enabled.Value || !pLocal || !pCmd)
		return;

	if (!pLocal->IsAlive() || pLocal->IsAGhost())
	{
		m_bMedicTimerPrimed = false;
		m_mSpyInvisibility.clear();
		return;
	}

	UpdateSpyInvisibilityCache(pLocal);
	HandleCallForMedic(pLocal);
	const bool bAnglesLocked = HandleSnapToUncloak(pLocal, pCmd);
	HandleSpycheckPulse(pLocal, pWeapon, pCmd, bAnglesLocked);
}

void CBotUtils::HandleCallForMedic(CTFPlayer* pLocal)
{
	if (!Vars::Misc::Movement::LegitBot::CallForMedic.Value || !pLocal || !pLocal->IsAlive())
	{
		m_bMedicTimerPrimed = false;
		return;
	}

	if (!pLocal->IsInValidTeam() || pLocal->IsTaunting() || pLocal->InCond(TF_COND_HALLOWEEN_KART))
	{
		m_bMedicTimerPrimed = false;
		return;
	}

	const int iMaxHealth = std::max(pLocal->GetMaxHealth(), 1);
	const float flThresholdPct = std::clamp(Vars::Misc::Movement::LegitBot::CallForMedicHealth.Value, 1.f, 100.f) * 0.01f;
	const float flThreshold = std::max(1.f, static_cast<float>(iMaxHealth) * flThresholdPct);
	const int iHealth = pLocal->m_iHealth();

	if (iHealth > flThreshold)
	{
		m_bMedicTimerPrimed = false;
		return;
	}

	if (!m_bMedicTimerPrimed)
	{
		m_tCallMedicTimer -= Vars::Misc::Movement::LegitBot::CallForMedicCooldown.Value;
		m_bMedicTimerPrimed = true;
	}

	const float flCooldown = std::max(Vars::Misc::Movement::LegitBot::CallForMedicCooldown.Value, 0.5f);
	if (!m_tCallMedicTimer.Run(flCooldown))
		return;

	I::EngineClient->ClientCmd_Unrestricted("voicemenu 0 0");
}

bool CBotUtils::HandleSnapToUncloak(CTFPlayer* pLocal, CUserCmd* pCmd)
{
	if (!Vars::Misc::Movement::LegitBot::SnapToUncloak.Value || !pLocal || !pCmd)
		return false;

	if (G::SilentAngles || G::PSilentAngles || pLocal->IsTaunting() || pLocal->InCond(TF_COND_HALLOWEEN_KART))
		return false;

	const float flRadius = std::max(Vars::Misc::Movement::LegitBot::SnapDetectionRadius.Value, 1.f);
	const float flRadiusSqr = flRadius * flRadius;
	const Vec3 vLocalCenter = pLocal->GetCenter();

	CTFPlayer* pBestTarget = nullptr;
	Vec3 vBestAngles = {};
	float flBestScore = 0.f;

	for (auto pEntity : H::Entities.GetGroup(EGroupType::PLAYERS_ENEMIES))
	{
		auto pSpy = pEntity->As<CTFPlayer>();
		if (!pSpy || !pSpy->IsAlive() || pSpy->IsAGhost())
			continue;

		if (pSpy->m_iClass() != TF_CLASS_SPY)
			continue;

		float flDistSqr = vLocalCenter.DistToSqr(pSpy->GetCenter());
		if (flDistSqr > flRadiusSqr)
			continue;

		const float flCloak = std::clamp(pSpy->m_flInvisibility(), 0.f, 1.f);
		float flPrev = 1.f;
		if (auto it = m_mSpyInvisibility.find(pSpy->entindex()); it != m_mSpyInvisibility.end())
			flPrev = it->second;
		m_mSpyInvisibility[pSpy->entindex()] = flCloak;

		const bool bWasFullCloak = flPrev >= 0.9f;
		const bool bNowVisible = flCloak <= 0.6f;
		const bool bDropping = flCloak < flPrev;
		const bool bUncloakCond = pSpy->InCond(TF_COND_STEALTHED_BLINK) || pSpy->InCond(TF_COND_STEALTHED_USER_BUFF_FADING);
		if (!(bUncloakCond || (bWasFullCloak && bNowVisible && bDropping)))
			continue;

		Vec3 vAim = Math::CalcAngle(pLocal->GetShootPos(), pSpy->GetCenter());
		Math::ClampAngles(vAim);

		const float flScore = (1.f - flCloak) + (flRadiusSqr - flDistSqr) / std::max(flRadiusSqr, 1.f);
		if (flScore > flBestScore)
		{
			flBestScore = flScore;
			pBestTarget = pSpy;
			vBestAngles = vAim;
		}
	}

	if (!pBestTarget)
		return false;

	Vec3 vOldAngles = pCmd->viewangles;
	pCmd->viewangles = vBestAngles;
	SDK::FixMovement(pCmd, vOldAngles, vBestAngles);
	return true;
}

bool CBotUtils::HandleSpycheckPulse(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd, bool bAnglesLocked)
{
	if (!Vars::Misc::Movement::LegitBot::SpycheckPulse.Value || !pLocal || !pWeapon || !pCmd)
		return false;

	if (pLocal->m_iClass() != TF_CLASS_PYRO || pWeapon->GetWeaponID() != TF_WEAPON_FLAMETHROWER)
		return false;

	if (!pWeapon->HasAmmo() || pLocal->IsTaunting() || pLocal->InCond(TF_COND_HALLOWEEN_KART) || G::Attacking == 1)
		return false;

	const float flInterval = std::max(Vars::Misc::Movement::LegitBot::SpycheckInterval.Value, 0.2f);
	if (!m_tSpycheckTimer.Run(flInterval))
		return false;

	const int iChance = std::clamp(Vars::Misc::Movement::LegitBot::SpycheckChance.Value, 0, 100);
	if (iChance <= 0)
		return false;
	if (SDK::RandomInt(1, 100) > iChance)
		return false;

	const float flRadius = std::max(Vars::Misc::Movement::LegitBot::SpycheckRadius.Value, 32.f);
	const float flRadiusSqr = flRadius * flRadius;
	const Vec3 vLocal = pLocal->GetCenter();

	CTFPlayer* pTarget = nullptr;
	float flBestDist = flRadiusSqr;

	for (auto pEntity : H::Entities.GetGroup(EGroupType::PLAYERS_ENEMIES))
	{
		auto pEnemy = pEntity->As<CTFPlayer>();
		if (!pEnemy || !pEnemy->IsAlive() || pEnemy->IsAGhost())
			continue;

		float flDistSqr = vLocal.DistToSqr(pEnemy->GetCenter());
		if (flDistSqr > flRadiusSqr)
			continue;

		const bool bSuspicious = pEnemy->m_iClass() == TF_CLASS_SPY
			|| pEnemy->InCond(TF_COND_DISGUISED)
			|| pEnemy->InCond(TF_COND_STEALTHED)
			|| pEnemy->InCond(TF_COND_STEALTHED_USER_BUFF)
			|| pEnemy->InCond(TF_COND_STEALTHED_USER_BUFF_FADING)
			|| pEnemy->m_flInvisibility() > 0.f;

		if (!bSuspicious)
			continue;

		if (flDistSqr < flBestDist)
		{
			flBestDist = flDistSqr;
			pTarget = pEnemy;
		}
	}

	Vec3 vOldAngles = pCmd->viewangles;
	if (!bAnglesLocked)
	{
		Vec3 vAim = vOldAngles;
		if (pTarget)
			vAim = Math::CalcAngle(pLocal->GetShootPos(), pTarget->GetCenter());
		else
			vAim = vAim + Vec3(SDK::RandomFloat(-2.5f, 2.5f), SDK::RandomFloat(-18.f, 18.f), 0.f);

		Math::ClampAngles(vAim);
		pCmd->viewangles = vAim;
		SDK::FixMovement(pCmd, vOldAngles, vAim);
	}

	pCmd->buttons |= IN_ATTACK;
	return true;
}

void CBotUtils::UpdateSpyInvisibilityCache(CTFPlayer* pLocal)
{
	if (m_mSpyInvisibility.empty())
		return;

	for (auto it = m_mSpyInvisibility.begin(); it != m_mSpyInvisibility.end(); )
	{
		auto pEntity = I::ClientEntityList->GetClientEntity(it->first);
		auto pPlayer = pEntity ? pEntity->As<CTFPlayer>() : nullptr;
		if (!pPlayer || !pPlayer->IsAlive() || pPlayer->m_iTeamNum() == pLocal->m_iTeamNum())
		{
			it = m_mSpyInvisibility.erase(it);
			continue;
		}
		++it;
	}
}

Vec3 CBotUtils::AdjustForObstacles(CTFPlayer* pLocal, const Vec3& candidateAngles, const Vec3& referenceAngles) const
{
	if (!pLocal)
		return candidateAngles;

	const Vec3 vEye = pLocal->GetEyePosition();
	Vec3 vDir;
	Math::AngleVectors(candidateAngles, &vDir);
	Vec3 vEnd = vEye + vDir * 72.f;

	CGameTrace trace = {};
	CTraceFilterWorldAndPropsOnly filter = {};
	SDK::Trace(vEye, vEnd, MASK_SOLID, &filter, &trace);

	if (trace.fraction >= 0.35f)
		return candidateAngles;

	const float flBlend = std::clamp(1.f - trace.fraction * 2.4f, 0.f, 1.f);
	Vec3 vAdjusted = candidateAngles.LerpAngle(referenceAngles, flBlend);
	Math::ClampAngles(vAdjusted);
	return vAdjusted;
}

bool CBotUtils::BuildScanTarget(CTFPlayer* pLocal, const Vec3& baseAngles, Vec3& outAngles) const
{
	if (!pLocal)
		return false;

	const Vec3 vEye = pLocal->GetEyePosition();

	const float flNow = I::GlobalVars->curtime;

	for (size_t i = 0; i < std::min<size_t>(m_vCloseEnemies.size(), 3); i++)
	{
		const auto& enemy = m_vCloseEnemies[i];
		Vec3 vTarget = {};
		const bool bRecentlySeen = enemy.m_flLastSeen > 0.f && (flNow - enemy.m_flLastSeen) <= 0.8f;

		if (enemy.m_pPlayer && enemy.m_pPlayer->IsAlive())
		{
			vTarget = enemy.m_pPlayer->GetCenter();
			if (vTarget.IsZero())
				vTarget = enemy.m_pPlayer->GetAbsOrigin() + Vec3(0.f, 0.f, 48.f);
			else
				vTarget.z += 8.f;
		}
		else if (bRecentlySeen && !enemy.m_vLastKnownPos.IsZero())
		{
			vTarget = enemy.m_vLastKnownPos;
		}
		else
		{
			continue;
		}

		if (vTarget.IsZero())
			continue;

		if (!IsLineVisible(vEye, vTarget))
			continue;

		outAngles = Math::CalcAngle(vEye, vTarget);
		return true;
	}

	Vec3 forward, right, up;
	Math::AngleVectors(baseAngles, &forward, &right, &up);

	struct Sample_t
	{
		float yaw;
		float pitch;
		float distance;
	};

	Sample_t samples[] = {
		{ SDK::RandomFloat(24.f, 42.f), SDK::RandomFloat(-6.f, 5.f), SDK::RandomFloat(320.f, 560.f) },
		{ -SDK::RandomFloat(24.f, 48.f), SDK::RandomFloat(-7.f, 6.f), SDK::RandomFloat(320.f, 560.f) },
		{ SDK::RandomFloat(-10.f, 10.f), SDK::RandomFloat(8.f, 16.f), SDK::RandomFloat(260.f, 420.f) },
		{ SDK::RandomFloat(-18.f, 18.f), -SDK::RandomFloat(6.f, 14.f), SDK::RandomFloat(300.f, 520.f) }
	};

	for (const auto& sample : samples)
	{
		Vec3 candidateAngles = baseAngles + Vec3(sample.pitch, sample.yaw, 0.f);
		Math::ClampAngles(candidateAngles);

		Vec3 dir;
		Math::AngleVectors(candidateAngles, &dir);
		Vec3 vEnd = vEye + dir * sample.distance;

		bool bNavClear = !F::NavEngine.IsNavMeshLoaded() || F::NavParser.IsVectorVisibleNavigation(vEye, vEnd);
		if (!bNavClear)
			continue;

		if (!IsLineVisible(vEye, vEnd))
			continue;

		outAngles = candidateAngles;
		return true;
	}

	return false;
}

Vec3 CBotUtils::UpdateLookState(CTFPlayer* pLocal, const Vec3& desiredAngles, const Vec3& currentCmdAngles, const CUserCmd* pCmd, float frameTime)
{
	auto& state = m_tLookState;
	Vec3 vDesired = desiredAngles;
	Vec3 vCmdAngles = currentCmdAngles;

	if (!pLocal)
		return vCmdAngles.IsZero() ? vDesired : vCmdAngles;

	if (!state.initialized)
	{
		ResetLookState();
		Vec3 vStart = vCmdAngles.IsZero() ? vDesired : vCmdAngles;
		state.currentAngles = vStart;
		state.lastRequestedAngles = vDesired;
		state.scanAngles = vStart;
		state.externalAngles = vStart;
		state.initialized = true;
		return vStart;
	}

	state.externalRelease = std::max(state.externalRelease - frameTime, 0.f);

	if (m_bPendingAssist)
	{
		state.assistAngles = m_vPendingAssistAngles;
		state.assistExpiry = m_flPendingAssistExpiry;
		state.assistSmooth = m_bPendingAssistSmooth;
		state.assistActive = true;
		m_bPendingAssist = false;
	}

	if (state.assistActive)
	{
		if (I::GlobalVars->curtime > state.assistExpiry)
		{
			state.assistActive = false;
		}
		else
		{
			if (state.assistSmooth)
			{
				float flBlendRate = std::clamp(frameTime * 6.f, 0.f, 1.f);
				vDesired = vDesired.LerpAngle(state.assistAngles, flBlendRate);
			}
			else
			{
				vDesired = state.assistAngles;
			}

			state.reactionPending = false;
			state.scanBlend = std::lerp(state.scanBlend, 0.f, frameTime * 6.f);
			state.curiosityBlend = std::lerp(state.curiosityBlend, 0.f, frameTime * 6.f);
			state.curiosityActive = false;
			state.pathOffset = state.pathOffset.Lerp({}, frameTime * 5.f);
			state.pathOffsetGoal = state.pathOffset;
		}
	}

	const float flCmdDelta = Math::CalcFov(state.currentAngles, vCmdAngles);
	const bool bManualOverride = pCmd && (std::abs(pCmd->mousedx) > 1 || std::abs(pCmd->mousedy) > 1) && flCmdDelta > 0.2f;

	if (bManualOverride)
	{
		state.externalActive = true;
		state.externalAngles = vCmdAngles;
		state.currentAngles = vCmdAngles;
		state.lastRequestedAngles = vDesired;
		state.scanAngles = vCmdAngles;

		state.reactionPending = false;
		state.scanBlend = std::lerp(state.scanBlend, 0.f, frameTime * 5.f);
		state.curiosityBlend = std::lerp(state.curiosityBlend, 0.f, frameTime * 4.f);

		state.pathOffset = state.pathOffset.Lerp({}, frameTime * 5.f);
		state.pathOffsetGoal = state.pathOffset;

		state.externalRelease = std::min(state.externalRelease + frameTime * 3.f, 0.5f);

		return vCmdAngles;
	}
	else if (state.externalActive)
	{
		state.externalActive = false;
		state.externalAngles = vCmdAngles;
		state.currentAngles = vCmdAngles;
		state.externalRelease = std::max(state.externalRelease, 0.18f);
	}

	float flDeltaToLast = Math::CalcFov(state.lastRequestedAngles, vDesired);
	if (flDeltaToLast > 1.5f)
	{
		state.reactionPending = true;
		float flReactionScale = std::clamp(flDeltaToLast / 40.f, 0.5f, 1.35f);
		float flBaseDelay = SDK::RandomFloat(0.3f, 0.9f);
		state.reactionDelay = std::clamp(flBaseDelay * flReactionScale, 0.3f, 0.9f);
		state.reactionTimer.Update();
	}
	state.lastRequestedAngles = vDesired;

	const float flBehindDiff = AngleDistance(state.currentAngles.y, vDesired.y);
	const bool bTargetBehind = flBehindDiff > 110.f;
	const bool bSevereBehind = flBehindDiff > 160.f;
	float flBehindSnapStrength = 0.f;
	if (bTargetBehind)
	{
		flBehindSnapStrength = std::clamp((flBehindDiff - 110.f) / 85.f, 0.f, 0.5f);
		if (bSevereBehind)
			state.reactionPending = false;
		state.scanning = false;
		state.curiosityActive = false;
		state.curiosityAggressive = false;
		state.scanBlend = std::lerp(state.scanBlend, 0.f, frameTime * 5.6f);
		state.curiosityBlend = std::lerp(state.curiosityBlend, 0.f, frameTime * 5.4f);
		state.curiosityCooldownTimer.Update();
		state.curiosityCooldown = SDK::RandomFloat(2.4f, 3.8f);
	}

	if (state.reactionPending)
	{
		if (!state.reactionTimer.Check(state.reactionDelay))
		{
			vDesired = state.currentAngles;
		}
		else
		{
			state.reactionPending = false;
		}
	}

	if (state.pathOffsetTimer.Check(state.pathOffsetInterval))
	{
		state.pathOffsetGoal.x = SDK::RandomFloat(-0.35f, 0.35f);
		state.pathOffsetGoal.y = SDK::RandomFloat(-1.3f, 1.3f);
		state.pathOffsetInterval = SDK::RandomFloat(1.0f, 1.9f);
		state.pathOffsetTimer.Update();
	}
	state.pathOffset = state.pathOffset.Lerp(state.pathOffsetGoal, frameTime * 2.4f);
	const float flReturnBlend = std::clamp(1.f - state.externalRelease * 2.3f, 0.2f, 1.f);
	Vec3 vWithOffset = vDesired + (state.pathOffset * flReturnBlend);
	Math::ClampAngles(vWithOffset);
	if (bSevereBehind)
	{
		state.pathOffset = state.pathOffset.Lerp({}, frameTime * 5.2f);
		state.pathOffsetGoal = state.pathOffset;
		vWithOffset = vDesired;
	}
	else if (bTargetBehind && flBehindSnapStrength > 0.f)
	{
		vWithOffset = vWithOffset.LerpAngle(vDesired, flBehindSnapStrength);
	}

	if (state.scanning)
	{
		if (state.scanHoldTimer.Check(state.scanDuration))
		{
			state.scanning = false;
			state.scanCooldown = SDK::RandomFloat(1.4f, 3.0f);
			state.scanCooldownTimer.Update();
		}
		state.curiosityActive = false;
	}
	else if (state.scanCooldownTimer.Check(state.scanCooldown))
	{
		Vec3 vScanTarget = {};
		if (BuildScanTarget(pLocal, vWithOffset, vScanTarget))
		{
			state.scanning = true;
			state.scanDuration = SDK::RandomFloat(0.5f, 1.05f);
			state.scanAngles = vScanTarget;
			state.scanHoldTimer.Update();
		}
		else
		{
			state.scanCooldown = SDK::RandomFloat(0.8f, 1.4f);
			state.scanCooldownTimer.Update();
		}
	}

	float flTargetBlend = state.scanning ? 1.f : 0.f;
	state.scanBlend = std::lerp(state.scanBlend, flTargetBlend, frameTime * 2.6f);
	if (state.scanBlend > 0.001f)
	{
		float flAdjustedBlend = std::clamp(state.scanBlend * flReturnBlend, 0.f, 1.f);
		vWithOffset = vWithOffset.LerpAngle(state.scanAngles, flAdjustedBlend);
	}
	else
	{
		state.scanAngles = vWithOffset;
	}

	float flFocusDelta = Math::CalcFov(state.currentAngles, vWithOffset);
	if (state.curiosityActive)
	{
		if (state.curiosityTimer.Check(state.curiosityDuration))
		{
			state.curiosityActive = false;
			state.curiosityAggressive = false;
			state.curiosityCooldown = SDK::RandomFloat(6.f, 12.f);
			state.curiosityCooldownTimer.Update();
		}
	}
	else if (!state.scanning && !bTargetBehind && flFocusDelta < 18.f && state.curiosityCooldownTimer.Check(state.curiosityCooldown))
	{
		state.curiosityActive = true;
		state.curiosityAggressive = true;
		state.curiosityDuration = SDK::RandomFloat(0.7f, 1.3f);
		Vec3 vCuriosity = {};
		bool bThreatFocus = false;
		const Vec3 vEye = pLocal->GetEyePosition();
		for (const auto& enemy : m_vCloseEnemies)
		{
			if (!enemy.m_pPlayer || !enemy.m_pPlayer->IsAlive())
				continue;
			if (enemy.m_pPlayer->m_iTeamNum() == pLocal->m_iTeamNum())
				continue;
			Vec3 vTarget = enemy.m_pPlayer->GetCenter();
			if (vTarget.IsZero())
				vTarget = enemy.m_pPlayer->GetAbsOrigin() + Vec3(0.f, 0.f, 48.f);
			Vec3 vEnemyAngles = Math::CalcAngle(vEye, vTarget);
			Math::ClampAngles(vEnemyAngles);
			if (AngleDistance(vEnemyAngles.y, vWithOffset.y) < 12.f && AngleDistance(vEnemyAngles.x, vWithOffset.x) < 8.f)
				continue;
			vCuriosity = vEnemyAngles;
			bThreatFocus = true;
			break;
		}
		if (!bThreatFocus)
		{
			state.curiosityTurnSign = state.curiosityTurnSign >= 0.f ? -1.f : 1.f;
			float flYawSweep = SDK::RandomFloat(28.f, 46.f);
			float flPitchSweep = SDK::RandomFloat(-3.f, 5.f);
			vCuriosity = vWithOffset;
			vCuriosity.y = Math::NormalizeAngle(vCuriosity.y + state.curiosityTurnSign * flYawSweep);
			vCuriosity.x = Math::NormalizeAngle(vCuriosity.x + flPitchSweep);
		}
		const auto clampAngleDelta = [](float target, float base, float maxDelta)
		{
			float delta = Math::NormalizeAngle(target - base);
			delta = std::clamp(delta, -maxDelta, maxDelta);
			return Math::NormalizeAngle(base + delta);
		};
		vCuriosity.y = clampAngleDelta(vCuriosity.y, vWithOffset.y, 42.f);
		vCuriosity.x = clampAngleDelta(vCuriosity.x, vWithOffset.x, 16.f);
		Math::ClampAngles(vCuriosity);
		state.curiosityAngles = AdjustForObstacles(pLocal, vCuriosity, vWithOffset);
		state.curiosityTimer.Update();
		state.curiosityBlend = 0.f;
	}

	float flCuriosityTarget = (state.curiosityActive && !state.scanning) ? 1.f : 0.f;
	state.curiosityBlend = std::lerp(state.curiosityBlend, flCuriosityTarget, frameTime * (state.curiosityActive ? 2.2f : 2.8f));
	if (state.curiosityBlend > 0.001f)
	{
		float flAdjustedCuriosity = std::clamp(state.curiosityBlend * flReturnBlend, 0.f, 1.f);
		vWithOffset = vWithOffset.LerpAngle(state.curiosityAngles, flAdjustedCuriosity);
	}
	else
	{
		state.curiosityAngles = vWithOffset;
	}

	const float flYawDiffToDesired = AngleDistance(vDesired.y, vWithOffset.y);
	const float flPitchDiffToDesired = AngleDistance(vDesired.x, vWithOffset.x);
	const float flDesiredPitch = vDesired.x;

	if (flYawDiffToDesired > 45.f)
	{
		state.scanning = false;
		state.curiosityActive = false;
		state.scanBlend = 0.f;
		state.curiosityBlend = 0.f;
	}

	const float flMaxYawDeviation = 35.f;
	if (flYawDiffToDesired > flMaxYawDeviation)
	{
		const float flStep = flYawDiffToDesired - flMaxYawDeviation;
		vWithOffset.y = ApproachAngle(vDesired.y, vWithOffset.y, flStep);
	}

	const float flMaxPitchDeviation = 12.f;
	if (flPitchDiffToDesired > flMaxPitchDeviation)
	{
		const float flStep = flPitchDiffToDesired - flMaxPitchDeviation;
		vWithOffset.x = ApproachAngle(vDesired.x, vWithOffset.x, flStep);
	}

	if (flYawDiffToDesired > 25.f)
	{
		state.scanBlend = std::lerp(state.scanBlend, 0.f, frameTime * 5.2f);
		state.curiosityBlend = std::lerp(state.curiosityBlend, 0.f, frameTime * 4.6f);
	}

	const float flAlignmentBias = std::clamp(flYawDiffToDesired / 60.f, 0.f, 0.8f);
	if (flAlignmentBias > 0.f)
	{
		vWithOffset = vWithOffset.LerpAngle(vDesired, flAlignmentBias);
	}

	if (flDesiredPitch < -5.f || vWithOffset.x < -5.f)
	{
		float flDesiredUp = std::min(flDesiredPitch, -5.f);
		float flSeverity = std::clamp(((-flDesiredUp) - 5.f) / 35.f, 0.f, 1.f);
		float flMaxUpPitch = std::lerp(-5.f, std::max(flDesiredUp, -22.f), flSeverity * 0.75f);
		float flBlend = std::clamp(frameTime * 4.2f, 0.f, 1.f);
		if (vWithOffset.x < flMaxUpPitch)
			vWithOffset.x = std::lerp(vWithOffset.x, flMaxUpPitch, flBlend);
	}

	Math::ClampAngles(vWithOffset);

	if (state.speedVarianceTimer.Check(state.speedVarianceInterval))
	{
		state.speedVariance = SDK::RandomFloat(0.82f, 1.34f);
		state.speedVarianceInterval = SDK::RandomFloat(0.45f, 1.3f);
		state.speedVarianceTimer.Update();
	}

	float flExternalDamp = std::clamp(1.f - state.externalRelease * 3.2f, 0.f, 1.f);
	Vec3 vFinalTarget = vWithOffset;
	Math::ClampAngles(vFinalTarget);
	vFinalTarget = AdjustForObstacles(pLocal, vFinalTarget, state.currentAngles);

	const float flBaseSetting = std::max(6.f, static_cast<float>(Vars::Misc::Movement::BotUtils::LookAtPathSpeed.Value));
	const float flYawDelta = AngleDistance(vFinalTarget.y, state.currentAngles.y);
	const float flPitchDelta = AngleDistance(vFinalTarget.x, state.currentAngles.x);
	float flUrgency = Math::SimpleSpline(std::clamp((flYawDelta + flPitchDelta * 0.45f) / 120.f, 0.f, 1.f));
	float flBurstScale = std::lerp(0.87f, 1.45f, flUrgency);
	if (state.curiosityActive)
		flBurstScale = std::max(flBurstScale, state.curiosityAggressive ? 1.22f : 1.12f);
	if (bTargetBehind)
		flBurstScale = std::max(flBurstScale, bSevereBehind ? 1.3f : 1.18f);
	const float flBaseSpeed = flBaseSetting * state.speedVariance * flBurstScale;

	const float flYawIntensity = Math::SimpleSpline(std::clamp(flYawDelta / 95.f, 0.f, 1.f));
	const float flPitchIntensity = Math::SimpleSpline(std::clamp(flPitchDelta / 80.f, 0.f, 1.f));

	const float flYawMin = flBaseSpeed * 0.6f;
	const float flPitchMin = flBaseSpeed * 0.45f;
	float flYawTargetSpeed = std::lerp(flYawMin, state.maxYawSpeed, flYawIntensity);
	float flPitchTargetSpeed = std::lerp(flPitchMin, state.maxPitchSpeed, flPitchIntensity);
	flYawTargetSpeed = std::max(flYawTargetSpeed, std::clamp(flYawDelta * 8.5f, flBaseSpeed * 0.9f, state.maxYawSpeed * 1.32f));
	flPitchTargetSpeed = std::max(flPitchTargetSpeed, std::clamp(flPitchDelta * 7.2f, flBaseSpeed * 0.8f, state.maxPitchSpeed * 1.26f));
	if (state.curiosityActive)
	{
		flYawTargetSpeed = std::max(flYawTargetSpeed, state.maxYawSpeed * (state.curiosityAggressive ? 1.08f : 1.02f));
		flPitchTargetSpeed = std::max(flPitchTargetSpeed, state.maxPitchSpeed * 1.04f);
	}
	if (bTargetBehind)
	{
		float flYawBoost = bSevereBehind ? 1.18f : 1.1f;
		float flPitchBoost = bSevereBehind ? 1.15f : 1.08f;
		flYawTargetSpeed = std::max(flYawTargetSpeed, state.maxYawSpeed * flYawBoost);
		flPitchTargetSpeed = std::max(flPitchTargetSpeed, state.maxPitchSpeed * flPitchBoost);
	}

	const float flVelocityBlend = std::clamp(frameTime * (3.2f + flUrgency * 6.5f), 0.f, 1.f);
	state.angularVelocity.x = std::lerp(state.angularVelocity.x, flYawTargetSpeed, flVelocityBlend);
	state.angularVelocity.y = std::lerp(state.angularVelocity.y, flPitchTargetSpeed, flVelocityBlend * 0.9f);
	state.angularVelocity.x = std::clamp(state.angularVelocity.x, 0.f, state.maxYawSpeed * 1.35f);
	state.angularVelocity.y = std::clamp(state.angularVelocity.y, 0.f, state.maxPitchSpeed * 1.3f);

	float flAdaptiveYawCatch = std::clamp(frameTime * (0.85f + flUrgency * 2.6f), 0.f, 0.85f);
	float flAdaptivePitchCatch = std::clamp(frameTime * (0.7f + flUrgency * 1.8f), 0.f, 0.75f);
	if (state.curiosityActive)
	{
		float flAggressiveYawCatch = std::clamp(frameTime * (0.95f + flUrgency * 1.9f), 0.f, 0.82f);
		float flAggressivePitchCatch = std::clamp(frameTime * (0.82f + flUrgency * 1.5f), 0.f, 0.78f);
		flAdaptiveYawCatch = std::max(flAdaptiveYawCatch, flAggressiveYawCatch);
		flAdaptivePitchCatch = std::max(flAdaptivePitchCatch, flAggressivePitchCatch);
	}
	if (bTargetBehind)
	{
		float flBehindYawCatch = std::clamp(frameTime * (1.15f + flUrgency * (bSevereBehind ? 3.5f : 3.f)), 0.f, 0.9f);
		float flBehindPitchCatch = std::clamp(frameTime * (0.95f + flUrgency * (bSevereBehind ? 2.4f : 2.f)), 0.f, 0.84f);
		flAdaptiveYawCatch = std::max(flAdaptiveYawCatch, flBehindYawCatch);
		flAdaptivePitchCatch = std::max(flAdaptivePitchCatch, flBehindPitchCatch);
	}
	float flYawStep = std::max(state.angularVelocity.x * frameTime, flYawDelta * flAdaptiveYawCatch);
	float flPitchStep = std::max(state.angularVelocity.y * frameTime, flPitchDelta * flAdaptivePitchCatch);
	if (state.curiosityActive)
	{
		flYawStep = std::max(flYawStep, flYawDelta * std::clamp(frameTime * 3.1f, 0.f, 0.72f));
		flPitchStep = std::max(flPitchStep, flPitchDelta * std::clamp(frameTime * 2.4f, 0.f, 0.78f));
	}
	if (bTargetBehind)
	{
		float flYawAccel = std::clamp(frameTime * (bSevereBehind ? 4.1f : 3.5f), 0.f, 0.82f);
		float flPitchAccel = std::clamp(frameTime * (bSevereBehind ? 2.9f : 2.5f), 0.f, 0.8f);
		flYawStep = std::max(flYawStep, flYawDelta * flYawAccel);
		flPitchStep = std::max(flPitchStep, flPitchDelta * flPitchAccel);
	}

	if (flYawDelta > 75.f)
		flYawStep = std::max(flYawStep, flYawDelta * std::clamp(frameTime * 3.8f, 0.f, 0.95f));
	if (flPitchDelta > 55.f)
		flPitchStep = std::max(flPitchStep, flPitchDelta * std::clamp(frameTime * 2.4f, 0.f, 0.8f));

	Vec3 vPrevAngles = state.currentAngles;
	Vec3 vUpdated = vPrevAngles;
	vUpdated.y = ApproachAngle(vFinalTarget.y, vUpdated.y, flYawStep);
	vUpdated.x = ApproachAngle(vFinalTarget.x, vUpdated.x, flPitchStep);
	vUpdated.z = 0.f;
	Math::ClampAngles(vUpdated);

	const float flAppliedYaw = AngleDistance(vUpdated.y, vPrevAngles.y) / std::max(frameTime, 0.0001f);
	const float flAppliedPitch = AngleDistance(vUpdated.x, vPrevAngles.x) / std::max(frameTime, 0.0001f);
	const float flFeedbackBlend = std::clamp(frameTime * 4.6f, 0.f, 1.f);
	state.angularVelocity.x = std::lerp(state.angularVelocity.x, flAppliedYaw, flFeedbackBlend);
	state.angularVelocity.y = std::lerp(state.angularVelocity.y, flAppliedPitch, flFeedbackBlend * 0.85f);
	state.angularVelocity.x = std::clamp(state.angularVelocity.x, 0.f, state.maxYawSpeed * 1.35f);
	state.angularVelocity.y = std::clamp(state.angularVelocity.y, 0.f, state.maxPitchSpeed * 1.3f);

	state.currentAngles = vUpdated;
	return vUpdated;
}

bool CBotUtils::HasMedigunTargets(CTFPlayer* pLocal, CTFWeaponBase* pWeapon)
{
	if (!Vars::Aimbot::Healing::AutoHeal.Value)
		return false;

	Vec3 vShootPos = F::Ticks.GetShootPos();
	float flRange = pWeapon->GetRange();
	for (auto pEntity : H::Entities.GetGroup(EGroupType::PLAYERS_TEAMMATES))
	{
		if (pEntity->entindex() == pLocal->entindex() || vShootPos.DistTo(pEntity->GetCenter()) > flRange)
			continue;

		if (pEntity->As<CTFPlayer>()->InCond(TF_COND_STEALTHED) ||
			(Vars::Aimbot::Healing::HealPriority.Value == Vars::Aimbot::Healing::HealPriorityEnum::FriendsOnly &&
			!H::Entities.IsFriend(pEntity->entindex()) && !H::Entities.InParty(pEntity->entindex())))
			continue;

		return true;
	}
	return false;
}

ShouldTargetState_t CBotUtils::ShouldTarget(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, int iPlayerIdx)
{
	auto pEntity = I::ClientEntityList->GetClientEntity(iPlayerIdx)->As<CBaseEntity>();
	if (!pEntity || !pEntity->IsPlayer())
		return ShouldTargetState_t::INVALID;

	auto pPlayer = pEntity->As<CTFPlayer>();
	if (!pPlayer->IsAlive() || pPlayer == pLocal)
		return ShouldTargetState_t::INVALID;

	if (Vars::Misc::Movement::NavBot::RespectRelationships.Value)
	{
		if (F::PlayerUtils.IsIgnored(iPlayerIdx) || H::Entities.IsFriend(iPlayerIdx) || H::Entities.InParty(iPlayerIdx))
			return ShouldTargetState_t::DONT_TARGET;

#ifdef TEXTMODE
		if (auto pResource = H::Entities.GetResource(); pResource && F::NamedPipe.IsLocalBot(pResource->m_iAccountID(iPlayerIdx)))
			return ShouldTargetState_t::DONT_TARGET;
#endif
	}

	if (Vars::Aimbot::General::Ignore.Value & Vars::Aimbot::General::IgnoreEnum::Friends && H::Entities.IsFriend(iPlayerIdx)
		|| Vars::Aimbot::General::Ignore.Value & Vars::Aimbot::General::IgnoreEnum::Party && H::Entities.InParty(iPlayerIdx)
		|| Vars::Aimbot::General::Ignore.Value & Vars::Aimbot::General::IgnoreEnum::Invulnerable && pPlayer->IsInvulnerable() && G::SavedDefIndexes[SLOT_MELEE] != Heavy_t_TheHolidayPunch
		|| Vars::Aimbot::General::Ignore.Value & Vars::Aimbot::General::IgnoreEnum::Invisible && pPlayer->m_flInvisibility() && pPlayer->m_flInvisibility() >= Vars::Aimbot::General::IgnoreInvisible.Value / 100.f
		|| Vars::Aimbot::General::Ignore.Value & Vars::Aimbot::General::IgnoreEnum::DeadRinger && pPlayer->m_bFeignDeathReady()
		|| Vars::Aimbot::General::Ignore.Value & Vars::Aimbot::General::IgnoreEnum::Taunting && pPlayer->IsTaunting()
		|| Vars::Aimbot::General::Ignore.Value & Vars::Aimbot::General::IgnoreEnum::Disguised && pPlayer->InCond(TF_COND_DISGUISED))
		return ShouldTargetState_t::DONT_TARGET;

	if (pPlayer->m_iTeamNum() == pLocal->m_iTeamNum())
		return ShouldTargetState_t::DONT_TARGET;

	if (Vars::Aimbot::General::Ignore.Value & Vars::Aimbot::General::IgnoreEnum::Vaccinator)
	{
		switch (SDK::GetWeaponType(pWeapon))
		{
		case EWeaponType::HITSCAN:
			if (pPlayer->InCond(TF_COND_MEDIGUN_UBER_BULLET_RESIST) && SDK::AttribHookValue(0, "mod_pierce_resists_absorbs", pWeapon) != 0)
				return ShouldTargetState_t::DONT_TARGET;
			break;
		case EWeaponType::PROJECTILE:
			if (pPlayer->InCond(TF_COND_MEDIGUN_UBER_FIRE_RESIST) && (G::SavedWepIds[SLOT_PRIMARY] == TF_WEAPON_FLAMETHROWER && G::SavedWepIds[SLOT_SECONDARY] == TF_WEAPON_FLAREGUN))
				return ShouldTargetState_t::DONT_TARGET;
			else if (pPlayer->InCond(TF_COND_MEDIGUN_UBER_BULLET_RESIST) && G::SavedWepIds[SLOT_PRIMARY] == TF_WEAPON_COMPOUND_BOW)
				return ShouldTargetState_t::DONT_TARGET;
			else if (pPlayer->InCond(TF_COND_MEDIGUN_UBER_BLAST_RESIST))
				return ShouldTargetState_t::DONT_TARGET;
		}
	}

	return ShouldTargetState_t::TARGET;
}

ShouldTargetState_t CBotUtils::ShouldTargetBuilding(CTFPlayer* pLocal, int iEntIdx)
{
	auto pEntity = I::ClientEntityList->GetClientEntity(iEntIdx)->As<CBaseEntity>();
	if (!pEntity || !pEntity->IsBuilding())
		return ShouldTargetState_t::INVALID;

	auto pBuilding = pEntity->As<CBaseObject>();
	if (!(Vars::Aimbot::General::Target.Value & Vars::Aimbot::General::TargetEnum::Sentry) && pBuilding->IsSentrygun()
		|| !(Vars::Aimbot::General::Target.Value & Vars::Aimbot::General::TargetEnum::Dispenser) && pBuilding->IsDispenser()
		|| !(Vars::Aimbot::General::Target.Value & Vars::Aimbot::General::TargetEnum::Teleporter) && pBuilding->IsTeleporter())
		return ShouldTargetState_t::TARGET;

	if (pLocal->m_iTeamNum() == pBuilding->m_iTeamNum())
		return ShouldTargetState_t::TARGET;

	auto pOwner = pBuilding->m_hBuilder().Get();
	if (pOwner)
	{
		if (F::PlayerUtils.IsIgnored(pOwner->entindex()))
			return ShouldTargetState_t::DONT_TARGET;

		if (Vars::Aimbot::General::Ignore.Value & Vars::Aimbot::General::IgnoreEnum::Friends && H::Entities.IsFriend(pOwner->entindex())
			|| Vars::Aimbot::General::Ignore.Value & Vars::Aimbot::General::IgnoreEnum::Party && H::Entities.InParty(pOwner->entindex()))
			return ShouldTargetState_t::DONT_TARGET;
	}

	return ShouldTargetState_t::TARGET;
}

ClosestEnemy_t CBotUtils::UpdateCloseEnemies(CTFPlayer* pLocal, CTFWeaponBase* pWeapon)
{
	const float flNow = I::GlobalVars->curtime;
	constexpr float flRetention = 0.8f;

	for (auto& enemy : m_vCloseEnemies)
		enemy.m_bUpdated = false;

	for (auto pEntity : H::Entities.GetGroup(EGroupType::PLAYERS_ENEMIES))
	{
		auto pPlayer = pEntity->As<CTFPlayer>();
		int iEntIndex = pPlayer->entindex();
		const auto targetState = ShouldTarget(pLocal, pWeapon, iEntIndex);
		if (targetState != ShouldTargetState_t::TARGET)
			continue;

		auto it = std::find_if(m_vCloseEnemies.begin(), m_vCloseEnemies.end(), [iEntIndex](const ClosestEnemy_t& enemy)
			{
				return enemy.m_iEntIdx == iEntIndex;
			});

		auto vOrigin = F::NavParser.GetDormantOrigin(iEntIndex);
		if (!vOrigin)
		{
			if (it != m_vCloseEnemies.end())
			{
				it->m_pPlayer = pPlayer;
			}
			continue;
		}

		Vec3 vKnownPos = *vOrigin;
		vKnownPos.z += 48.f;
		float flDist = pLocal->GetAbsOrigin().DistTo(vKnownPos);

		if (it != m_vCloseEnemies.end())
		{
			it->m_pPlayer = pPlayer;
			it->m_flDist = flDist;
			it->m_vLastKnownPos = vKnownPos;
			it->m_flLastSeen = flNow;
			it->m_bUpdated = true;
		}
		else
		{
			ClosestEnemy_t tEntry{};
			tEntry.m_iEntIdx = iEntIndex;
			tEntry.m_pPlayer = pPlayer;
			tEntry.m_flDist = flDist;
			tEntry.m_vLastKnownPos = vKnownPos;
			tEntry.m_flLastSeen = flNow;
			tEntry.m_bUpdated = true;
			m_vCloseEnemies.push_back(tEntry);
		}
	}

	const Vec3 vLocalOrigin = pLocal->GetAbsOrigin();
	for (auto& enemy : m_vCloseEnemies)
	{
		if (!enemy.m_bUpdated && enemy.m_flLastSeen > 0.f && !enemy.m_vLastKnownPos.IsZero())
			enemy.m_flDist = vLocalOrigin.DistTo(enemy.m_vLastKnownPos);
		enemy.m_bUpdated = false;
	}

	m_vCloseEnemies.erase(std::remove_if(m_vCloseEnemies.begin(), m_vCloseEnemies.end(), [flNow](const ClosestEnemy_t& enemy)
		{
			return enemy.m_flLastSeen < 0.f || (flNow - enemy.m_flLastSeen) > flRetention;
		}), m_vCloseEnemies.end());

	std::sort(m_vCloseEnemies.begin(), m_vCloseEnemies.end(), [](const ClosestEnemy_t& a, const ClosestEnemy_t& b) -> bool
		  {
			  return a.m_flDist < b.m_flDist;
		  });

	if (m_vCloseEnemies.empty())
		return {};

	return m_vCloseEnemies.front();
}


void CBotUtils::UpdateBestSlot(CTFPlayer* pLocal)
{
	if (!Vars::Misc::Movement::BotUtils::WeaponSlot.Value)
	{
		m_iBestSlot = -1;
		return;
	}

	if (Vars::Misc::Movement::BotUtils::WeaponSlot.Value != Vars::Misc::Movement::BotUtils::WeaponSlotEnum::Best)
	{
		m_iBestSlot = Vars::Misc::Movement::BotUtils::WeaponSlot.Value - 2;
		return;
	}

	auto pPrimaryWeapon = pLocal->GetWeaponFromSlot(SLOT_PRIMARY);
	auto pSecondaryWeapon = pLocal->GetWeaponFromSlot(SLOT_SECONDARY);
	if (!pPrimaryWeapon || !pSecondaryWeapon)
		return;

	int iPrimaryResAmmo = SDK::WeaponDoesNotUseAmmo(pPrimaryWeapon) ? -1 : pLocal->GetAmmoCount(pPrimaryWeapon->m_iPrimaryAmmoType());
	int iSecondaryResAmmo = SDK::WeaponDoesNotUseAmmo(pSecondaryWeapon) ? -1 : pLocal->GetAmmoCount(pSecondaryWeapon->m_iPrimaryAmmoType());
	switch (pLocal->m_iClass())
	{
	case TF_CLASS_SCOUT:
	{
		if ((!G::AmmoInSlot[SLOT_PRIMARY] && (!G::AmmoInSlot[SLOT_SECONDARY] || (iSecondaryResAmmo != -1 &&
			iSecondaryResAmmo <= SDK::GetWeaponMaxReserveAmmo(G::SavedWepIds[SLOT_SECONDARY], G::SavedDefIndexes[SLOT_SECONDARY]) / 4))) && m_tClosestEnemy.m_flDist <= 200.f)
			m_iBestSlot = SLOT_MELEE;
		else if (G::AmmoInSlot[SLOT_PRIMARY] && m_tClosestEnemy.m_flDist <= 800.f)
			m_iBestSlot = SLOT_PRIMARY;
		else if (G::AmmoInSlot[SLOT_SECONDARY])
			m_iBestSlot = SLOT_SECONDARY;
		break;
	}
	case TF_CLASS_HEAVY:
	{
		if (!G::AmmoInSlot[SLOT_PRIMARY] &&
			(!G::AmmoInSlot[SLOT_SECONDARY] && iSecondaryResAmmo == 0) ||
			(G::SavedDefIndexes[SLOT_MELEE] == Heavy_t_TheHolidayPunch &&
			(m_tClosestEnemy.m_pPlayer && !m_tClosestEnemy.m_pPlayer->IsTaunting() && m_tClosestEnemy.m_pPlayer->IsInvulnerable()) && m_tClosestEnemy.m_flDist < 400.f))
			m_iBestSlot = SLOT_MELEE;
		else if ((!m_tClosestEnemy.m_pPlayer || m_tClosestEnemy.m_flDist <= 900.f) && G::AmmoInSlot[SLOT_PRIMARY])
			m_iBestSlot = SLOT_PRIMARY;
		else if (G::AmmoInSlot[SLOT_SECONDARY] && G::SavedWepIds[SLOT_SECONDARY] == TF_WEAPON_SHOTGUN_HWG)
			m_iBestSlot = SLOT_SECONDARY;
		break;
	}
	case TF_CLASS_MEDIC:
	{
		if (pSecondaryWeapon->As<CWeaponMedigun>()->m_hHealingTarget() || HasMedigunTargets(pLocal, pSecondaryWeapon))
			m_iBestSlot = SLOT_SECONDARY;
		else if (!G::AmmoInSlot[SLOT_PRIMARY] || (m_tClosestEnemy.m_flDist <= 400.f && m_tClosestEnemy.m_pPlayer))
			m_iBestSlot = SLOT_MELEE;
		else
			m_iBestSlot = SLOT_PRIMARY;
		break;
	}
	case TF_CLASS_SPY:
	{
		if (m_tClosestEnemy.m_flDist <= 250.f && m_tClosestEnemy.m_pPlayer)
			m_iBestSlot = SLOT_MELEE;
		else if (G::AmmoInSlot[SLOT_PRIMARY] || iPrimaryResAmmo)
			m_iBestSlot = SLOT_PRIMARY;
		break;
	}
	case TF_CLASS_SNIPER:
	{
		int iPlayerLowHp = m_tClosestEnemy.m_pPlayer ? (m_tClosestEnemy.m_pPlayer->m_iHealth() < m_tClosestEnemy.m_pPlayer->GetMaxHealth() * 0.35f ? 2 : m_tClosestEnemy.m_pPlayer->m_iHealth() < m_tClosestEnemy.m_pPlayer->GetMaxHealth() * 0.75f) : -1;
		if (!G::AmmoInSlot[SLOT_PRIMARY] && !G::AmmoInSlot[SLOT_SECONDARY] || (m_tClosestEnemy.m_flDist <= 200.f && m_tClosestEnemy.m_pPlayer))
			m_iBestSlot = SLOT_MELEE;
		else if ((G::AmmoInSlot[SLOT_SECONDARY] || iSecondaryResAmmo) && (m_tClosestEnemy.m_flDist <= 300.f && iPlayerLowHp > 1))
			m_iBestSlot = SLOT_SECONDARY;
		// Keep the smg if the target we previosly tried shooting is running away
		else if (m_iCurrentSlot != -1 && m_iCurrentSlot < 2 && G::AmmoInSlot[m_iCurrentSlot] && (m_tClosestEnemy.m_flDist <= 800.f && iPlayerLowHp > 1))
			break;
		else if (G::AmmoInSlot[SLOT_PRIMARY])
			m_iBestSlot = SLOT_PRIMARY;
		break;
	}
	case TF_CLASS_PYRO:
	{
		if (!G::AmmoInSlot[SLOT_PRIMARY] && (!G::AmmoInSlot[SLOT_SECONDARY] && iSecondaryResAmmo != -1 &&
			iSecondaryResAmmo <= SDK::GetWeaponMaxReserveAmmo(G::SavedWepIds[SLOT_SECONDARY], G::SavedDefIndexes[SLOT_SECONDARY]) / 4) &&
			(m_tClosestEnemy.m_pPlayer && m_tClosestEnemy.m_flDist <= 300.f))
			m_iBestSlot = SLOT_MELEE;
		else if (G::AmmoInSlot[SLOT_PRIMARY] && (m_tClosestEnemy.m_flDist <= 550.f || !m_tClosestEnemy.m_pPlayer))
			m_iBestSlot = SLOT_PRIMARY;
		else if (G::AmmoInSlot[SLOT_SECONDARY])
			m_iBestSlot = SLOT_SECONDARY;
		break;
	}
	case TF_CLASS_SOLDIER:
	{
		auto pEnemyWeapon = m_tClosestEnemy.m_pPlayer ? m_tClosestEnemy.m_pPlayer->m_hActiveWeapon().Get()->As<CTFWeaponBase>() : nullptr;
		bool bEnemyCanAirblast = pEnemyWeapon && pEnemyWeapon->GetWeaponID() == TF_WEAPON_FLAMETHROWER && pEnemyWeapon->m_iItemDefinitionIndex() != Pyro_m_ThePhlogistinator;
		bool bEnemyClose = m_tClosestEnemy.m_pPlayer && m_tClosestEnemy.m_flDist <= 250.f;
		if ((m_iCurrentSlot != SLOT_PRIMARY || !G::AmmoInSlot[SLOT_PRIMARY] && iPrimaryResAmmo == 0) && bEnemyClose && (m_tClosestEnemy.m_pPlayer->m_iHealth() < 80 ? !G::AmmoInSlot[SLOT_SECONDARY] : m_tClosestEnemy.m_pPlayer->m_iHealth() >= 150 || G::AmmoInSlot[SLOT_SECONDARY] < 2))
			m_iBestSlot = SLOT_MELEE;
		else if (G::AmmoInSlot[SLOT_SECONDARY] && (bEnemyCanAirblast || (m_tClosestEnemy.m_flDist <= 350.f && m_tClosestEnemy.m_pPlayer && m_tClosestEnemy.m_pPlayer->m_iHealth() <= 125)))
			m_iBestSlot = SLOT_SECONDARY;
		else if (G::AmmoInSlot[SLOT_PRIMARY])
			m_iBestSlot = SLOT_PRIMARY;
		break;
	}
	case TF_CLASS_DEMOMAN:
	{
		if (!G::AmmoInSlot[SLOT_PRIMARY] && !G::AmmoInSlot[SLOT_SECONDARY] && (m_tClosestEnemy.m_pPlayer && m_tClosestEnemy.m_flDist <= 200.f))
			m_iBestSlot = SLOT_MELEE;
		else if (G::AmmoInSlot[SLOT_PRIMARY] && (m_tClosestEnemy.m_flDist <= 800.f))
			m_iBestSlot = SLOT_PRIMARY;
		else if (G::AmmoInSlot[SLOT_SECONDARY] || iSecondaryResAmmo >= SDK::GetWeaponMaxReserveAmmo(G::SavedWepIds[SLOT_SECONDARY], G::SavedDefIndexes[SLOT_SECONDARY]) / 2)
			m_iBestSlot = SLOT_SECONDARY;
		break;
	}
	case TF_CLASS_ENGINEER:
	{
		if (!G::AmmoInSlot[SLOT_PRIMARY] && !G::AmmoInSlot[SLOT_SECONDARY] && (m_tClosestEnemy.m_pPlayer && m_tClosestEnemy.m_flDist <= 200.f))
			m_iBestSlot = SLOT_MELEE;
		else if ((G::AmmoInSlot[SLOT_PRIMARY] || iPrimaryResAmmo) && (m_tClosestEnemy.m_pPlayer && m_tClosestEnemy.m_flDist <= 500.f))
			m_iBestSlot = SLOT_PRIMARY;
		else if (G::AmmoInSlot[SLOT_SECONDARY] || iSecondaryResAmmo)
			m_iBestSlot = SLOT_SECONDARY;
		break;
	}
	default:
		break;
	}
}

void CBotUtils::SetSlot(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, int iSlot)
{
	if (iSlot > -1)
	{
		auto sCommand = "slot" + std::to_string(iSlot+1);
		if (m_iCurrentSlot != iSlot)
			I::EngineClient->ClientCmd_Unrestricted(sCommand.c_str());
	}
}

void CBotUtils::DoSlowAim(Vec3& vWishAngles, float flSpeed, Vec3 vPreviousAngles)
{
	// Yaw
	if (vPreviousAngles.y != vWishAngles.y)
	{
		Vec3 vSlowDelta = vWishAngles - vPreviousAngles;

		while (vSlowDelta.y > 180)
			vSlowDelta.y -= 360;
		while (vSlowDelta.y < -180)
			vSlowDelta.y += 360;

		vSlowDelta /= flSpeed;
		vWishAngles = vPreviousAngles + vSlowDelta;

		// Clamp as we changed angles
		Math::ClampAngles(vWishAngles);
	}
}

void CBotUtils::LookAtPathPlain(CTFPlayer* pLocal, CUserCmd* pCmd, Vec2 vDest, bool bSilent, bool bSmooth)
{
	if (!pLocal)
		return;

	Vec3 vEye = pLocal->GetEyePosition();
	Vec3 vWorldTarget{ vDest.x, vDest.y, vEye.z };
	Vec3 vAngles = Math::CalcAngle(vEye, vWorldTarget);
	LookAtPathPlain(pLocal, pCmd, vAngles, bSilent, bSmooth);
}

void CBotUtils::LookAtPathPlain(CTFPlayer* pLocal, CUserCmd* pCmd, Vec3 vWishAngles, bool bSilent, bool bSmooth)
{
	if (!pLocal)
		return;

	if (m_tLookState.initialized)
		ResetLookState();
	Math::ClampAngles(vWishAngles);
	if (bSmooth)
		DoSlowAim(vWishAngles, static_cast<float>(Vars::Misc::Movement::BotUtils::LookAtPathSpeed.Value), m_vLastAngles);
	else
		m_vLastAngles = vWishAngles;

	if (bSilent)
	{
		pCmd->viewangles = bSmooth ? vWishAngles : m_vLastAngles;
	}
	else
	{
		Vec3 vApply = bSmooth ? vWishAngles : m_vLastAngles;
		I::EngineClient->SetViewAngles(vApply);
		pCmd->viewangles = vApply;
	}
	if (bSmooth)
		m_vLastAngles = vWishAngles;
}

void CBotUtils::LookAtPath(CTFPlayer* pLocal, CUserCmd* pCmd, Vec2 vDest, bool bSilent)
{
	if (!pLocal)
		return;

	Vec3 vEye = pLocal->GetEyePosition();
	Vec3 vWorldTarget{ vDest.x, vDest.y, vEye.z };
	Vec3 vWishAngles = Math::CalcAngle(vEye, vWorldTarget);
	LookAtPath(pLocal, pCmd, vWishAngles, bSilent, true);
}

void CBotUtils::LookAtPath(CTFPlayer* pLocal, CUserCmd* pCmd, Vec3 vWishAngles, bool bSilent, bool bSmooth)
{
	if (!pLocal)
		return;

	Math::ClampAngles(vWishAngles);
	if (!bSmooth)
	{
		ResetLookState();
		if (bSilent)
			pCmd->viewangles = vWishAngles;
		else
		{
			I::EngineClient->SetViewAngles(vWishAngles);
			pCmd->viewangles = vWishAngles;
		}
		m_vLastAngles = vWishAngles;
		m_tLookState.currentAngles = vWishAngles;
		m_tLookState.lastRequestedAngles = vWishAngles;
		m_tLookState.scanAngles = vWishAngles;
		m_tLookState.initialized = true;
		return;
	}

	const float flTickInterval = std::max(I::GlobalVars->interval_per_tick, 0.001f);
	float flFrameTime = std::clamp(std::max(I::GlobalVars->frametime, flTickInterval), 0.008f, 0.05f);
	Vec3 vResult = UpdateLookState(pLocal, vWishAngles, pCmd->viewangles, pCmd, flFrameTime);

	if (bSilent)
	{
		pCmd->viewangles = vResult;
	}
	else
	{
		I::EngineClient->SetViewAngles(vResult);
		pCmd->viewangles = vResult;
	}

	m_vLastAngles = vResult;
}

void CBotUtils::AutoScope(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd)
{
	static bool bKeep = false;
	static bool bShouldClearCache = false;
	static Timer tScopeTimer{};
	bool bIsClassic = pWeapon->GetWeaponID() == TF_WEAPON_SNIPERRIFLE_CLASSIC;
	if (!Vars::Misc::Movement::BotUtils::AutoScope.Value || pWeapon->GetWeaponID() != TF_WEAPON_SNIPERRIFLE && !bIsClassic && pWeapon->GetWeaponID() != TF_WEAPON_SNIPERRIFLE_DECAP)
	{
		bKeep = false;
		m_mAutoScopeCache.clear();
		return;
	}

	if (!Vars::Misc::Movement::BotUtils::AutoScopeUseCachedResults.Value)
		bShouldClearCache = true;

	if (bShouldClearCache)
	{
		m_mAutoScopeCache.clear();
		bShouldClearCache = false;
	}
	else if (m_mAutoScopeCache.size())
		bShouldClearCache = true;

	if (bIsClassic)
	{
		if (bKeep)
		{
			if (!(pCmd->buttons & IN_ATTACK))
				pCmd->buttons |= IN_ATTACK;
			if (tScopeTimer.Check(Vars::Misc::Movement::BotUtils::AutoScopeCancelTime.Value)) // cancel classic charge
				pCmd->buttons |= IN_JUMP;
		}
		if (!pLocal->OnSolid() && !(pCmd->buttons & IN_ATTACK))
			bKeep = false;
	}
	else
	{
		if (bKeep)
		{
			if (pLocal->InCond(TF_COND_ZOOMED))
			{
				if (tScopeTimer.Check(Vars::Misc::Movement::BotUtils::AutoScopeCancelTime.Value))
				{
					bKeep = false;
					pCmd->buttons |= IN_ATTACK2;
					return;
				}
			}
		}
	}

	CNavArea* pCurrentDestinationArea = nullptr;
	auto pCrumbs = F::NavEngine.getCrumbs();
	if (pCrumbs->size() > 4)
		pCurrentDestinationArea = pCrumbs->at(4).navarea;

	auto vLocalOrigin = pLocal->GetAbsOrigin();
	auto pLocalNav = pCurrentDestinationArea ? pCurrentDestinationArea : F::NavEngine.findClosestNavSquare(vLocalOrigin);
	if (!pLocalNav)
		return;

	Vector vFrom = pLocalNav->m_center;
	vFrom.z += PLAYER_JUMP_HEIGHT;

	std::vector<std::pair<CBaseEntity*, float>> vEnemiesSorted;
	for (auto pEnemy : H::Entities.GetGroup(EGroupType::PLAYERS_ENEMIES))
	{
		if (pEnemy->IsDormant())
			continue;

		if (ShouldTarget(pLocal, pWeapon, pEnemy->entindex()) != ShouldTargetState_t::TARGET)
			continue;

		vEnemiesSorted.emplace_back(pEnemy, pEnemy->GetAbsOrigin().DistToSqr(vLocalOrigin));
	}

	for (auto pEnemyBuilding : H::Entities.GetGroup(EGroupType::BUILDINGS_ENEMIES))
	{
		if (pEnemyBuilding->IsDormant())
			continue;

		if (ShouldTargetBuilding(pLocal, pEnemyBuilding->entindex()) != ShouldTargetState_t::TARGET)
			continue;

		vEnemiesSorted.emplace_back(pEnemyBuilding, pEnemyBuilding->GetAbsOrigin().DistToSqr(vLocalOrigin));
	}

	if (vEnemiesSorted.empty())
		return;

	std::sort(vEnemiesSorted.begin(), vEnemiesSorted.end(), [&](std::pair<CBaseEntity*, float> a, std::pair<CBaseEntity*, float> b) -> bool { return a.second < b.second; });

	auto CheckVisibility = [&](const Vec3& vTo, int iEntIndex) -> bool
		{
			CGameTrace trace = {};
			CTraceFilterWorldAndPropsOnly filter = {};

			// Trace from local pos first
			SDK::Trace(Vector(vLocalOrigin.x, vLocalOrigin.y, vLocalOrigin.z + PLAYER_JUMP_HEIGHT), vTo, MASK_SHOT | CONTENTS_GRATE, &filter, &trace);
			bool bHit = trace.fraction == 1.0f;
			if (!bHit)
			{
				// Try to trace from our destination pos
				SDK::Trace(vFrom, vTo, MASK_SHOT | CONTENTS_GRATE, &filter, &trace);
				bHit = trace.fraction == 1.0f;
			}

			if (iEntIndex != -1)
				m_mAutoScopeCache[iEntIndex] = bHit;

			if (bHit)
			{
				if (bIsClassic)
					pCmd->buttons |= IN_ATTACK;
				else if (!pLocal->InCond(TF_COND_ZOOMED) && !(pCmd->buttons & IN_ATTACK2))
					pCmd->buttons |= IN_ATTACK2;

				tScopeTimer.Update();
				return bKeep = true;
			}
			return false;
		};

	bool bSimple = Vars::Misc::Movement::BotUtils::AutoScope.Value == Vars::Misc::Movement::BotUtils::AutoScopeEnum::Simple;

	int iMaxTicks = TIME_TO_TICKS(0.5f);
	MoveStorage tStorage;
	for (auto [pEnemy, _] : vEnemiesSorted)
	{
		int iEntIndex = Vars::Misc::Movement::BotUtils::AutoScopeUseCachedResults.Value ? pEnemy->entindex() : -1;
		if (m_mAutoScopeCache.contains(iEntIndex))
		{
			if (m_mAutoScopeCache[iEntIndex])
			{
				if (bIsClassic)
					pCmd->buttons |= IN_ATTACK;
				else if (!pLocal->InCond(TF_COND_ZOOMED) && !(pCmd->buttons & IN_ATTACK2))
					pCmd->buttons |= IN_ATTACK2;

				tScopeTimer.Update();
				bKeep = true;
				break;
			}
			continue;
		}

		Vector vNonPredictedPos = pEnemy->GetAbsOrigin();
		vNonPredictedPos.z += PLAYER_JUMP_HEIGHT;
		if (CheckVisibility(vNonPredictedPos, iEntIndex))
			return;

		if (!bSimple)
		{
			F::MoveSim.Initialize(pEnemy, tStorage, false);
			if (tStorage.m_bFailed)
			{
				F::MoveSim.Restore(tStorage);
				continue;
			}

			for (int i = 0; i < iMaxTicks; i++)
				F::MoveSim.RunTick(tStorage);
		}

		bool bResult = false;
		Vector vPredictedPos = bSimple ? pEnemy->GetAbsOrigin() + pEnemy->GetAbsVelocity() * TICKS_TO_TIME(iMaxTicks) : tStorage.m_vPredictedOrigin;

		auto pTargetNav = F::NavEngine.findClosestNavSquare(vPredictedPos);
		if (pTargetNav)
		{
			Vector vTo = pTargetNav->m_center;

			// If player is in the air dont try to vischeck nav areas below him, check the predicted position instead
			if (!pEnemy->As<CBasePlayer>()->OnSolid() && vTo.DistToSqr(vPredictedPos) >= pow(400.f, 2))
				vTo = vPredictedPos;

			vTo.z += PLAYER_JUMP_HEIGHT;
			bResult = CheckVisibility(vTo, iEntIndex);
		}
		if (!bSimple)
			F::MoveSim.Restore(tStorage);

		if (bResult)
			break;
	}
}

void CBotUtils::Run(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd)
{
	if (!pLocal || !pWeapon)
	{
		Reset();
		return;
	}

	if (!pLocal->IsAlive())
	{
		Reset();
		return;
	}

	RunLegitBot(pLocal, pWeapon, pCmd);

	const bool bAutomationActive = Vars::Misc::Movement::NavBot::Enabled.Value ||
		(Vars::Misc::Movement::FollowBot::Enabled.Value && Vars::Misc::Movement::FollowBot::Targets.Value);

	m_tClosestEnemy = UpdateCloseEnemies(pLocal, pWeapon);
	m_iCurrentSlot = pWeapon->GetSlot();

	if (!bAutomationActive)
	{
		// Keep look state alive for legit look-at-path behaviour while disabling automation specific helpers.
		m_mAutoScopeCache.clear();
		m_iBestSlot = -1;
		return;
	}

	UpdateBestSlot(pLocal);

	if (!F::NavEngine.IsNavMeshLoaded() || (pCmd->buttons & (IN_FORWARD | IN_BACK | IN_MOVERIGHT | IN_MOVELEFT) && !F::Misc.m_bAntiAFK))
	{
		m_mAutoScopeCache.clear();
		return;
	}

	AutoScope(pLocal, pWeapon, pCmd);

	// Spin up the minigun if there are enemies nearby or if we had an active aimbot target 
	if (pWeapon->GetWeaponID() == TF_WEAPON_MINIGUN)
	{
		static Timer tSpinupTimer{};
		if (m_tClosestEnemy.m_pPlayer && m_tClosestEnemy.m_pPlayer->IsAlive() && !m_tClosestEnemy.m_pPlayer->IsInvulnerable() && pWeapon->HasAmmo())
		{
			if (G::AimTarget.m_iEntIndex && G::AimTarget.m_iDuration || m_tClosestEnemy.m_flDist <= 800.f)
				tSpinupTimer.Update();
			if (!tSpinupTimer.Check(3.f)) // 3 seconds until unrev
				pCmd->buttons |= IN_ATTACK2;
		}
	}
}

void CBotUtils::RegisterAimAssist(const Vec3& vAngles, bool bSmooth)
{
	m_vPendingAssistAngles = vAngles;
	m_flPendingAssistExpiry = I::GlobalVars->curtime + (bSmooth ? 0.32f : 0.18f);
	m_bPendingAssist = true;
	m_bPendingAssistSmooth = bSmooth;
}

void CBotUtils::SyncAimbotView(const Vec3& vAngles)
{
	m_vLastAngles = vAngles;
	if (!m_tLookState.initialized)
		return;

	auto& state = m_tLookState;
	state.currentAngles = vAngles;
	state.lastRequestedAngles = vAngles;
	state.scanAngles = vAngles;
	state.curiosityAngles = vAngles;
	state.externalAngles = vAngles;
	state.scanBlend = std::lerp(state.scanBlend, 0.f, 0.8f);
	state.curiosityBlend = std::lerp(state.curiosityBlend, 0.f, 0.8f);
	state.pathOffset = state.pathOffset.Lerp({}, 0.9f);
	state.pathOffsetGoal = state.pathOffset;
}

void CBotUtils::Reset()
{
	m_mAutoScopeCache.clear();
	m_vCloseEnemies.clear();
	m_tClosestEnemy = {};
	m_iBestSlot = -1;
	m_iCurrentSlot = -1;
	m_vLastAngles = {};
	m_vPendingAssistAngles = {};
	m_flPendingAssistExpiry = 0.f;
	m_bPendingAssist = false;
	m_bPendingAssistSmooth = false;
	m_mSpyInvisibility.clear();
	m_bMedicTimerPrimed = false;
	m_tCallMedicTimer.Update();
	m_tSpycheckTimer.Update();
	ResetLookState();
}