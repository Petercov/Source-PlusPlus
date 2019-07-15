#ifndef LAZUUL_GAMERULES_H
#define LAZUUL_GAMERULES_H
#ifdef _WIN32
#pragma once
#endif

#include "teamplayroundbased_gamerules.h"
#include "hl2_gamerules.h"
#include "tf_shareddefs.h"

#ifdef CLIENT_DLL
#define CLazuul C_Lazuul
#define CLazuulProxy C_LazuulProxy
#endif // CLIENT_DLL

//enum
//{
//	TEAM_COMBINE = FIRST_GAME_TEAM,
//	TEAM_REBELS,
//};

enum {
	LAZ_GM_SINGLEPLAYER = -1,
	LAZ_GM_DEATHMATCH = 0,
	LAZ_GM_COOP,
	LAZ_GM_VERSUS,

	LAZ_GM_COUNT
};

class CLazuulProxy : public CTeamplayRoundBasedRulesProxy
{
public:
	DECLARE_CLASS(CLazuulProxy, CTeamplayRoundBasedRulesProxy);
	DECLARE_NETWORKCLASS();

#ifdef GAME_DLL
	DECLARE_DATADESC();
	virtual void Activate();

	bool	m_bModeAllowed[LAZ_GM_COUNT];
#endif
};

class CLazuul : public CTeamplayRoundBasedRules, public IHalfLife2
{
	DECLARE_CLASS(CLazuul, CTeamplayRoundBasedRules)
	DECLARE_NETWORKCLASS_NOBASE()
public:
	CLazuul();
	virtual ~CLazuul();

	// Damage Query Overrides.
	virtual bool			Damage_IsTimeBased(int iDmgType);
	// TEMP:
	virtual int				Damage_GetTimeBased(void);

	int				GetFarthestOwnedControlPoint(int iTeam, bool bWithSpawnpoints);
	virtual bool	TeamMayCapturePoint(int iTeam, int iPointIndex);
	virtual bool	PlayerMayCapturePoint(CBasePlayer *pPlayer, int iPointIndex, char *pszReason = NULL, int iMaxReasonLength = 0);
	virtual bool	PlayerMayBlockPoint(CBasePlayer *pPlayer, int iPointIndex, char *pszReason = NULL, int iMaxReasonLength = 0);

	virtual bool			ShouldCollide(int collisionGroup0, int collisionGroup1);
	virtual bool			ShouldUseRobustRadiusDamage(CBaseEntity* pEntity);

	virtual bool	IsConnectedUserInfoChangeAllowed(CBasePlayer *pPlayer) { return true; }
#ifndef CLIENT_DLL
	virtual bool			ShouldAutoAim(CBasePlayer* pPlayer, edict_t* target);
	virtual float			GetAutoAimScale(CBasePlayer* pPlayer);
	virtual float			GetAmmoQuantityScale(int iAmmoIndex);
	virtual void			LevelInitPreEntity();

	virtual int PlayerRelationship(CBaseEntity *pPlayer, CBaseEntity *pTarget);

	virtual void PlayerSpawn(CBasePlayer* pPlayer);
	virtual bool			ClientCommand(CBaseEntity *pEdict, const CCommand &args);

	// Game Achievements (server version)
	virtual void MarkAchievement(IRecipientFilter& filter, char const *pchAchievementName);

	// NPCs
	virtual bool FAllowNPCs(void);

	// Weapon spawn/respawn control
	virtual int WeaponShouldRespawn(CBaseCombatWeapon* pWeapon);
	//virtual float FlWeaponRespawnTime(CBaseCombatWeapon* pWeapon);
	//virtual float FlWeaponTryRespawn(CBaseCombatWeapon* pWeapon);
	//virtual Vector VecWeaponRespawnSpot(CBaseCombatWeapon* pWeapon);

	// Item spawn/respawn control
	virtual int ItemShouldRespawn(CItem* pItem);
	//virtual float FlItemRespawnTime(CItem* pItem);
	//virtual Vector VecItemRespawnSpot(CItem* pItem);
	//virtual QAngle VecItemRespawnAngles(CItem* pItem);
#endif

	bool	MegaPhyscannonActive(void) { return m_bMegaPhysgun; }

	bool	IsMultiplayer() { return (gpGlobals->maxClients > 1); }
	bool	IsCoop()
	{
		int iMode = GetGameMode();
		return (iMode == LAZ_GM_COOP || iMode == LAZ_GM_VERSUS);
	}
	bool	IsDeathmatch()
	{
		int iMode = GetGameMode();
		return (iMode == LAZ_GM_DEATHMATCH || iMode == LAZ_GM_VERSUS);
	}

	int		GetGameMode()
	{
		if (!IsMultiplayer())
			return LAZ_GM_SINGLEPLAYER;
		return m_nGameMode;
	}

	int		GetProtaganistTeam()
	{
		return TEAM_REBELS;
	}

#ifndef CLIENT_DLL
	virtual float			GetAmmoDamage(CBaseEntity* pAttacker, CBaseEntity* pVictim, int nAmmoType);

	virtual bool IsAlyxInDarknessMode();
	virtual void			Think(void);

	virtual void			InitDefaultAIRelationships(void);
	virtual const char* AIClassText(int classType);
	virtual const char* GetGameDescription(void);

	// Override this to prevent removal of game specific entities that need to persist
	virtual bool	RoundCleanupShouldIgnore(CBaseEntity* pEnt);
	virtual bool	ShouldCreateEntity(const char* pszClassName);

	// Sets up g_pPlayerResource.
	virtual void CreateStandardEntities();

	// Can only set on server
	void	SetGameMode(int iMode);
	void	SetAllowedModes(bool bModes[]);

	virtual bool			ShouldBurningPropsEmitLight();

	bool AllowDamage(CBaseEntity* pVictim, const CTakeDamageInfo& info);

	virtual void PlayerKilled(CBasePlayer* pVictim, const CTakeDamageInfo& info);
	virtual void NPCKilled(CAI_BaseNPC* pVictim, const CTakeDamageInfo& info);
	virtual void DeathNotice(CBasePlayer* pVictim, const CTakeDamageInfo& info);
	virtual void DeathNotice(CAI_BaseNPC* pVictim, const CTakeDamageInfo& info);
	virtual CBasePlayer* GetDeathScorer(CBaseEntity* pKiller, CBaseEntity* pInflictor, CBaseEntity* pVictim);

	const char* GetKillingWeaponName(const CTakeDamageInfo& info, CBaseEntity* pVictim, int& iWeaponID);
	CBaseEntity* GetAssister(CBaseEntity* pVictim, CBaseEntity* pKiller, CBaseEntity* pInflictor);
	CBaseEntity* GetRecentDamager(CBaseEntity* pVictim, int iDamager, float flMaxElapsed);

	bool	NPC_ShouldDropGrenade(CBasePlayer* pRecipient);
	bool	NPC_ShouldDropHealth(CBasePlayer* pRecipient);
	void	NPC_DroppedHealth(void);
	void	NPC_DroppedGrenade(void);
protected:
	void AdjustPlayerDamageTaken(CTakeDamageInfo* pInfo);
	float AdjustPlayerDamageInflicted(float damage);
private:
	CBitVec<LAZ_GM_COUNT> m_bitAllowedModes;

	float	m_flLastHealthDropTime;
	float	m_flLastGrenadeDropTime;
#endif


private:
	// Rules change for the mega physgun
	CNetworkVar(bool, m_bMegaPhysgun);
	CNetworkVar(int, m_nGameMode);
};

inline CLazuul* LazuulRules()
{
	return assert_cast<CLazuul*> (g_pGameRules);
}

#endif
