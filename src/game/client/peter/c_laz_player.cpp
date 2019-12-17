#include "cbase.h"
#include "c_laz_player.h"
#include "choreoevent.h"
#include "cam_thirdperson.h"
#include "in_buttons.h"
#include "c_ai_basenpc.h"
#include "c_team.h"
#include "collisionutils.h"
#include "model_types.h"
#include "iclientshadowmgr.h"
#include "flashlighteffect.h"
#include "view_scene.h"
#include "view.h"
#include "PortalRender.h"
#include "iclientvehicle.h"
#include "bone_setup.h"
#include "functionproxy.h"
#include "c_laz_player_resource.h"
#include "multiplayer/basenetworkedragdoll_cl.h"
#include "C_PortalGhostRenderable.h"
#include "fmtstr.h"

static Vector TF_TAUNTCAM_HULL_MIN(-9.0f, -9.0f, -9.0f);
static Vector TF_TAUNTCAM_HULL_MAX(9.0f, 9.0f, 9.0f);

static ConVar tf_tauntcam_yaw("tf_tauntcam_yaw", "0", FCVAR_CHEAT | FCVAR_DEVELOPMENTONLY);
static ConVar tf_tauntcam_pitch("tf_tauntcam_pitch", "0", FCVAR_CHEAT | FCVAR_DEVELOPMENTONLY);
static ConVar tf_tauntcam_dist("tf_tauntcam_dist", "150", FCVAR_CHEAT | FCVAR_DEVELOPMENTONLY);

static ConVar cl_playermodel("cl_playermodel", "none", FCVAR_USERINFO | FCVAR_ARCHIVE | FCVAR_SERVER_CAN_EXECUTE, "Default Player Model");
static ConVar cl_defaultweapon("cl_defaultweapon", "weapon_physcannon", FCVAR_USERINFO | FCVAR_ARCHIVE, "Default Spawn Weapon");
static ConVar cl_laz_mp_suit("cl_laz_mp_suit", "1", FCVAR_USERINFO | FCVAR_ARCHIVE, "Enable suit voice in multiplayer");

extern ConVar cl_meathook_neck_pivot_ingame_up;
extern ConVar cl_meathook_neck_pivot_ingame_fwd;

#define FLASHLIGHT_DISTANCE		1000

ConVar	cl_legs_enable("cl_legs_enable", "1", FCVAR_ARCHIVE, "0 hides the legs, 1 shows the legs, 2 shows the legs and hair", true, 0.f, true, 2.f);

IMPLEMENT_NETWORKCLASS_ALIASED(Laz_Player, DT_Laz_Player);

BEGIN_RECV_TABLE(C_Laz_Player, DT_Laz_Player)
RecvPropInt(RECVINFO(m_bHasLongJump)),
RecvPropInt(RECVINFO(m_iPlayerSoundType)),

RecvPropBool(RECVINFO(m_fIsWalking)),
RecvPropInt(RECVINFO(m_nFlashlightType)),
END_RECV_TABLE();

BEGIN_PREDICTION_DATA(C_Laz_Player)
DEFINE_PRED_FIELD(m_fIsWalking, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE),
END_PREDICTION_DATA();

class CPlayerColorConvarSystem : public CAutoGameSystem, public IPlayerColorConvars
{
public:
	CPlayerColorConvarSystem() : CAutoGameSystem("PlayerColorConvarSystem")
	{
		for (int i = 0; i < NUM_PLAYER_COLORS; i++)
		{
			m_pConvars[i] = nullptr;
		}
	}

	virtual bool Init()
	{
		for (int i = 0; i < NUM_PLAYER_COLORS; i++)
		{
			CFmtStr str("cl_laz_player_color%i", i);
			m_pszConvarNames[i] = V_strdup(str.Access());

			m_pConvars[i] = new ConVar(m_pszConvarNames[i], "0 1 1", FCVAR_ARCHIVE | FCVAR_USERINFO);
		}

		return true;
	}

	virtual void Shutdown()
	{
		for (int i = 0; i < NUM_PLAYER_COLORS; i++)
		{
			if (m_pConvars[i])
			{
				g_pCVar->UnregisterConCommand(m_pConvars[i]);
				delete m_pConvars[i];
				m_pConvars[i] = nullptr;
				delete m_pszConvarNames[i];
				m_pszConvarNames[i] = nullptr;
			}
		}
	}

	virtual ConVar* GetColorConVar(int iColor)
	{
		if (iColor < 0 || iColor >= NUM_PLAYER_COLORS)
			return nullptr;

		return m_pConvars[iColor];
	}

protected:
	ConVar* m_pConvars[NUM_PLAYER_COLORS];
	const char* m_pszConvarNames[NUM_PLAYER_COLORS];
};

CPlayerColorConvarSystem g_ColorConvarSystem;
IPlayerColorConvars* g_pColorConvars = &g_ColorConvarSystem;

class CLazPlayerColorProxy : public CResultProxy
{
public:
	virtual bool Init(IMaterial* pMaterial, KeyValues* pKeyValues);
	virtual void OnBind(void*);

protected:
	virtual int GetPlayerColor() { return PLRCOLOR_CLOTHING; }
	Vector GetDefaultColor();
	IMaterialVar* m_pSrc1;
};

Vector CLazPlayerColorProxy::GetDefaultColor()
{
	if (m_pSrc1)
	{
		Vector vRet;
		V_memcpy(vRet.Base(), m_pSrc1->GetVecValue(), sizeof(vec_t) * 3);
	}

	return Vector(62 / 255, 88 / 255, 106 / 255);
}

bool CLazPlayerColorProxy::Init(IMaterial* pMaterial, KeyValues* pKeyValues)
{
	if (!CResultProxy::Init(pMaterial, pKeyValues))
		return false;

	char const* pSrcVar1 = pKeyValues->GetString("defaultColorVar", nullptr);
	if (pSrcVar1)
	{
		bool foundVar;
		m_pSrc1 = pMaterial->FindVar(pSrcVar1, &foundVar, true);
		if (!foundVar)
			return false;
	}
	else
	{
		m_pSrc1 = nullptr;
	}

	return true;
}

