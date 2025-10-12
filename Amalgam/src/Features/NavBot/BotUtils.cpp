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
	m_tLookState.maxYawSpeed = 210.f;
	m_tLookState.maxPitchSpeed = 140.f;
	m_tLookState.curiosityCooldown = SDK::RandomFloat(6.f, 11.f);
	m_tLookState.curiosityDuration = SDK::RandomFloat(0.9f, 1.5f);
	m_tLookState.reactionTimer.Update();
	m_tLookState.scanCooldownTimer.Update();
	m_tLookState.pathOffsetTimer.Update();
	m_tLookState.scanHoldTimer.Update();
	m_tLookState.curiosityTimer.Update();
	m_tLookState.curiosityCooldownTimer.Update();
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
		state.pathOffsetGoal.x = SDK::RandomFloat(-0.55f, 0.55f);
		state.pathOffsetGoal.y = SDK::RandomFloat(-1.9f, 1.9f);
		state.pathOffsetInterval = SDK::RandomFloat(1.0f, 1.9f);
		state.pathOffsetTimer.Update();
	}
	state.pathOffset = state.pathOffset.Lerp(state.pathOffsetGoal, frameTime * 2.4f);
	const float flReturnBlend = std::clamp(1.f - state.externalRelease * 2.3f, 0.2f, 1.f);
	Vec3 vWithOffset = vDesired + (state.pathOffset * flReturnBlend);
	Math::ClampAngles(vWithOffset);

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
			state.curiosityCooldown = SDK::RandomFloat(6.f, 12.f);
			state.curiosityCooldownTimer.Update();
		}
	}
	else if (!state.scanning && flFocusDelta < 20.f && state.curiosityCooldownTimer.Check(state.curiosityCooldown))
	{
		state.curiosityActive = true;
		state.curiosityDuration = SDK::RandomFloat(0.9f, 1.6f);
		Vec3 vCuriosity = vWithOffset + Vec3(SDK::RandomFloat(-6.f, 6.f), SDK::RandomFloat(-60.f, 60.f), SDK::RandomFloat(-4.f, 8.f));
		Math::ClampAngles(vCuriosity);
		state.curiosityAngles = AdjustForObstacles(pLocal, vCuriosity, vWithOffset);
		state.curiosityTimer.Update();
		state.curiosityBlend = 0.f;
	}

	float flCuriosityTarget = (state.curiosityActive && !state.scanning) ? 1.f : 0.f;
	state.curiosityBlend = std::lerp(state.curiosityBlend, flCuriosityTarget, frameTime * (state.curiosityActive ? 1.9f : 3.2f));
	if (state.curiosityBlend > 0.001f)
	{
		float flAdjustedCuriosity = std::clamp(state.curiosityBlend * flReturnBlend, 0.f, 1.f);
		vWithOffset = vWithOffset.LerpAngle(state.curiosityAngles, flAdjustedCuriosity);
	}
	else
	{
		state.curiosityAngles = vWithOffset;
	}

	float flExternalDamp = std::clamp(1.f - state.externalRelease * 3.2f, 0.f, 1.f);
	Vec3 vFinalTarget = vWithOffset;
	Math::ClampAngles(vFinalTarget);
	vFinalTarget = AdjustForObstacles(pLocal, vFinalTarget, state.currentAngles);

	const float flBaseSpeed = std::max(6.f, static_cast<float>(Vars::Misc::Movement::BotUtils::LookAtPathSpeed.Value));
	const float flYawDelta = AngleDistance(vFinalTarget.y, state.currentAngles.y);
	const float flPitchDelta = AngleDistance(vFinalTarget.x, state.currentAngles.x);

	const float flYawIntensity = Math::SimpleSpline(std::clamp(flYawDelta / 95.f, 0.f, 1.f));
	const float flPitchIntensity = Math::SimpleSpline(std::clamp(flPitchDelta / 80.f, 0.f, 1.f));

	const float flYawMin = flBaseSpeed * 0.65f;
	const float flPitchMin = flBaseSpeed * 0.48f;
	float flYawTargetSpeed = std::lerp(flYawMin, state.maxYawSpeed, flYawIntensity);
	float flPitchTargetSpeed = std::lerp(flPitchMin, state.maxPitchSpeed, flPitchIntensity);

	const float flAcceleration = 510.f;
	const float flFrameAccel = flAcceleration * frameTime;
	state.angularVelocity.x = ApproachFloat(flYawTargetSpeed, state.angularVelocity.x, flFrameAccel);
	state.angularVelocity.y = ApproachFloat(flPitchTargetSpeed, state.angularVelocity.y, flFrameAccel * 0.75f);

	float flYawStep = state.angularVelocity.x * frameTime;
	float flPitchStep = state.angularVelocity.y * frameTime;

	Vec3 vPrevAngles = state.currentAngles;
	Vec3 vUpdated = vPrevAngles;
	vUpdated.y = ApproachAngle(vFinalTarget.y, vUpdated.y, flYawStep);
	vUpdated.x = ApproachAngle(vFinalTarget.x, vUpdated.x, flPitchStep);
	vUpdated.z = 0.f;
	Math::ClampAngles(vUpdated);

	const float flAppliedYaw = AngleDistance(vUpdated.y, vPrevAngles.y) / std::max(frameTime, 0.0001f);
	const float flAppliedPitch = AngleDistance(vUpdated.x, vPrevAngles.x) / std::max(frameTime, 0.0001f);
	state.angularVelocity.x = ApproachFloat(flAppliedYaw, state.angularVelocity.x, flFrameAccel * 0.45f);
	state.angularVelocity.y = ApproachFloat(flAppliedPitch, state.angularVelocity.y, flFrameAccel * 0.35f);

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

#ifdef TEXTMODE
	if (auto pResource = H::Entities.GetResource(); pResource && F::NamedPipe.IsLocalBot(pResource->m_iAccountID(iPlayerIdx)))
		return ShouldTargetState_t::DONT_TARGET;
#endif

	if (F::PlayerUtils.IsIgnored(iPlayerIdx))
		return ShouldTargetState_t::DONT_TARGET;

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
		if (!ShouldTarget(pLocal, pWeapon, iEntIndex))
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

	float flFrameTime = std::clamp(I::GlobalVars->frametime, 0.001f, 0.05f);
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

		if (!ShouldTarget(pLocal, pWeapon, pEnemy->entindex()))
			continue;

		vEnemiesSorted.emplace_back(pEnemy, pEnemy->GetAbsOrigin().DistToSqr(vLocalOrigin));
	}

	for (auto pEnemyBuilding : H::Entities.GetGroup(EGroupType::BUILDINGS_ENEMIES))
	{
		if (pEnemyBuilding->IsDormant())
			continue;

		if (!ShouldTargetBuilding(pLocal, pEnemyBuilding->entindex()))
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
	ResetLookState();
}