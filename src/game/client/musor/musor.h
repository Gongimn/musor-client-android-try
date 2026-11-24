/* Musor on Android - client-side aim assist layer.
   Written against the public CGameClient API of DDNet (GPL-3.0). */
#ifndef GAME_CLIENT_MUSOR_MUSOR_H
#define GAME_CLIENT_MUSOR_MUSOR_H

#include <base/vmath.h>
#include <game/client/component.h>

class CMusor : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }

	void OnConsoleInit() override;
	void OnUpdate() override;

	// Called from CGameClient::OnSnapInput after the normal input has been built.
	// pData is the raw int array that goes on the wire, Size its length in ints.
	void ApplyInput(int *pData, int Size, bool Dummy);

	// settings, written by the musor_* console variables
	int m_Aim = 1; // aim assist on
	int m_AimFov = 1100; // max distance to a valid target, px
	int m_AimPred = 1; // lead the target with the projectile's real speed
	int m_AimTeam = 1; // 0 = shoot anyone, 1 = only other team
	int m_AimSmooth = 0; // 0 = snap, >0 = lerp percent per tick (1..100)
	int m_Trigger = 0; // fire on the target automatically
	int m_TriggerDeg = 9; // aim error, degrees, under which the trigger fires
	int m_AutoWeapon = 0; // switch to the best weapon for the range
	int m_AutoShotgun = 0; // pull the shotgun when close and loaded
	int m_AutoGrenade = 0; // pull the grenade when far and loaded
	int m_Hud = 1; // console line about the current target, once a second

private:
	int m_TargetId = -1;
	float m_AimError = 0.0f;
	float m_AimAngle = 0.0f;
	float m_LastAimAngle = 0.0f;
	int m_Ammo = 0;
	int64_t m_LastLog = 0;

	int PickTarget(int LocalId, float &OutDist);
	void AimAt(int *pData, int LocalId, int Weapon);
	void PickWeapon(int *pData, int Weapon, float Dist);
};

#endif