void CLazPlayerColorProxy::OnBind(void* pRend)
{
	C_BaseEntity* pEnt = BindArgToEntity(pRend);
	if (pEnt)
	{
		int iEntIndex = 0;

		C_PortalGhostRenderable* pGhostAnim = dynamic_cast<C_PortalGhostRenderable*> (pEnt);
		if (pGhostAnim)
			pEnt = pGhostAnim->m_pGhostedRenderable;

		//C_BasePlayer* pPlayer = ToBasePlayer(pEnt);
		if (pEnt->IsPlayer())
		{
			iEntIndex = pEnt->entindex();
		}
		else
		{
			C_BaseNetworkedRagdoll* pRagdoll = dynamic_cast<C_BaseNetworkedRagdoll*> (pEnt);
			if (pRagdoll)
				iEntIndex = pRagdoll->GetPlayerEntIndex();
			else
			{
				C_BaseCombatWeapon* pWeapon = dynamic_cast<C_BaseCombatWeapon*> (pEnt);
				if (pWeapon)
				{
					CBaseCombatCharacter* pOwner = pWeapon->GetOwner();
					if (pOwner && pOwner->IsPlayer())
					{
						iEntIndex = pOwner->entindex();
					}
				}
				else
				{
					C_BaseViewModel* pViewModel = dynamic_cast<C_BaseViewModel*> (pEnt);
					if (pViewModel)
					{
						C_BaseEntity* pPlayer = pViewModel->GetOwner();
						if (pPlayer)
							iEntIndex = pPlayer->entindex();
					}
				}
			}
		}

		if (iEntIndex > 0)
		{
			C_LAZ_PlayerResource* pLazRes = (C_LAZ_PlayerResource*)g_PR;
			if (pLazRes)
			{
				Vector vecColor = pLazRes->GetPlayerColorVector(iEntIndex, GetPlayerColor());
				m_pResult->SetVecValue(vecColor.Base(), 3);
				return;
			}
		}
	}

	m_pResult->SetVecValue(GetDefaultColor().Base(), 3);
}

class CLazPlayerGlowColorProxy : public CLazPlayerColorProxy
{
protected:
	virtual int GetPlayerColor() { return PLRCOLOR_CLOTHING_GLOW; }
};

class CLazPlayerWeaponColorProxy : public CLazPlayerColorProxy
{
protected:
	virtual int GetPlayerColor() { return PLRCOLOR_WEAPON; }
};

EXPOSE_INTERFACE(CLazPlayerColorProxy, IMaterialProxy, "PlayerColor" IMATERIAL_PROXY_INTERFACE_VERSION);
EXPOSE_INTERFACE(CLazPlayerWeaponColorProxy, IMaterialProxy, "PlayerWeaponColor" IMATERIAL_PROXY_INTERFACE_VERSION);
EXPOSE_INTERFACE(CLazPlayerGlowColorProxy, IMaterialProxy, "PlayerGlowColor" IMATERIAL_PROXY_INTERFACE_VERSION);

#define	HL2_WALK_SPEED 150
#define	HL2_NORM_SPEED 190
#define	HL2_SPRINT_SPEED 320

bool C_Laz_Player::ShouldDoPortalRenderCulling()
{
	return (cl_legs_enable.GetInt() < 1);
}

void C_Laz_Player::PreThink(void)
{
	BaseClass::PreThink();

	HandleSpeedChanges();

	if (m_HL2Local.m_flSuitPower <= 0.0f)
	{
		if (IsSprinting())
		{
			StopSprinting();
		}
	}

	// Disallow shooting while zooming
	if (IsX360())
	{
		if (m_hZoomOwner.Get() != nullptr)
		{
			if (GetActiveWeapon() && !GetActiveWeapon()->IsWeaponZoomed())
			{
				// If not zoomed because of the weapon itself, do not attack.
				m_nButtons &= ~(IN_ATTACK | IN_ATTACK2);
			}
		}
	}
	else
	{
		if (m_nButtons & IN_ZOOM)
		{
			//FIXME: Held weapons like the grenade get sad when this happens
#ifdef HL2_EPISODIC
		// Episodic allows players to zoom while using a func_tank
			CBaseCombatWeapon* pWep = GetActiveWeapon();
			if (!GetUseEntity() || (pWep && pWep->IsWeaponVisible()))
#endif
				m_nButtons &= ~(IN_ATTACK | IN_ATTACK2);
		}
	}
}

void C_Laz_Player::ClientThink()
{
	BaseClass::ClientThink();

	if (IsLocalPlayer())
	{
		
	}
	else
	{
		// Cold breath.
		UpdateColdBreath();
	}
}

float C_Laz_Player::GetFOV( void )
{
	//Find our FOV with offset zoom value
	float flFOVOffset = C_BasePlayer::GetFOV() + GetZoom();

	// Clamp FOV in MP
	float min_fov = GetMinFOV();

	// Don't let it go too low
	flFOVOffset = MAX( min_fov, flFOVOffset );

	return flFOVOffset;
}

