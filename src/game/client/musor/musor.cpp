/* Musor on Android - aim assist, projectile lead and trigger.
   Physics is the game's own: CalcPos() from gamecore.h is the exact solver the
   client uses for its predicted projectiles, so the lead is analytic, not guessed. */
#include "musor.h"

#include <base/math.h>
#include <base/system.h>
#include <engine/console.h>
#include <engine/shared/config.h>
#include <generated/protocol.h>
#include <game/gamecore.h>

#include <game/client/gameclient.h>

#include <cmath>

enum
{
	INPUT_DIRECTION = 0,
	INPUT_TARGET_X,
	INPUT_TARGET_Y,
	INPUT_JUMP,
	INPUT_FIRE,
	INPUT_HOOK,
	INPUT_PLAYER_FLAGS,
	INPUT_WANTED_WEAPON,
	INPUT_NEXT_WEAPON,
	INPUT_PREV_WEAPON,
	INPUT_NUM,
};

static const float AIM_RADIUS = 256.0f;

static void ConSetInt(IConsole::IResult *pResult, void *pUser)
{
	*static_cast<int *>(pUser) = pResult->GetInteger(0);
}

void CMusor::OnConsoleInit()
{
	IConsole *pConsole = Console();
	pConsole->Register("musor_aim", "i", CFGFLAG_CLIENT, ConSetInt, &m_Aim, "aim assist, 0/1");
	pConsole->Register("musor_aim_fov", "i", CFGFLAG_CLIENT, ConSetInt, &m_AimFov, "max distance to a target in px");
	pConsole->Register("musor_aim_pred", "i", CFGFLAG_CLIENT, ConSetInt, &m_AimPred, "lead the target with the real projectile speed");
	pConsole->Register("musor_aim_team", "i", CFGFLAG_CLIENT, ConSetInt, &m_AimTeam, "1 = skip players of your own team");
	pConsole->Register("musor_aim_smooth", "i", CFGFLAG_CLIENT, ConSetInt, &m_AimSmooth, "0 = snap, else max degrees turned per tick");
	pConsole->Register("musor_trigger", "i", CFGFLAG_CLIENT, ConSetInt, &m_Trigger, "fire automatically on the target");
	pConsole->Register("musor_trigger_deg", "i", CFGFLAG_CLIENT, ConSetInt, &m_TriggerDeg, "aim error in degrees that still fires");
	pConsole->Register("musor_auto_weapon", "i", CFGFLAG_CLIENT, ConSetInt, &m_AutoWeapon, "pick the weapon that fits the distance");
	pConsole->Register("musor_auto_shotgun", "i", CFGFLAG_CLIENT, ConSetInt, &m_AutoShotgun, "switch to the shotgun when close");
	pConsole->Register("musor_auto_grenade", "i", CFGFLAG_CLIENT, ConSetInt, &m_AutoGrenade, "switch to the grenade when far");
	pConsole->Register("musor_hud", "i", CFGFLAG_CLIENT, ConSetInt, &m_Hud, "print the current target to the console");
}

void CMusor::OnUpdate()
{
	if(!m_Hud || !m_Aim)
		return;
	CGameClient *pClient = GameClient();
	if(!pClient->m_Snap.m_pLocalCharacter || m_TargetId < 0)
		return;
	const int64_t Tick = pClient->Client()->GameTick();
	if(Tick - m_LastLog < 50)
		return;
	m_LastLog = Tick;
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "target %d (%s) err %.1f deg weapon %d ammo %d",
		m_TargetId,
		pClient->m_aClients[m_TargetId].m_aName,
		m_AimError,
		pClient->m_Snap.m_pLocalCharacter->m_Weapon,
		m_Ammo);
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "musor", aBuf);
}

