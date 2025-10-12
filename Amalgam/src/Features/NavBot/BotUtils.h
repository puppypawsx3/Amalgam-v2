#pragma once
#include "NavEngine/NavEngine.h"
#include "../../Utils/Timer/Timer.h"

struct ClosestEnemy_t
{	
	int m_iEntIdx = -1;
	CTFPlayer* m_pPlayer = nullptr;
	float m_flDist = -1.f;
};

enum ShouldTargetState_t
{
	INVALID,
	DONT_TARGET,
	TARGET
};

class CBotUtils
{
private:
	std::unordered_map<int, bool> m_mAutoScopeCache;
	std::vector<ClosestEnemy_t> m_vCloseEnemies;
	bool HasMedigunTargets(CTFPlayer* pLocal, CTFWeaponBase* pWeapon);
	struct LookState_t
	{
		Timer reactionTimer;
		Timer scanCooldownTimer;
		Timer scanHoldTimer;
		Timer pathOffsetTimer;
		Timer curiosityTimer;
		Timer curiosityCooldownTimer;
		Vec3 currentAngles = {};
		Vec3 lastRequestedAngles = {};
		Vec3 scanAngles = {};
		Vec3 pathOffset = {};
		Vec3 pathOffsetGoal = {};
		Vec3 curiosityAngles = {};
		Vec3 externalAngles = {};
		Vec3 assistAngles = {};
		Vec2 angularVelocity = {};
		float reactionDelay = 0.f;
		float scanDuration = 0.75f;
		float scanCooldown = 2.f;
		float pathOffsetInterval = 1.f;
		float scanBlend = 0.f;
		float maxYawSpeed = 220.f;
		float maxPitchSpeed = 150.f;
		float curiosityDuration = 1.f;
		float curiosityCooldown = 8.f;
		float curiosityBlend = 0.f;
		float externalRelease = 0.f;
		float assistExpiry = 0.f;
		bool reactionPending = false;
		bool scanning = false;
		bool curiosityActive = false;
		bool externalActive = false;
		bool assistSmooth = false;
		bool assistActive = false;
		bool initialized = false;
	};
	LookState_t m_tLookState;
	bool BuildScanTarget(CTFPlayer* pLocal, const Vec3& baseAngles, Vec3& outAngles) const;
	bool IsLineVisible(const Vec3& from, const Vec3& to) const;
	Vec3 AdjustForObstacles(CTFPlayer* pLocal, const Vec3& candidateAngles, const Vec3& referenceAngles) const;
	Vec3 UpdateLookState(CTFPlayer* pLocal, const Vec3& desiredAngles, const Vec3& currentCmdAngles, const CUserCmd* pCmd, float frameTime);
	void ResetLookState();
	Vec3 m_vPendingAssistAngles = {};
	float m_flPendingAssistExpiry = 0.f;
	bool m_bPendingAssist = false;
	bool m_bPendingAssistSmooth = false;
public:
	int m_iCurrentSlot = -1;
	int m_iBestSlot = -1;
	ClosestEnemy_t m_tClosestEnemy = {};
	Vec3 m_vLastAngles = {};

	ShouldTargetState_t ShouldTarget(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, int iPlayerIdx);
	ShouldTargetState_t ShouldTargetBuilding(CTFPlayer* pLocal, int iEntIdx);

	ClosestEnemy_t UpdateCloseEnemies(CTFPlayer* pLocal, CTFWeaponBase* pWeapon);
	void UpdateBestSlot(CTFPlayer* pLocal);
	void SetSlot(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, int iSlot);

	void DoSlowAim(Vec3& vWishAngles, float flSpeed , Vec3 vPreviousAngles);
	void LookAtPathPlain(CTFPlayer* pLocal, CUserCmd* pCmd, Vec2 vDest, bool bSilent, bool bSmooth = true);
	void LookAtPathPlain(CTFPlayer* pLocal, CUserCmd* pCmd, Vec3 vWishAngles, bool bSilent, bool bSmooth = true);
	void LookAtPath(CTFPlayer* pLocal, CUserCmd* pCmd, Vec2 vDest, bool bSilent);
	void LookAtPath(CTFPlayer* pLocal, CUserCmd* pCmd, Vec3 vWishAngles, bool bSilent, bool bSmooth = true);
	void RegisterAimAssist(const Vec3& vAngles, bool bSmooth);
	void SyncAimbotView(const Vec3& vAngles);

	void AutoScope(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd);
	void Run(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd);
	void Reset();
};

ADD_FEATURE(CBotUtils, BotUtils);