void C_Laz_Player::OnDataChanged(DataUpdateType_t type)
{
	BaseClass::OnDataChanged(type);

	if (type == DATA_UPDATE_CREATED)
	{
		SetNextClientThink(CLIENT_THINK_ALWAYS);
	}
	else
	{
		UpdateWearables();
	}

	UpdateVisibility();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool C_Laz_Player::StartSceneEvent(CSceneEventInfo *info, CChoreoScene *scene, CChoreoEvent *event, CChoreoActor *actor, C_BaseEntity *pTarget)
{
	switch (event->GetType())
	{
	case CChoreoEvent::SEQUENCE:
	case CChoreoEvent::GESTURE:
		return StartGestureSceneEvent(info, scene, event, actor, pTarget);
	default:
		return BaseClass::StartSceneEvent(info, scene, event, actor, pTarget);
	}
}

void C_Laz_Player::CalcView(Vector & eyeOrigin, QAngle & eyeAngles, float & zNear, float & zFar, float & fov)
{
	HandleTaunting();
	BaseClass::CalcView(eyeOrigin, eyeAngles, zNear, zFar, fov);
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool C_Laz_Player::StartGestureSceneEvent(CSceneEventInfo *info, CChoreoScene *scene, CChoreoEvent *event, CChoreoActor *actor, CBaseEntity *pTarget)
{
	// Get the (gesture) sequence.
	info->m_nSequence = LookupSequence(event->GetParameters());
	if (info->m_nSequence < 0)
		return false;

	// Player the (gesture) sequence.
	if (GetAnimState())
		GetAnimState()->AddVCDSequenceToGestureSlot(GESTURE_SLOT_VCD, info->m_nSequence);

	return true;
}

void C_Laz_Player::TurnOnTauntCam(void)
{
	if (!IsLocalPlayer())
		return;

	// Already in third person?
	if (g_ThirdPersonManager.WantToUseGameThirdPerson())
		return;

	// Save the old view angles.
	/*engine->GetViewAngles( m_angTauntEngViewAngles );
	prediction->GetViewAngles( m_angTauntPredViewAngles );*/

	m_TauntCameraData.m_flPitch = tf_tauntcam_pitch.GetFloat();
	m_TauntCameraData.m_flYaw = tf_tauntcam_yaw.GetFloat();
	m_TauntCameraData.m_flDist = tf_tauntcam_dist.GetFloat();
	m_TauntCameraData.m_flLag = 4.0f;
	m_TauntCameraData.m_vecHullMin = TF_TAUNTCAM_HULL_MIN;
	m_TauntCameraData.m_vecHullMax = TF_TAUNTCAM_HULL_MAX;

	QAngle vecCameraOffset(tf_tauntcam_pitch.GetFloat(), tf_tauntcam_yaw.GetFloat(), tf_tauntcam_dist.GetFloat());

	g_ThirdPersonManager.SetDesiredCameraOffset(Vector(tf_tauntcam_dist.GetFloat(), 0.0f, 0.0f));
	g_ThirdPersonManager.SetOverridingThirdPerson(true);
	::input->CAM_ToThirdPerson();
	ThirdPersonSwitch(true);

	::input->CAM_SetCameraThirdData(&m_TauntCameraData, vecCameraOffset);

	if (GetActiveWeapon())
	{
		GetActiveWeapon()->UpdateVisibility();
	}
}

void C_Laz_Player::TurnOffTauntCam(void)
{
	if (!IsLocalPlayer())
		return;

	/*Vector vecOffset = g_ThirdPersonManager.GetCameraOffsetAngles();

	tf_tauntcam_pitch.SetValue( vecOffset[PITCH] - m_angTauntPredViewAngles[PITCH] );
	tf_tauntcam_yaw.SetValue( vecOffset[YAW] - m_angTauntPredViewAngles[YAW] );*/

	g_ThirdPersonManager.SetOverridingThirdPerson(false);
	::input->CAM_SetCameraThirdData(NULL, vec3_angle);

	if (g_ThirdPersonManager.WantToUseGameThirdPerson())
	{
		ThirdPersonSwitch(true);
		return;
	}

	::input->CAM_ToFirstPerson();
	ThirdPersonSwitch(false);

	// Reset the old view angles.
	/*engine->SetViewAngles( m_angTauntEngViewAngles );
	prediction->SetViewAngles( m_angTauntPredViewAngles );*/

	// Force the feet to line up with the view direction post taunt.
	if (GetAnimState())
		GetAnimState()->m_bForceAimYaw = true;

	if (GetViewModel())
	{
		GetViewModel()->UpdateVisibility();
	}
}

void C_Laz_Player::HandleTaunting(void)
{
	C_BasePlayer *pLocalPlayer = C_BasePlayer::GetLocalPlayer();

	bool bUseTauntCam = false;

	if (GetAnimState() && GetAnimState()->IsPlayingCustomSequence())
		bUseTauntCam = true;

	// Clear the taunt slot.
	if (!m_bWasTaunting && (bUseTauntCam))
	{
		m_bWasTaunting = true;

		// Handle the camera for the local player.
		if (pLocalPlayer)
		{
			TurnOnTauntCam();
		}
	}

	if (m_bWasTaunting && (!bUseTauntCam))
	{
		m_bWasTaunting = false;

		// Clear the vcd slot.
		//m_PlayerAnimState->ResetGestureSlot(GESTURE_SLOT_VCD);

		// Handle the camera for the local player.
		if (pLocalPlayer)
		{
			TurnOffTauntCam();
		}
	}
}

void C_Laz_Player::UpdateOnRemove()
{
	// Stop the taunt.
	if (m_bWasTaunting)
	{
		TurnOffTauntCam();
	}

	BaseClass::UpdateOnRemove();
}

//-----------------------------------------------------------------------------
// Purpose: Try to steer away from any players and objects we might interpenetrate
//-----------------------------------------------------------------------------
#define TF_AVOID_MAX_RADIUS_SQR		5184.0f			// Based on player extents and max buildable extents.
#define TF_OO_AVOID_MAX_RADIUS_SQR	0.00019f

ConVar tf_max_separation_force("tf_max_separation_force", "256", FCVAR_DEVELOPMENTONLY);

extern ConVar cl_forwardspeed;
extern ConVar cl_backspeed;
extern ConVar cl_sidespeed;

void C_Laz_Player::AvoidPlayers(CUserCmd *pCmd)
{
	// Don't test if the player doesn't exist or is dead.
	if (IsAlive() == false)
		return;

	C_Team *pTeam = (C_Team *)GetTeam();
	if (!pTeam)
		return;

	// Up vector.
	static Vector vecUp(0.0f, 0.0f, 1.0f);

	Vector vecTFPlayerCenter = GetAbsOrigin();
	Vector vecTFPlayerMin = GetPlayerMins();
	Vector vecTFPlayerMax = GetPlayerMaxs();
	float flZHeight = vecTFPlayerMax.z - vecTFPlayerMin.z;
	vecTFPlayerCenter.z += 0.5f * flZHeight;
	VectorAdd(vecTFPlayerMin, vecTFPlayerCenter, vecTFPlayerMin);
	VectorAdd(vecTFPlayerMax, vecTFPlayerCenter, vecTFPlayerMax);

	// Find an intersecting player or object.
	int nAvoidPlayerCount = 0;
	C_Laz_Player *pAvoidPlayerList[MAX_PLAYERS];

	C_Laz_Player *pIntersectPlayer = NULL;
	//C_AI_BaseNPC *pIntersectObject = NULL;
	float flAvoidRadius = 0.0f;

	Vector vecAvoidCenter, vecAvoidMin, vecAvoidMax;
	for (int i = 0; i < pTeam->GetNumPlayers(); ++i)
	{
		C_Laz_Player *pAvoidPlayer = static_cast<C_Laz_Player *>(pTeam->GetPlayer(i));
		if (pAvoidPlayer == NULL)
			continue;
		// Is the avoid player me?
		if (pAvoidPlayer == this)
			continue;

		// Save as list to check against for objects.
		pAvoidPlayerList[nAvoidPlayerCount] = pAvoidPlayer;
		++nAvoidPlayerCount;

		// Check to see if the avoid player is dormant.
		if (pAvoidPlayer->IsDormant())
			continue;

		// Is the avoid player solid?
		if (pAvoidPlayer->IsSolidFlagSet(FSOLID_NOT_SOLID))
			continue;

		vecAvoidCenter = pAvoidPlayer->GetAbsOrigin();
		vecAvoidMin = pAvoidPlayer->GetPlayerMins();
		vecAvoidMax = pAvoidPlayer->GetPlayerMaxs();
		flZHeight = vecAvoidMax.z - vecAvoidMin.z;
		vecAvoidCenter.z += 0.5f * flZHeight;
		VectorAdd(vecAvoidMin, vecAvoidCenter, vecAvoidMin);
		VectorAdd(vecAvoidMax, vecAvoidCenter, vecAvoidMax);

		if (IsBoxIntersectingBox(vecTFPlayerMin, vecTFPlayerMax, vecAvoidMin, vecAvoidMax))
		{
			// Need to avoid this player.
			if (!pIntersectPlayer)
			{
				pIntersectPlayer = pAvoidPlayer;
				break;
			}
		}
	}
#if 0
	// We didn't find a player - look for objects to avoid.
	if (!pIntersectPlayer)
	{
		for (int iPlayer = 0; iPlayer < nAvoidPlayerCount; ++iPlayer)
		{
			// Stop when we found an intersecting object.
			if (pIntersectObject)
				break;


			for (int iObject = 0; iObject < pTeam->GetNumNPCs(); ++iObject)
			{
				C_AI_BaseNPC *pAvoidObject = pTeam->GetNPC(iObject);
				if (!pAvoidObject)
					continue;

				// Check to see if the object is dormant.
				if (pAvoidObject->IsDormant())
					continue;

				// Is the object solid.
				if (pAvoidObject->IsSolidFlagSet(FSOLID_NOT_SOLID))
					continue;

				// If we shouldn't avoid it, see if we intersect it.
				//if (pAvoidObject->ShouldPlayersAvoid())
				{
					vecAvoidCenter = pAvoidObject->WorldSpaceCenter();
					vecAvoidMin = pAvoidObject->WorldAlignMins();
					vecAvoidMax = pAvoidObject->WorldAlignMaxs();
					VectorAdd(vecAvoidMin, vecAvoidCenter, vecAvoidMin);
					VectorAdd(vecAvoidMax, vecAvoidCenter, vecAvoidMax);

					if (IsBoxIntersectingBox(vecTFPlayerMin, vecTFPlayerMax, vecAvoidMin, vecAvoidMax))
					{
						// Need to avoid this object.
						pIntersectObject = pAvoidObject;
						break;
					}
				}
			}
		}
	}
#endif
	// Anything to avoid?
	if (!pIntersectPlayer /*&& !pIntersectObject*/)
	{
		return;
	}

	// Calculate the push strength and direction.
	Vector vecDelta;

	// Avoid a player - they have precedence.
	if (pIntersectPlayer)
	{
		VectorSubtract(pIntersectPlayer->WorldSpaceCenter(), vecTFPlayerCenter, vecDelta);

		Vector vRad = pIntersectPlayer->WorldAlignMaxs() - pIntersectPlayer->WorldAlignMins();
		vRad.z = 0;

		flAvoidRadius = vRad.Length();
	}
	// Avoid a object.
	/*else
	{
		VectorSubtract(pIntersectObject->WorldSpaceCenter(), vecTFPlayerCenter, vecDelta);

		Vector vRad = pIntersectObject->WorldAlignMaxs() - pIntersectObject->WorldAlignMins();
		vRad.z = 0;

		flAvoidRadius = vRad.Length();
	}*/

	float flPushStrength = RemapValClamped(vecDelta.Length(), flAvoidRadius, 0, 0, tf_max_separation_force.GetInt()); //flPushScale;

	//Msg( "PushScale = %f\n", flPushStrength );

	// Check to see if we have enough push strength to make a difference.
	if (flPushStrength < 0.01f)
		return;

	Vector vecPush;
	if (GetAbsVelocity().Length2DSqr() > 0.1f)
	{
		Vector vecVelocity = GetAbsVelocity();
		vecVelocity.z = 0.0f;
		CrossProduct(vecUp, vecVelocity, vecPush);
		VectorNormalize(vecPush);
	}
	else
	{
		// We are not moving, but we're still intersecting.
		QAngle angView = pCmd->viewangles;
		angView.x = 0.0f;
		AngleVectors(angView, NULL, &vecPush, NULL);
	}

	// Move away from the other player/object.
	Vector vecSeparationVelocity;
	if (vecDelta.Dot(vecPush) < 0)
	{
		vecSeparationVelocity = vecPush * flPushStrength;
	}
	else
	{
		vecSeparationVelocity = vecPush * -flPushStrength;
	}

	// Don't allow the max push speed to be greater than the max player speed.
	float flMaxPlayerSpeed = MaxSpeed();
	float flCropFraction = 1.33333333f;

	if ((GetFlags() & FL_DUCKING) && (GetGroundEntity() != NULL))
	{
		flMaxPlayerSpeed *= flCropFraction;
	}

	float flMaxPlayerSpeedSqr = flMaxPlayerSpeed * flMaxPlayerSpeed;

	if (vecSeparationVelocity.LengthSqr() > flMaxPlayerSpeedSqr)
	{
		vecSeparationVelocity.NormalizeInPlace();
		VectorScale(vecSeparationVelocity, flMaxPlayerSpeed, vecSeparationVelocity);
	}

	QAngle vAngles = pCmd->viewangles;
	vAngles.x = 0;
	Vector currentdir;
	Vector rightdir;

	AngleVectors(vAngles, &currentdir, &rightdir, NULL);

	Vector vDirection = vecSeparationVelocity;

	VectorNormalize(vDirection);

	float fwd = currentdir.Dot(vDirection);
	float rt = rightdir.Dot(vDirection);

	float forward = fwd * flPushStrength;
	float side = rt * flPushStrength;

	//Msg( "fwd: %f - rt: %f - forward: %f - side: %f\n", fwd, rt, forward, side );

	pCmd->forwardmove += forward;
	pCmd->sidemove += side;

	// Clamp the move to within legal limits, preserving direction. This is a little
	// complicated because we have different limits for forward, back, and side

	//Msg( "PRECLAMP: forwardmove=%f, sidemove=%f\n", pCmd->forwardmove, pCmd->sidemove );

	float flForwardScale = 1.0f;
	if (pCmd->forwardmove > fabs(cl_forwardspeed.GetFloat()))
	{
		flForwardScale = fabs(cl_forwardspeed.GetFloat()) / pCmd->forwardmove;
	}
	else if (pCmd->forwardmove < -fabs(cl_backspeed.GetFloat()))
	{
		flForwardScale = fabs(cl_backspeed.GetFloat()) / fabs(pCmd->forwardmove);
	}

	float flSideScale = 1.0f;
	if (fabs(pCmd->sidemove) > fabs(cl_sidespeed.GetFloat()))
	{
		flSideScale = fabs(cl_sidespeed.GetFloat()) / fabs(pCmd->sidemove);
	}

	float flScale = min(flForwardScale, flSideScale);
	pCmd->forwardmove *= flScale;
	pCmd->sidemove *= flScale;

	//Msg( "Pforwardmove=%f, sidemove=%f\n", pCmd->forwardmove, pCmd->sidemove );
}

bool C_Laz_Player::CreateMove(float flInputSampleTime, CUserCmd * pCmd)
{
	static QAngle angMoveAngle(0.0f, 0.0f, 0.0f);

	bool bNoTaunt = true;
	if (InTauntCam())
	{
		// show centerprint message 
		pCmd->forwardmove = 0.0f;
		pCmd->sidemove = 0.0f;
		pCmd->upmove = 0.0f;
		//int nOldButtons = pCmd->buttons;
		pCmd->buttons = 0;
		pCmd->weaponselect = 0;

		VectorCopy(angMoveAngle, pCmd->viewangles);
		bNoTaunt = false;
	}
	else
	{
		VectorCopy(pCmd->viewangles, angMoveAngle);
	}

	BaseClass::CreateMove(flInputSampleTime, pCmd);

	Vector2D vMove(pCmd->forwardmove, pCmd->sidemove);
	if (vMove.IsLengthLessThan(HL2_WALK_SPEED))
		AvoidPlayers(pCmd);

	return bNoTaunt;
}

ConVar cl_blobbyshadows("cl_blobbyshadows", "0", FCVAR_CLIENTDLL);
ShadowType_t C_Laz_Player::ShadowCastType(void)
{
	// Removed the GetPercentInvisible - should be taken care off in BindProxy now.
	if (!IsVisible() /*|| GetPercentInvisible() > 0.0f*/ || IsLocalPlayer())
		return SHADOWS_NONE;

	if (IsEffectActive(EF_NODRAW | EF_NOSHADOW))
		return SHADOWS_NONE;

	// If in ragdoll mode.
	if (m_nRenderFX == kRenderFxRagdoll)
		return SHADOWS_NONE;

	C_BasePlayer *pLocalPlayer = GetLocalPlayer();

	// if we're first person spectating this player
	if (pLocalPlayer &&
		pLocalPlayer->GetObserverTarget() == this &&
		pLocalPlayer->GetObserverMode() == OBS_MODE_IN_EYE)
	{
		return SHADOWS_NONE;
	}

	if (cl_blobbyshadows.GetBool())
		return SHADOWS_SIMPLE;

	return SHADOWS_RENDER_TO_TEXTURE_DYNAMIC;
}

float g_flFattenAmt = 4;
void C_Laz_Player::GetShadowRenderBounds(Vector &mins, Vector &maxs, ShadowType_t shadowType)
{
	if (shadowType == SHADOWS_SIMPLE)
	{
		// Don't let the render bounds change when we're using blobby shadows, or else the shadow
		// will pop and stretch.
		mins = CollisionProp()->OBBMins();
		maxs = CollisionProp()->OBBMaxs();
	}
	else
	{
		GetRenderBounds(mins, maxs);

		// We do this because the normal bbox calculations don't take pose params into account, and 
		// the rotation of the guy's upper torso can place his gun a ways out of his bbox, and 
		// the shadow will get cut off as he rotates.
		//
		// Thus, we give it some padding here.
		mins -= Vector(g_flFattenAmt, g_flFattenAmt, 0);
		maxs += Vector(g_flFattenAmt, g_flFattenAmt, 0);
	}
}


void C_Laz_Player::GetRenderBounds(Vector& theMins, Vector& theMaxs)
{
	// TODO POSTSHIP - this hack/fix goes hand-in-hand with a fix in CalcSequenceBoundingBoxes in utils/studiomdl/simplify.cpp.
	// When we enable the fix in CalcSequenceBoundingBoxes, we can get rid of this.
	//
	// What we're doing right here is making sure it only uses the bbox for our lower-body sequences since,
	// with the current animations and the bug in CalcSequenceBoundingBoxes, are WAY bigger than they need to be.
	C_BaseAnimating::GetRenderBounds(theMins, theMaxs);
}


bool C_Laz_Player::GetShadowCastDirection(Vector *pDirection, ShadowType_t shadowType) const
{
	if (shadowType == SHADOWS_SIMPLE)
	{
		// Blobby shadows should sit directly underneath us.
		pDirection->Init(0, 0, -1);
		return true;
	}
	else
	{
		return BaseClass::GetShadowCastDirection(pDirection, shadowType);
	}
}

bool C_Laz_Player::ShouldReceiveProjectedTextures(int flags)
{
	Assert(flags & SHADOW_FLAGS_PROJECTED_TEXTURE_TYPE_MASK);

	if (IsEffectActive(EF_NODRAW))
		return false;

	if (flags & SHADOW_FLAGS_FLASHLIGHT)
	{
		return true;
	}

	return BaseClass::ShouldReceiveProjectedTextures(flags);
}

bool C_Laz_Player::CanSprint(void)
{
	return ((!m_Local.m_bDucked && !m_Local.m_bDucking) && (GetWaterLevel() != 3));
}

void C_Laz_Player::StartSprinting(void)
{
	if (m_HL2Local.m_flSuitPower < 10)
	{
		// Don't sprint unless there's a reasonable
		// amount of suit power.
		CPASAttenuationFilter filter(this);
		filter.UsePredictionRules();
		EmitSound(filter, entindex(), "SynergyPlayer.SprintNoPower");
		return;
	}

	CPASAttenuationFilter filter(this);
	filter.UsePredictionRules();
	EmitSound(filter, entindex(), "SynergyPlayer.SprintStart");

	SetMaxSpeed(HL2_SPRINT_SPEED);
	m_fIsSprinting = true;
}

void C_Laz_Player::StopSprinting(void)
{
	SetMaxSpeed(HL2_NORM_SPEED);
	m_fIsSprinting = false;
}

void C_Laz_Player::HandleSpeedChanges(void)
{
	int buttonsChanged = m_afButtonPressed | m_afButtonReleased;

	if (buttonsChanged & IN_SPEED)
	{
		// The state of the sprint/run button has changed.
		if (IsSuitEquipped())
		{
			if (!(m_afButtonPressed & IN_SPEED) && IsSprinting())
			{
				StopSprinting();
			}
			else if ((m_afButtonPressed & IN_SPEED) && !IsSprinting())
			{
				if (CanSprint())
				{
					StartSprinting();
				}
				else
				{
					// Reset key, so it will be activated post whatever is suppressing it.
					m_nButtons &= ~IN_SPEED;
				}
			}
		}
	}
	else if (buttonsChanged & IN_WALK)
	{
		if (IsSuitEquipped())
		{
			// The state of the WALK button has changed.
			if (IsWalking() && !(m_afButtonPressed & IN_WALK))
			{
				StopWalking();
			}
			else if (!IsWalking() && !IsSprinting() && (m_afButtonPressed & IN_WALK) && !(m_nButtons & IN_DUCK))
			{
				StartWalking();
			}
		}
	}

	if (IsSuitEquipped() && m_fIsWalking && !(m_nButtons & IN_WALK))
		StopWalking();
}

void C_Laz_Player::StartWalking(void)
{
	SetMaxSpeed(HL2_WALK_SPEED);
	m_fIsWalking = true;
}

void C_Laz_Player::StopWalking(void)
{
	SetMaxSpeed(HL2_NORM_SPEED);
	m_fIsWalking = false;
}

void C_Laz_Player::DoAnimationEvents(CStudioHdr* pStudio)
{
	if (InFirstPersonView())
		return;

	BaseClass::DoAnimationEvents(pStudio);
}

int C_Laz_Player::DrawModel(int flags)
{
	// Don't draw in our flashlight's depth texture
	if (IsRenderingMyFlashlight())
		return 0;

	if (CurrentViewID() == VIEW_REFLECTION && !g_pPortalRender->IsRenderingPortal())
		return 0;

	return BaseClass::DrawModel(flags);
}

CStudioHdr* C_Laz_Player::OnNewModel(void)
{
	CStudioHdr* hdr = BaseClass::OnNewModel();

	m_bitLeftArm.ClearAll();
	m_bitRightArm.ClearAll();
	m_bitHair.ClearAll();

	if (hdr)
	{
		m_iHeadBone = LookupBone("ValveBiped.Bip01_Head1");
		if (m_iHeadBone < 0)
			m_iHeadBone = LookupBone("ValveBiped.HC_Body_Bone");

		if (m_iHeadBone >= 0)
		{
			m_bitLeftArm.Set(LookupBone("ValveBiped.Bip01_L_Upperarm"));
			m_bitRightArm.Set(LookupBone("ValveBiped.Bip01_R_Upperarm"));
			m_bitHair.Set(m_iHeadBone);
		}

		for (int i = 0; i < hdr->numbones(); i++)
		{
			int iParent = hdr->boneParent(i);

			if (iParent >= 0)
			{
				if (m_bitLeftArm.IsBitSet(iParent))
					m_bitLeftArm.Set(i);

				if (m_bitRightArm.IsBitSet(iParent))
					m_bitRightArm.Set(i);

				if (m_bitHair.IsBitSet(iParent))
					m_bitHair.Set(i);
			}
		}
	}

	return hdr;
}

void C_Laz_Player::CalculateIKLocks(float currentTime)
{
	if (!m_pIk)
		return;

	int targetCount = m_pIk->m_target.Count();
	if (targetCount == 0)
		return;

	// In TF, we might be attaching a player's view to a walking model that's using IK. If we are, it can
	// get in here during the view setup code, and it's not normally supposed to be able to access the spatial
	// partition that early in the rendering loop. So we allow access right here for that special case.
	SpatialPartitionListMask_t curSuppressed = partition->GetSuppressedLists();
	partition->SuppressLists(PARTITION_ALL_CLIENT_EDICTS, false);
	CBaseEntity::PushEnableAbsRecomputations(false);

	Ray_t ray;
	CTraceFilterNoNPCsOrPlayer traceFilter(this, GetCollisionGroup());

	// FIXME: trace based on gravity or trace based on angles?
	Vector up;
	AngleVectors(GetRenderAngles(), NULL, NULL, &up);

	// FIXME: check number of slots?
	float minHeight = FLT_MAX;
	float maxHeight = -FLT_MAX;

	for (int i = 0; i < targetCount; i++)
	{
		trace_t trace;
		CIKTarget* pTarget = &m_pIk->m_target[i];

		if (!pTarget->IsActive())
			continue;

		switch (pTarget->type)
		{
		case IK_GROUND:
		{
			Vector estGround;
			Vector p1, p2;

			// adjust ground to original ground position
			estGround = (pTarget->est.pos - GetRenderOrigin());
			estGround = estGround - (estGround * up) * up;
			estGround = GetAbsOrigin() + estGround + pTarget->est.floor * up;

			VectorMA(estGround, pTarget->est.height, up, p1);
			VectorMA(estGround, -pTarget->est.height, up, p2);

			float r = MAX(pTarget->est.radius, 1.f);

			// don't IK to other characters
			ray.Init(p1, p2, Vector(-r, -r, 0), Vector(r, r, r * 2));
			enginetrace->TraceRay(ray, PhysicsSolidMaskForEntity(), &traceFilter, &trace);

			if (trace.m_pEnt != NULL && trace.m_pEnt->GetMoveType() == MOVETYPE_PUSH)
			{
				pTarget->SetOwner(trace.m_pEnt->entindex(), trace.m_pEnt->GetAbsOrigin(), trace.m_pEnt->GetAbsAngles());
			}
			else
			{
				pTarget->ClearOwner();
			}

			if (trace.startsolid)
			{
				// trace from back towards hip
				Vector tmp = estGround - pTarget->trace.closest;
				tmp.NormalizeInPlace();
				ray.Init(estGround - tmp * pTarget->est.height, estGround, Vector(-r, -r, 0), Vector(r, r, 1));

				// debugoverlay->AddLineOverlay( ray.m_Start, ray.m_Start + ray.m_Delta, 255, 0, 0, 0, 0 );

				enginetrace->TraceRay(ray, MASK_SOLID, &traceFilter, &trace);

				if (!trace.startsolid)
				{
					p1 = trace.endpos;
					VectorMA(p1, -pTarget->est.height, up, p2);
					ray.Init(p1, p2, Vector(-r, -r, 0), Vector(r, r, 1));

					enginetrace->TraceRay(ray, MASK_SOLID, &traceFilter, &trace);
				}

				// debugoverlay->AddLineOverlay( ray.m_Start, ray.m_Start + ray.m_Delta, 0, 255, 0, 0, 0 );
			}


			if (!trace.startsolid)
			{
				if (trace.DidHitWorld())
				{
					// clamp normal to 33 degrees
					const float limit = 0.832;
					float dot = DotProduct(trace.plane.normal, up);
					if (dot < limit)
					{
						Assert(dot >= 0);
						// subtract out up component
						Vector diff = trace.plane.normal - up * dot;
						// scale remainder such that it and the up vector are a unit vector
						float d = sqrt((1 - limit * limit) / DotProduct(diff, diff));
						trace.plane.normal = up * limit + d * diff;
					}
					// FIXME: this is wrong with respect to contact position and actual ankle offset
					pTarget->SetPosWithNormalOffset(trace.endpos, trace.plane.normal);
					pTarget->SetNormal(trace.plane.normal);
					pTarget->SetOnWorld(true);

					// only do this on forward tracking or commited IK ground rules
					if (pTarget->est.release < 0.1)
					{
						// keep track of ground height
						float offset = DotProduct(pTarget->est.pos, up);
						if (minHeight > offset)
							minHeight = offset;

						if (maxHeight < offset)
							maxHeight = offset;
					}
					// FIXME: if we don't drop legs, running down hills looks horrible
					/*
					if (DotProduct( pTarget->est.pos, up ) < DotProduct( estGround, up ))
					{
						pTarget->est.pos = estGround;
					}
					*/
				}
				else if (trace.DidHitNonWorldEntity())
				{
					pTarget->SetPos(trace.endpos);
					pTarget->SetAngles(GetRenderAngles());

					// only do this on forward tracking or commited IK ground rules
					if (pTarget->est.release < 0.1)
					{
						float offset = DotProduct(pTarget->est.pos, up);
						if (minHeight > offset)
							minHeight = offset;

						if (maxHeight < offset)
							maxHeight = offset;
					}
					// FIXME: if we don't drop legs, running down hills looks horrible
					/*
					if (DotProduct( pTarget->est.pos, up ) < DotProduct( estGround, up ))
					{
						pTarget->est.pos = estGround;
					}
					*/
				}
				else
				{
					pTarget->IKFailed();
				}
			}
			else
			{
				if (!trace.DidHitWorld())
				{
					pTarget->IKFailed();
				}
				else
				{
					pTarget->SetPos(trace.endpos);
					pTarget->SetAngles(GetRenderAngles());
					pTarget->SetOnWorld(true);
				}
			}

			/*
			debugoverlay->AddTextOverlay( p1, i, 0, "%d %.1f %.1f %.1f ", i,
				pTarget->latched.deltaPos.x, pTarget->latched.deltaPos.y, pTarget->latched.deltaPos.z );
			debugoverlay->AddBoxOverlay( pTarget->est.pos, Vector( -r, -r, -1 ), Vector( r, r, 1), QAngle( 0, 0, 0 ), 255, 0, 0, 0, 0 );
			*/
			// debugoverlay->AddBoxOverlay( pTarget->latched.pos, Vector( -2, -2, 2 ), Vector( 2, 2, 6), QAngle( 0, 0, 0 ), 0, 255, 0, 0, 0 );
		}
		break;

		case IK_ATTACHMENT:
		{
			bool bDoNormal = true;
			if (IsInAVehicle())
			{
				C_BaseEntity* pVehicle = GetVehicle()->GetVehicleEnt();
				C_BaseAnimating* pAnim = pVehicle->GetBaseAnimating();
				if (pAnim)
				{
					int iAttachment = pAnim->LookupAttachment(pTarget->offset.pAttachmentName);
					if (iAttachment > 0)
					{
						Vector origin;
						QAngle angles;
						pAnim->GetAttachment(iAttachment, origin, angles);

						pTarget->SetPos(origin);
						pTarget->SetAngles(angles);

						bDoNormal = false;
					}
				}
			}

			if (bDoNormal)
			{
				C_BaseEntity* pEntity = NULL;
				float flDist = pTarget->est.radius;

				// FIXME: make entity finding sticky!
				// FIXME: what should the radius check be?
				for (CEntitySphereQuery sphere(pTarget->est.pos, 64); (pEntity = sphere.GetCurrentEntity()) != NULL; sphere.NextEntity())
				{
					C_BaseAnimating* pAnim = pEntity->GetBaseAnimating();
					if (!pAnim)
						continue;

					int iAttachment = pAnim->LookupAttachment(pTarget->offset.pAttachmentName);
					if (iAttachment <= 0)
						continue;

					Vector origin;
					QAngle angles;
					pAnim->GetAttachment(iAttachment, origin, angles);

					// debugoverlay->AddBoxOverlay( origin, Vector( -1, -1, -1 ), Vector( 1, 1, 1 ), QAngle( 0, 0, 0 ), 255, 0, 0, 0, 0 );

					float d = (pTarget->est.pos - origin).Length();

					if (d >= flDist)
						continue;

					flDist = d;
					pTarget->SetPos(origin);
					pTarget->SetAngles(angles);
					// debugoverlay->AddBoxOverlay( pTarget->est.pos, Vector( -pTarget->est.radius, -pTarget->est.radius, -pTarget->est.radius ), Vector( pTarget->est.radius, pTarget->est.radius, pTarget->est.radius), QAngle( 0, 0, 0 ), 0, 255, 0, 0, 0 );
				}

				if (flDist >= pTarget->est.radius)
				{
					// debugoverlay->AddBoxOverlay( pTarget->est.pos, Vector( -pTarget->est.radius, -pTarget->est.radius, -pTarget->est.radius ), Vector( pTarget->est.radius, pTarget->est.radius, pTarget->est.radius), QAngle( 0, 0, 0 ), 0, 0, 255, 0, 0 );
					// no solution, disable ik rule
					pTarget->IKFailed();
				}
			}
		}
		break;
		}
	}

	//#if defined( HL2_CLIENT_DLL )
	if (gpGlobals->maxClients == 1 && minHeight < FLT_MAX)
	{
		input->AddIKGroundContactInfo(entindex(), minHeight, maxHeight);
	}
	//#endif

	CBaseEntity::PopEnableAbsRecomputations();
	partition->SuppressLists(curSuppressed, true);
}

//-----------------------------------------------------------------------------
// Purpose: Creates, destroys, and updates the flashlight effect as needed.
//-----------------------------------------------------------------------------
void C_Laz_Player::UpdateFlashlight()
{
	// The dim light is the flashlight.
	if (IsEffectActive(EF_DIMLIGHT))
	{
		if (!m_pFlashlight)
		{
			// Turned on the headlight; create it.
#ifdef DEFERRED
			m_pFlashlight = new CFlashlightEffectDeferred(index);
#else
			m_pFlashlight = new CFlashlightEffect(index, m_nFlashlightType == FLASHLIGHT_NVG);
#endif

			if (!m_pFlashlight)
				return;

			m_pFlashlight->TurnOn();
		}

		Vector vecForward, vecRight, vecUp;
		EyeVectors(&vecForward, &vecRight, &vecUp);

		// Update the light with the new position and direction.		
		m_pFlashlight->UpdateLight(EyePosition(), vecForward, vecRight, vecUp, FLASHLIGHT_DISTANCE);
	}
	else if (m_pFlashlight)
	{
		// Turned off the flashlight; delete it.
		delete m_pFlashlight;
		m_pFlashlight = NULL;
	}
}

bool C_Laz_Player::IsInReload()
{
	if (!GetActiveWeapon())
		return false;

	Activity actWeap = GetViewModel()->GetSequenceActivity(GetViewModel()->GetSequence());
	if (actWeap >= ACT_VM_RELOAD && actWeap <= ACT_VM_RELOAD_FINISH)
		return true;

	if (actWeap == ACT_VM_RELOAD_EMPTY)
		return true;

	return false;
}

int C_Laz_Player::GetHideBits()
{
	int iBits = 0;

	if (IsInAVehicle() && !UsingStandardWeaponsInVehicle())
		return 0;

	if (GetActiveWeapon())
	{
		if (GetActiveWeapon()->GetWpnData().bBodyHideArmL)
			iBits |= HIDEARM_LEFT;

		if (GetActiveWeapon()->GetWpnData().bBodyHideArmR)
			iBits |= HIDEARM_RIGHT;
	}

	if (IsInReload())
		iBits |= (HIDEARM_LEFT | HIDEARM_RIGHT);

	return iBits;
}

ConVar hair_dist_scale("cl_legs_hair_scale", "2.5", FCVAR_NONE, "Scale added to hair bones.");

//-----------------------------------------------------------------------------
// Purpose: In meathook mode, fix the bone transforms to hang the user's own
//			avatar under the camera.
//-----------------------------------------------------------------------------
void C_Laz_Player::BuildFirstPersonMeathookTransformations(CStudioHdr* hdr, Vector* pos, Quaternion q[], const matrix3x4_t& cameraTransform, int boneMask, CBoneBitList& boneComputed, const char* pchHeadBoneName)
{
	// Handle meathook mode. If we aren't rendering, just use last frame's transforms
	if (!InFirstPersonView())
		return;

	// If we're in third-person view, don't do anything special.
	// If we're in first-person view rendering the main view and using the viewmodel, we shouldn't have even got here!
	// If we're in first-person view rendering the main view(s), meathook and headless.
	// If we're in first-person view rendering shadowbuffers/reflections, don't do anything special either (we could do meathook but with a head?)
	if (m_hRagdoll.Get())
	{
		// We're re-animating specifically to set up the ragdoll.
		// Meathook can push the player through the floor, which makes the ragdoll fall through the world, which is no good.
		// So do nothing.
		return;
	}

	if (g_pPortalRender->IsRenderingPortal() || m_bForceNormalBoneSetup)
	{
		return;
	}

	int iView = CurrentViewID();
	if (iView != VIEW_MAIN && iView != VIEW_REFRACTION && iView != VIEW_WATER_INTERSECTION)
	{
		return;
	}

	// If we aren't drawing the player anyway, don't mess with the bones. This can happen in Portal.
	if ((IsLocalPlayer() && ShouldDrawThisPlayer()) || !cl_legs_enable.GetBool())
	{
		if (GetModelPtr() && m_iHeadBone >= 0 && GetModelPtr()->numbones() > m_iHeadBone)
		{
			mstudiobone_t* pBone = GetModelPtr()->pBone(m_iHeadBone);
			pchHeadBoneName = pBone->pszName();
		}

		BaseClass::BuildFirstPersonMeathookTransformations(hdr, pos, q, cameraTransform, boneMask, boneComputed, pchHeadBoneName);
		return;
	}

	m_BoneAccessor.SetWritableBones(BONE_USED_BY_ANYTHING);

	int iHead = m_iHeadBone;
	if (iHead == -1)
	{
		return;
	}

	matrix3x4_t& mHeadTransform = GetBoneForWrite(iHead);

	// "up" on the head bone is along the negative Y axis - not sure why.
	//Vector vHeadTransformUp ( -mHeadTransform[0][1], -mHeadTransform[1][1], -mHeadTransform[2][1] );
	//Vector vHeadTransformFwd ( mHeadTransform[0][1], mHeadTransform[1][1], mHeadTransform[2][1] );
	Vector vHeadTransformTranslation(mHeadTransform[0][3], mHeadTransform[1][3], mHeadTransform[2][3]);


	// Find out where the player's head (driven by the HMD) is in the world.
	// We can't move this with animations or effects without causing nausea, so we need to move
	// the whole body so that the animated head is in the right place to match the player-controlled head.
	//Vector vHeadUp;
	Vector vRealPivotPoint(0);


	// figure out where to put the body from the aim angles
	Vector vForward, vRight, vUp;
	AngleVectors(MainViewAngles(), &vForward, &vRight, &vUp);

	vRealPivotPoint = MainViewOrigin() - (vUp * cl_meathook_neck_pivot_ingame_up.GetFloat()) - (vForward * cl_meathook_neck_pivot_ingame_fwd.GetFloat());


	if (m_Local.m_bDucking && GetGroundEntity())
		vRealPivotPoint.z += 21;

	Vector vDeltaToAdd = vRealPivotPoint - vHeadTransformTranslation;

	Vector vAdd = vUp * -128;

	if (!IsInAVehicle())
	{
		// Now add this offset to the entire skeleton.
		for (int i = 0; i < hdr->numbones(); i++)
		{
			// Only update bones reference by the bone mask.
			if (!(hdr->boneFlags(i) & boneMask))
			{
				continue;
			}
			matrix3x4_t& bone = GetBoneForWrite(i);
			Vector vBonePos;
			MatrixGetTranslation(bone, vBonePos);
			vBonePos += vDeltaToAdd;
			MatrixSetTranslation(vBonePos, bone);
		}
	}

	Vector vHeadAdd;
	VectorRotate(Vector(-128, 128, 0), mHeadTransform, vHeadAdd);

	// Then scale the head to zero, but leave its position - forms a "neck stub".
	// This prevents us rendering junk all over the screen, e.g. inside of mouth, etc.
	MatrixScaleByZero(mHeadTransform);
	if (cl_legs_enable.GetInt() < 2)
	{
		for (int iBone = 0; iBone < hdr->numbones(); iBone++)
		{
			if (m_bitHair.IsBitSet(iBone))
			{
				Vector vBonePos;
				MatrixGetTranslation(GetBoneForWrite(iBone), vBonePos);
				vBonePos += vHeadAdd;
				MatrixSetTranslation(vBonePos, GetBoneForWrite(iBone));
			}
		}
	}
	else
	{
		Vector vHeadPos;
		MatrixGetTranslation(mHeadTransform, vHeadPos);
		for (int iBone = 0; iBone < hdr->numbones(); iBone++)
		{
			if (iBone != iHead && m_bitHair.IsBitSet(iBone))
			{
				Vector vBonePos, vHeadDelta;
				matrix3x4_t& mBoneTransform = GetBoneForWrite(iBone);
				MatrixGetTranslation(mBoneTransform, vBonePos);

				vHeadDelta = vBonePos - vHeadPos;
				vBonePos += vHeadDelta * (hair_dist_scale.GetFloat() - 1.0f);
				if (!IsInAVehicle())
					vBonePos += (vUp * cl_meathook_neck_pivot_ingame_up.GetFloat()) - (vForward * cl_meathook_neck_pivot_ingame_fwd.GetFloat());

				MatrixSetTranslation(vBonePos, mBoneTransform);
				MatrixScaleBy(hair_dist_scale.GetFloat(), mBoneTransform);
			}
		}
	}


	bool bHideArmL = GetHideBits() & HIDEARM_LEFT;
	bool bHideArmR = GetHideBits() & HIDEARM_RIGHT;

	if (bHideArmL)
	{
		/*int iBone = LookupBone("ValveBiped.Bip01_L_Upperarm");
		matrix3x4_t bone = GetBone(iBone);

		Vector vAdd;
		VectorRotate(Vector(0, 0, -128), bone, vAdd);*/

		for (int iBone = 0; iBone < hdr->numbones(); iBone++)
		{
			if (m_bitLeftArm.IsBitSet(iBone))
			{
				matrix3x4_t& bone = GetBoneForWrite(iBone);

				MatrixScaleByZero(bone);

				Vector vBonePos;
				MatrixGetTranslation(bone, vBonePos);
				vBonePos += vAdd;
				MatrixSetTranslation(vBonePos, bone);
			}
		}
	}

	if (bHideArmR)
	{
		/*int iBone = LookupBone("ValveBiped.Bip01_R_Upperarm");
		matrix3x4_t bone = GetBone(iBone);

		Vector vAdd;
		VectorRotate(Vector(0, 0, 128), bone, vAdd);*/

		for (int iBone = 0; iBone < hdr->numbones(); iBone++)
		{
			if (m_bitRightArm.IsBitSet(iBone))
			{
				matrix3x4_t& bone = GetBoneForWrite(iBone);

				MatrixScaleByZero(bone);

				Vector vBonePos;
				MatrixGetTranslation(bone, vBonePos);
				vBonePos += vAdd;
				MatrixSetTranslation(vBonePos, bone);
			}
		}
	}
}