int CMusor::PickTarget(int LocalId, float &OutDist)
{
	CGameClient *pClient = GameClient();
	const vec2 From = pClient->m_PredictedChar.m_Pos;
	const int OwnTeam = pClient->m_aClients[LocalId].m_Team;
	int Best = -1;
	float BestDist = (float)m_AimFov;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == LocalId || !pClient->m_Snap.m_aCharacters[i].m_Active)
			continue;
		if(m_AimTeam && pClient->m_aClients[i].m_Team == OwnTeam)
			continue;
		const float Dist = distance(pClient->m_aClients[i].m_Predicted.m_Pos, From);
		if(Dist <= 0.0f || Dist >= BestDist)
			continue;
		Best = i;
		BestDist = Dist;
	}
	OutDist = BestDist;
	return Best;
}

/* Inverse of CalcPos(): find the launch angle whose trajectory is at the target's
   predicted position at the same moment. Time is found by the point where the
   travelled length of the trajectory equals speed * time. */
static bool SolveTrajectory(const vec2 &From, const vec2 &TargetPos, const vec2 &TargetVel,
	float Speed, float Curvature, float &OutAngle, float &OutTime)
{
	if(Speed <= 1.0f)
		return false;
	auto miss = [&](float Time) {
		const vec2 Aim = TargetPos + TargetVel * Time;
		const float Reach = Time * Speed;
		const float Drop = Curvature / 10000.0f * Reach * Reach;
		const vec2 Delta = Aim - From - vec2(0.0f, Drop);
		return length(Delta) - Reach;
	};
	const float Step = 1.0f / 100.0f;
	float PrevT = Step;
	float Prev = miss(PrevT);
	bool Found = false;
	float Lo = 0.0f, Hi = 0.0f;
	for(float Time = Step * 2.0f; Time < 2.0f; Time += Step)
	{
		const float Cur = miss(Time);
		if(Prev <= 0.0f && Cur > 0.0f)
		{
			Lo = PrevT;
			Hi = Time;
			Found = true;
			break;
		}
		PrevT = Time;
		Prev = Cur;
	}
	if(!Found)
		return false;
	for(int i = 0; i < 24; i++)
	{
		const float Mid = (Lo + Hi) * 0.5f;
		if(miss(Mid) > 0.0f)
			Hi = Mid;
		else
			Lo = Mid;
	}
	const float Time = (Lo + Hi) * 0.5f;
	const vec2 Aim = TargetPos + TargetVel * Time;
	const float Reach = Time * Speed;
	const float Drop = Curvature / 10000.0f * Reach * Reach;
	OutAngle = std::atan2(Aim.y - From.y - Drop, Aim.x - From.x);
	OutTime = Time;
	return true;
}

void CMusor::PickWeapon(int *pData, int Weapon, float Dist)
{
	if(!m_AutoWeapon && !m_AutoShotgun && !m_AutoGrenade)
		return;
	int Wanted = Weapon;
	if(m_AutoShotgun && Dist < 420.0f)
		Wanted = WEAPON_SHOTGUN;
	else if(m_AutoGrenade && Dist > 500.0f)
		Wanted = WEAPON_GRENADE;
	else if(m_AutoWeapon)
		Wanted = Dist < 140.0f ? WEAPON_HAMMER : (Dist < 420.0f ? WEAPON_SHOTGUN : (Dist > 500.0f ? WEAPON_GRENADE : WEAPON_GUN));
	const int Existing = pData[INPUT_WANTED_WEAPON] - 1;
	if(Wanted == Existing || Wanted == Weapon)
	{
		pData[INPUT_WANTED_WEAPON] = Weapon + 1;
		return;
	}
	pData[INPUT_WANTED_WEAPON] = Wanted + 1;
}

