#ifndef LAZUUL_GAMERULES_H
#define LAZUUL_GAMERULES_H
#ifdef _WIN32
#pragma once
#endif

#include "teamplayroundbased_gamerules.h"
#include "hl2_gamerules.h"

#ifdef CLIENT_DLL
#define CLazuul C_Lazuul
#define CLazuulProxy C_LazuulProxy
#endif // CLIENT_DLL

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
	virtual ~CLazuul() {}

	// Damage Query Overrides.
	virtual bool			Damage_IsTimeBased(int iDmgType);
	// TEMP:
	virtual int				Damage_GetTimeBased(void);

	virtual bool			ShouldCollide(int collisionGroup0, int collisionGroup1);
	virtual bool			ShouldUseRobustRadiusDamage(CBaseEntity* pEntity);
#ifndef CLIENT_DLL
	virtual bool			ShouldAutoAim(CBasePlayer* pPlayer, edict_t* target);
	virtual float			GetAutoAimScale(CBasePlayer* pPlayer);
	virtual float			GetAmmoQuantityScale(int iAmmoIndex);
	virtual void			LevelInitPreEntity();
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
	void	SetAllowedModes(bool bModes[])
	{
		for (int i = 0; i < LAZ_GM_COUNT; i++)
		{
			m_bitAllowedModes.Set(i, bModes[i]);
		}
	}	

	virtual bool			ShouldBurningPropsEmitLight();

	bool AllowDamage(CBaseEntity* pVictim, const CTakeDamageInfo& info);

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