void CMusor::AimAt(int *pData, int LocalId, int Weapon)
{
	CGameClient *pClient = GameClient();
	const vec2 From = pClient->m_PredictedChar.m_Pos;
	const vec2 TargetPos = pClient->m_aClients[m_TargetId].m_Predicted.m_Pos;
	const vec2 TargetVel = pClient->m_aClients[m_TargetId].m_Predicted.m_Vel;
	const CTuningParams &T = pClient->m_aTuning[g_Config.m_ClDummy];

	float Angle = std::atan2(TargetPos.y - From.y, TargetPos.x - From.x);
	float Time = 0.0f;
	float Speed = 0.0f;
	float Curvature = 0.0f;
	switch(Weapon)
	{
	case WEAPON_GUN:
		Speed = T.m_WeaponGunSpeed;
		Curvature = T.m_WeaponGunCurvature;
		break;
	case WEAPON_SHOTGUN:
		Speed = T.m_WeaponShotgunSpeed;
		Curvature = T.m_WeaponShotgunCurvature;
		break;
	case WEAPON_GRENADE:
		Speed = T.m_WeaponGrenadeSpeed;
		Curvature = T.m_WeaponGrenadeCurvature;
		break;
	default:
		break;
	}
	const bool Projectile = Speed > 0.0f;
	float UsedAngle = Angle;
	if(Projectile && m_AimPred && SolveTrajectory(From, TargetPos, TargetVel, Speed, Curvature, UsedAngle, Time))
	{
	}
	else if(m_AimPred && Projectile)
	{
		// no solution inside 2 s: fall back to a straight lead
		const float Straight = distance(TargetPos, From) / Speed;
		const vec2 Aim = TargetPos + TargetVel * Straight;
		UsedAngle = std::atan2(Aim.y - From.y, Aim.x - From.x);
		Time = Straight;
	}

	// optional turn rate limit
	if(m_AimSmooth > 0)
	{
		const float MaxStep = m_AimSmooth * (float)pi / 180.0f;
		float Delta = UsedAngle - m_LastAimAngle;
		while(Delta > (float)pi)
			Delta -= 2.0f * (float)pi;
		while(Delta < -(float)pi)
			Delta += 2.0f * (float)pi;
		if(Delta > MaxStep)
			UsedAngle = m_LastAimAngle + MaxStep;
		else if(Delta < -MaxStep)
			UsedAngle = m_LastAimAngle - MaxStep;
	}
	m_LastAimAngle = UsedAngle;
	m_AimAngle = UsedAngle;

	pData[INPUT_TARGET_X] = (int)std::round(std::cos(UsedAngle) * AIM_RADIUS);
	pData[INPUT_TARGET_Y] = (int)std::round(std::sin(UsedAngle) * AIM_RADIUS);

	const float DirectAngle = std::atan2(TargetPos.y - From.y, TargetPos.x - From.x);
	float Error = std::fabs(UsedAngle - DirectAngle) * 180.0f / (float)pi;
	if(Error > 180.0f)
		Error = 360.0f - Error;
	m_AimError = Error;

	if(m_Trigger && Error <= (float)m_TriggerDeg && Time >= 0.0f)
		pData[INPUT_FIRE] = 1;
}

void CMusor::ApplyInput(int *pData, int Size, bool Dummy)
{
	if(Dummy || Size < INPUT_NUM || !m_Aim)
		return;
	CGameClient *pClient = GameClient();
	if(!pClient->m_Snap.m_pLocalCharacter)
	{
		m_TargetId = -1;
		return;
	}
	const int LocalId = pClient->m_Snap.m_LocalClientId;
	if(LocalId < 0)
		return;
	float Dist = 0.0f;
	m_TargetId = PickTarget(LocalId, Dist);
	if(m_TargetId < 0)
		return;
	int Weapon = pClient->m_Snap.m_pLocalCharacter->m_Weapon;
	PickWeapon(pData, Weapon, Dist);
	const int Wanted = pData[INPUT_WANTED_WEAPON] - 1;
	if(Wanted >= 0 && Wanted < NUM_WEAPONS)
		Weapon = Wanted;
	AimAt(pData, LocalId, Weapon);
}
