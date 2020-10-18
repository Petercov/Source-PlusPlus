//========= Copyright © 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=============================================================================//


#include "cbase.h"
#include "soundemittersystem/isoundemittersystembase.h"
#include "ai_responsesystem.h"
#include "igamesystem.h"
#include "ai_criteria.h"
#include <keyvalues.h>
#include "filesystem.h"
#include "utldict.h"
#ifdef GAME_DLL
#include "ai_speech.h"
#endif
#include "tier0/icommandline.h"
#include <ctype.h>
#include "isaverestore.h"
#include "utlbuffer.h"
#include "stringpool.h"
#include "fmtstr.h"
#include "multiplay_gamerules.h"
#include "characterset.h"
#include "responserules/response_host_interface.h"
#include "../../responserules/runtime/response_types_internal.h"

#include "scenefilecache/ISceneFileCache.h"
#include "tier1/generichash.h"

#ifdef GAME_DLL
#include "sceneentity.h"
#endif

#include "networkstringtabledefs.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

using namespace ResponseRules;

extern ConVar rr_debugresponses; // ( "rr_debugresponses", "0", FCVAR_NONE, "Show verbose matching output (1 for simple, 2 for rule scoring, 3 for noisy). If set to 4, it will only show response success/failure for npc_selected NPCs." );
extern ConVar rr_debugrule; // ( "rr_debugrule", "", FCVAR_NONE, "If set to the name of the rule, that rule's score will be shown whenever a concept is passed into the response rules system.");
extern ConVar rr_dumpresponses; // ( "rr_dumpresponses", "0", FCVAR_NONE, "Dump all response_rules.txt and rules (requires restart)" );
extern ConVar rr_debugresponseconcept; // ( "rr_debugresponseconcept", "", FCVAR_NONE, "If set, rr_debugresponses will print only responses testing for the specified concept" );

extern ISceneFileCache *scenefilecache;
extern INetworkStringTable *g_pStringTableClientSideChoreoScenes;

static characterset_t	g_BreakSetIncludingColons;

// Simple class to initialize breakset
class CBreakInit
{
public:
	CBreakInit()
	{
		CharacterSetBuild( &g_BreakSetIncludingColons, "{}()':" );
	}
} g_BreakInit;

inline char rr_tolower( char c )
{
	if ( c >= 'A' && c <= 'Z' )
		return c - 'A' + 'a';
	return c;
}
// BUG BUG:  Note that this function doesn't check for data overruns!!!
// Also, this function lowercases the token as it parses!!!
inline const char *RR_Parse(const char *data, char *token )
{
	unsigned char    c;
	int             len;
	characterset_t	*breaks = &g_BreakSetIncludingColons;
	len = 0;
	token[0] = 0;

	if (!data)
		return NULL;

	// skip whitespace
skipwhite:
	while ( (c = *data) <= ' ')
	{
		if (c == 0)
			return NULL;                    // end of file;
		data++;
	}

	// skip // comments
	if (c=='/' && data[1] == '/')
	{
		while (*data && *data != '\n')
			data++;
		goto skipwhite;
	}


	// handle quoted strings specially
	if (c == '\"')
	{
		data++;
		while (1)
		{
			c = rr_tolower( *data++ );
			if (c=='\"' || !c)
			{
				token[len] = 0;
				return data;
			}
			token[len] = c;
			len++;
		}
	}

	// parse single characters
	if ( IN_CHARACTERSET( *breaks, c ) )
	{
		token[len] = c;
		len++;
		token[len] = 0;
		return data+1;
	}

	// parse a regular word
	do
	{
		token[len] = rr_tolower( c );
		data++;
		len++;
		c = rr_tolower( *data );
		if ( IN_CHARACTERSET( *breaks, c ) )
			break;
	} while (c>32);

	token[len] = 0;
	return data;
}

namespace ResponseRules
{
	extern const char *ResponseCopyString( const char *in );
}

// Host functions required by the ResponseRules::IEngineEmulator interface
class CResponseRulesToEngineInterface : public ResponseRules::IEngineEmulator
{
	static CUniformRandomStream rr_stream;

	/// Given an input text buffer data pointer, parses a single token into the variable token and returns the new
	///  reading position
	virtual const char			*ParseFile( const char *data, char *token, int maxlen ) 
	{
		NOTE_UNUSED( maxlen );
		return RR_Parse( data, token );
	}

	/// Return a pointer to an IFileSystem we can use to read and process scripts.
	virtual IFileSystem *GetFilesystem() 
	{
		return filesystem;
	}

	/// Return a pointer to an instance of an IUniformRandomStream
	virtual IUniformRandomStream *GetRandomStream() 
	{
		return &rr_stream;
	}

	/// Return a pointer to a tier0 ICommandLine
	virtual ICommandLine *GetCommandLine() 
	{
		return CommandLine();
	}

	/// Emulates the server's UTIL_LoadFileForMe
	virtual byte *LoadFileForMe( const char *filename, int *pLength )
	{
		return UTIL_LoadFileForMe( filename, pLength );
	}

	/// Emulates the server's UTIL_FreeFile
	virtual void  FreeFile( byte *buffer ) 
	{
		return UTIL_FreeFile( buffer );
	}

	virtual IScriptVM* GetScriptVM()
	{
		return g_pScriptVM;
	}

	virtual bool VScriptRunScript(const char* pszScriptName, HSCRIPT hScope, bool bWarnMissing = false)
	{
		return ::VScriptRunScript(pszScriptName, hScope, bWarnMissing);
	}
};

CUniformRandomStream CResponseRulesToEngineInterface::rr_stream;

CResponseRulesToEngineInterface g_ResponseRulesEngineWrapper;
IEngineEmulator *IEngineEmulator::s_pSingleton = &g_ResponseRulesEngineWrapper;


BEGIN_SIMPLE_DATADESC( ParserResponse )
	// DEFINE_FIELD( type, FIELD_INTEGER ),
	// DEFINE_ARRAY( value, FIELD_CHARACTER ),
	// DEFINE_FIELD( weight, FIELD_FLOAT ),
	DEFINE_FIELD( depletioncount, FIELD_CHARACTER ),
	// DEFINE_FIELD( first, FIELD_BOOLEAN ),
	// DEFINE_FIELD( last, FIELD_BOOLEAN ),
END_DATADESC()


BEGIN_SIMPLE_DATADESC( ResponseGroup )
	// DEFINE_FIELD( group, FIELD_UTLVECTOR ),
	// DEFINE_FIELD( rp, FIELD_EMBEDDED ),
	// DEFINE_FIELD( m_bDepleteBeforeRepeat, FIELD_BOOLEAN ),
	DEFINE_FIELD( m_nDepletionCount, FIELD_CHARACTER ),
	// DEFINE_FIELD( m_bHasFirst, FIELD_BOOLEAN ),
	// DEFINE_FIELD( m_bHasLast, FIELD_BOOLEAN ),
	// DEFINE_FIELD( m_bSequential, FIELD_BOOLEAN ),
	// DEFINE_FIELD( m_bNoRepeat, FIELD_BOOLEAN ),
	DEFINE_FIELD( m_bEnabled, FIELD_BOOLEAN ),
	DEFINE_FIELD( m_nCurrentIndex, FIELD_CHARACTER ),
END_DATADESC()


/// Add some game-specific code to the basic response system
/// (eg, the scene precacher, which requires the client and server
///  to work)

class CGameResponseSystem : public CResponseSystem
{
public:
	CGameResponseSystem();

	virtual void Precache();
	virtual void PrecacheResponses( bool bEnable )
	{
		m_bPrecache = bEnable;
	}
	bool		ShouldPrecache()	{ return m_bPrecache; }

protected:
	bool		m_bPrecache;	
};


//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
CGameResponseSystem::CGameResponseSystem() : m_bPrecache(true)
{};

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------

#if 0
class CScenePrecacheSystem : public CAutoGameSystem
{
public:
	CScenePrecacheSystem() : CAutoGameSystem( "CScenePrecacheSystem" ), m_RepeatCounts( 0, 0, DefLessFunc( int ) )
	{
	}

	// Level init, shutdown
	virtual void LevelShutdownPreEntity()
	{
		m_RepeatCounts.Purge();
	}

	bool ShouldPrecache( char const *pszScene )
	{
		int hash = HashStringCaselessConventional( pszScene );

		int slot = m_RepeatCounts.Find( hash );
		if ( slot != m_RepeatCounts.InvalidIndex() )
		{
			m_RepeatCounts[ slot ]++;
			return false;
		}

		m_RepeatCounts.Insert( hash, 0 );
		return true;
	}

private:

	CUtlMap< int, int > m_RepeatCounts;
};

static CScenePrecacheSystem g_ScenePrecacheSystem;
//-----------------------------------------------------------------------------
// Purpose: Used for precaching instanced scenes
// Input  : *pszScene - 
//-----------------------------------------------------------------------------
void PrecacheInstancedScene( char const *pszScene )
{
	static int nMakingReslists = -1;

	if ( !g_ScenePrecacheSystem.ShouldPrecache( pszScene ) )
		return;

	if ( nMakingReslists == -1 )
	{
		nMakingReslists = CommandLine()->FindParm( "-makereslists" ) > 0 ? 1 : 0;
	}

	if ( nMakingReslists == 1 )
	{
		// Just stat the file to add to reslist
		g_pFullFileSystem->Size( pszScene );
	}

	// verify existence, cache is pre-populated, should be there
	SceneCachedData_t sceneData;
	if ( !scenefilecache->GetSceneCachedData( pszScene, &sceneData ) )
	{
		// Scenes are sloppy and don't always exist.
		// A scene that is not in the pre-built cache image, but on disk, is a true error.
		if ( IsX360() && ( g_pFullFileSystem->GetDVDMode() != DVDMODE_STRICT ) && g_pFullFileSystem->FileExists( pszScene, "GAME" ) )
		{
			Warning( "PrecacheInstancedScene: Missing scene '%s' from scene image cache.\nRebuild scene image cache!\n", pszScene );
		}
	}
	else
	{
		for ( int i = 0; i < sceneData.numSounds; ++i )
		{
			short stringId = scenefilecache->GetSceneCachedSound( sceneData.sceneId, i );
			CBaseEntity::PrecacheScriptSound( scenefilecache->GetSceneString( stringId ) );
		}
	}

	g_pStringTableClientSideChoreoScenes->AddString( CBaseEntity::IsServer(), pszScene );
}
#endif
static void TouchFile( char const *pchFileName )
{
	IEngineEmulator::Get()->GetFilesystem()->Size( pchFileName );
}

void CGameResponseSystem::Precache()
{
	bool bTouchFiles = CommandLine()->FindParm( "-makereslists" ) != 0;

	// enumerate and mark all the scripts so we know they're referenced
	for ( int i = 0; i < (int)m_Responses.Count(); i++ )
	{
		ResponseGroup &group = m_Responses[i];

		for ( int j = 0; j < group.group.Count(); j++)
		{
			ParserResponse &response = group.group[j];

			switch ( response.type )
			{
			default:
				break;
			case RESPONSE_SCENE:
				{
					// fixup $gender references
					char file[_MAX_PATH];
					Q_strncpy( file, response.value, sizeof(file) );
					char *gender = strstr( file, "$gender" );
					if ( gender )
					{
						// replace with male & female
						const char *postGender = gender + strlen("$gender");
						*gender = 0;
						char genderFile[_MAX_PATH];
						// male
						Q_snprintf( genderFile, sizeof(genderFile), "%smale%s", file, postGender);

						PrecacheInstancedScene( genderFile );
						if ( bTouchFiles )
						{
							TouchFile( genderFile );
						}

						Q_snprintf( genderFile, sizeof(genderFile), "%sfemale%s", file, postGender);

						PrecacheInstancedScene( genderFile );
						if ( bTouchFiles )
						{
							TouchFile( genderFile );
						}
					}
					else
					{
						PrecacheInstancedScene( file );
						if ( bTouchFiles )
						{
							TouchFile( file );
						}
					}
				}
				break;
			case RESPONSE_SPEAK:
				{
					CBaseEntity::PrecacheScriptSound( response.value );
				}
				break;
			}
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: A special purpose response system associated with a custom entity
//-----------------------------------------------------------------------------
class CInstancedResponseSystem : public CGameResponseSystem
{
	typedef CGameResponseSystem BaseClass;

public:
	CInstancedResponseSystem( const char *scriptfile ) :
	  m_pszScriptFile( 0 )
	  {
		  Assert( scriptfile );

		  int len = Q_strlen( scriptfile ) + 1;
		  m_pszScriptFile = new char[ len ];
		  Assert( m_pszScriptFile );
		  Q_strncpy( m_pszScriptFile, scriptfile, len );
	  }

	  ~CInstancedResponseSystem()
	  {
		  delete[] m_pszScriptFile;
	  }
	  virtual const char *GetScriptFile( void ) 
	  {
		  Assert( m_pszScriptFile );
		  return m_pszScriptFile;
	  }

	  // CAutoGameSystem
	  virtual bool Init()
	  {
		  const char *basescript = GetScriptFile();
		  LoadRuleSet( basescript );
		  return true;
	  }

	  virtual void LevelInitPostEntity()
	  {
		  ResetResponseGroups();
	  }

	  virtual void Release()
	  {
		  Clear();
		  delete this;
	  }
private:

	char *m_pszScriptFile;
};

//-----------------------------------------------------------------------------
// Purpose: The default response system for expressive AIs
//-----------------------------------------------------------------------------
class CDefaultResponseSystem : public CGameResponseSystem, public CAutoGameSystem, public IGameScriptPersistable
{
	typedef CAutoGameSystem BaseClass;

public:
	CDefaultResponseSystem() : CAutoGameSystem( "CDefaultResponseSystem" )
	{
	}

	virtual const char *GetScriptFile( void ) 
	{
		return "scripts/talker/response_rules.txt";
	}

	// CAutoServerSystem
	virtual bool Init();
	virtual void Shutdown();

	virtual bool InitScript()
	{
		int c = m_InstancedSystems.Count();
		for (int i = c - 1; i >= 0; i--)
		{
			CInstancedResponseSystem* sys = m_InstancedSystems[i];

			sys->InitScript();
		}

		for (int i = 0; i < (int)m_AlternateSystems.Count(); i++)
		{
			CInstancedResponseSystem* sys = m_AlternateSystems[i];

			sys->InitScript();
		}

		return CResponseSystem::InitScript();
	}
	virtual void TermScript()
	{
		CResponseSystem::TermScript();

		int c = m_InstancedSystems.Count();
		for (int i = c - 1; i >= 0; i--)
		{
			CInstancedResponseSystem* sys = m_InstancedSystems[i];
			
			sys->TermScript();
		}

		for (int i = 0; i < (int)m_AlternateSystems.Count(); i++)
		{
			CInstancedResponseSystem* sys = m_AlternateSystems[i];

			sys->TermScript();
		}
	}

	virtual void LevelInitPostEntity()
	{
	}

	virtual void Release()
	{
		Assert( 0 );
	}

	void AddInstancedResponseSystem( const char *scriptfile, CInstancedResponseSystem *sys )
	{
		m_InstancedSystems.Insert( scriptfile, sys );
	}

	CInstancedResponseSystem *FindResponseSystem( const char *scriptfile )
	{
		int idx = m_InstancedSystems.Find( scriptfile );
		if ( idx == m_InstancedSystems.InvalidIndex() )
			return NULL;
		return m_InstancedSystems[ idx ];
	}

	IResponseSystem *PrecacheCustomResponseSystem( const char *scriptfile )
	{
		COM_TimestampedLog( "PrecacheCustomResponseSystem %s - Start", scriptfile );
		CInstancedResponseSystem *sys = ( CInstancedResponseSystem * )FindResponseSystem( scriptfile );
		if ( !sys )
		{
			sys = new CInstancedResponseSystem( scriptfile );
			if ( !sys )
			{
				Error( "Failed to load response system data from %s", scriptfile );
			}

			if ( !sys->Init() )
			{
				Error( "CInstancedResponseSystem:  Failed to init response system from %s!", scriptfile );
			}

			AddInstancedResponseSystem( scriptfile, sys );
		}

		sys->Precache();
		sys->InitScript();

		COM_TimestampedLog( "PrecacheCustomResponseSystem %s - Finish", scriptfile );

		return ( IResponseSystem * )sys;
	}

	void AddAlternateResponseSystem(const char* scriptfile, CInstancedResponseSystem* sys)
	{
		m_AlternateSystems.Insert(scriptfile, sys);
	}

	CInstancedResponseSystem* FindAlternateResponseSystem(const char* scriptfile)
	{
		int idx = m_AlternateSystems.Find(scriptfile);
		if (idx == m_AlternateSystems.InvalidIndex())
			return NULL;
		return m_AlternateSystems[idx];
	}

	IResponseSystem* PrecacheAlternateResponseSystem(const char* scriptfile)
	{
		COM_TimestampedLog("PrecacheCustomResponseSystem %s - Start", scriptfile);
		CInstancedResponseSystem* sys = (CInstancedResponseSystem*)FindAlternateResponseSystem(scriptfile);
		if (!sys)
		{
			sys = new CInstancedResponseSystem(scriptfile);
			if (!sys)
			{
				Error("Failed to load response system data from %s", scriptfile);
			}

			if (!sys->Init())
			{
				Error("CInstancedResponseSystem:  Failed to init response system from %s!", scriptfile);
			}

			AddAlternateResponseSystem(scriptfile, sys);
		}

		sys->Precache();
		sys->InitScript();

		COM_TimestampedLog("PrecacheCustomResponseSystem %s - Finish", scriptfile);

		return (IResponseSystem*)sys;
	}

	IResponseSystem *BuildCustomResponseSystemGivenCriteria( const char *pszBaseFile, const char *pszCustomName, AI_CriteriaSet &criteriaSet, float flCriteriaScore );
	void DestroyCustomResponseSystems();

	virtual void LevelInitPreEntity()
	{
		// This will precache the default system
		// All user installed systems are init'd by PrecacheCustomResponseSystem which will call sys->Precache() on the ones being used

		// FIXME:  This is SLOW the first time you run the engine (can take 3 - 10 seconds!!!)
		if ( ShouldPrecache() )
		{
			Precache();
		}

		ResetResponseGroups();
	}

	void ReloadAllResponseSystems()
	{
		TermScript();
		Clear();
		Init();
		InitScript();

		int c = m_InstancedSystems.Count();
		for ( int i = c - 1 ; i >= 0; i-- )
		{
			CInstancedResponseSystem *sys = m_InstancedSystems[ i ];
			if ( !IsCustomManagable() )
			{
				sys->TermScript();
				sys->Clear();
				sys->Init();
				sys->InitScript();
			}
			else
			{
				// Custom reponse rules will manage/reload themselves - remove them.
				m_InstancedSystems.RemoveAt( i );
			}
		}

		for (int i = 0; i < (int)m_AlternateSystems.Count(); i++)
		{
			CInstancedResponseSystem* sys = m_AlternateSystems[i];

			sys->TermScript();
			sys->Clear();
			sys->Init();
			sys->InitScript();
		}

		// precache sounds in case we added new ones
		Precache();

	}

	CUtlDict< CInstancedResponseSystem*, int >& GetAlternateSystems()
	{
		return m_AlternateSystems;
	}

private:

	void ClearInstanced()
	{
		int c = m_InstancedSystems.Count();
		for ( int i = c - 1 ; i >= 0; i-- )
		{
			CInstancedResponseSystem *sys = m_InstancedSystems[ i ];
			sys->Release();
		}
		m_InstancedSystems.RemoveAll();
	}

	CUtlDict< CInstancedResponseSystem *, int > m_InstancedSystems;
	CUtlDict< CInstancedResponseSystem*, int > m_AlternateSystems;
	friend void CC_RR_DumpHashInfo( const CCommand &args );
};

ResponseRules::IResponseSystem *CDefaultResponseSystem::BuildCustomResponseSystemGivenCriteria( const char *pszBaseFile, const char *pszCustomName, AI_CriteriaSet &criteriaSet, float flCriteriaScore )
{
	CInstancedResponseSystem* pBaseSystem = static_cast<CInstancedResponseSystem*> (PrecacheCustomResponseSystem(pszBaseFile));

	// Create a instanced response system. 
	CInstancedResponseSystem *pCustomSystem = new CInstancedResponseSystem( pszCustomName );
	if ( !pCustomSystem )
	{
		Error( "BuildCustomResponseSystemGivenCriterea: Failed to create custom response system %s!", pszCustomName );
	}

	pCustomSystem->Clear();

	// Copy the relevant rules and data.
	/*
	int nRuleCount = m_Rules.Count();
	for ( int iRule = 0; iRule < nRuleCount; ++iRule )
	*/
	for ( ResponseRulePartition::tIndex iIdx = pBaseSystem->m_RulePartitions.First() ;
		pBaseSystem->m_RulePartitions.IsValid(iIdx) ;
		iIdx = pBaseSystem->m_RulePartitions.Next( iIdx ) )
	{
		Rule *pRule = &pBaseSystem->m_RulePartitions[iIdx];
		if ( pRule )
		{
			float flScore = 0.0f;

			int nCriteriaCount = pRule->m_Criteria.Count();
			for ( int iCriteria = 0; iCriteria < nCriteriaCount; ++iCriteria )
			{
				int iRuleCriteria = pRule->m_Criteria[iCriteria];

				flScore += pBaseSystem->LookForCriteria( criteriaSet, iRuleCriteria );
				if ( flScore >= flCriteriaScore )
				{
					pBaseSystem->CopyRuleFrom( pRule, iIdx, pCustomSystem );
					break;
				}
			}
		}
	}

	// Set as a custom response system.
	m_bCustomManagable = true;
	AddInstancedResponseSystem( pszCustomName, pCustomSystem );

	//	pCustomSystem->DumpDictionary( pszCustomName );

	return pCustomSystem;
}

void CDefaultResponseSystem::DestroyCustomResponseSystems()
{
	ClearInstanced();
}


static CDefaultResponseSystem defaultresponsesytem;
ResponseRules::IResponseSystem *g_pResponseSystem = &defaultresponsesytem;

CON_COMMAND( rr_reloadresponsesystems, "Reload all response system scripts." )
{
#ifdef GAME_DLL
	if ( !UTIL_IsCommandIssuedByServerAdmin() )
		return;
#endif

	defaultresponsesytem.ReloadAllResponseSystems();
}

// Designed for extern magic, this gives the <, >, etc. of response system criteria to the outside world.
// Mostly just used for Matcher_Match in matchers.h.
bool ResponseSystemCompare(const char* criterion, const char* value)
{
	Criteria criteria;
	criteria.value = criterion;
	defaultresponsesytem.ComputeMatcher(&criteria, criteria.matcher);
	return defaultresponsesytem.CompareUsingMatcher(value, criteria.matcher, true);

	return false;
}

//-----------------------------------------------------------------------------
// CResponseSystemSaveRestoreOps
//
// Purpose: Handles save and load for instanced response systems...
//
// BUGBUG:  This will save the same response system to file multiple times for "shared" response systems and 
//  therefore it'll restore the same data onto the same pointer N times on reload (probably benign for now, but we could
//  write code to save/restore the instanced ones by filename in the block handler above maybe?
//-----------------------------------------------------------------------------

class CResponseSystemSaveRestoreOps : public CDefSaveRestoreOps
{
public:

	virtual void Save(const SaveRestoreFieldInfo_t& fieldInfo, ISave* pSave)
	{
		CResponseSystem* pRS = *(CResponseSystem**)fieldInfo.pField;
		if (!pRS || pRS == &defaultresponsesytem)
			return;

		int count = pRS->m_Responses.Count();
		pSave->WriteInt(&count);
		for (int i = 0; i < count; ++i)
		{
			pSave->StartBlock("ResponseGroup");

			pSave->WriteString(pRS->m_Responses.GetElementName(i));
			const ResponseGroup* group = &pRS->m_Responses[i];
			pSave->WriteAll(group);

			short groupCount = group->group.Count();
			pSave->WriteShort(&groupCount);
			for (int j = 0; j < groupCount; ++j)
			{
				const ParserResponse* response = &group->group[j];
				pSave->StartBlock("Response");
				pSave->WriteString(response->value);
				pSave->WriteAll(response);
				pSave->EndBlock();
			}

			pSave->EndBlock();
		}
	}

	virtual void Restore(const SaveRestoreFieldInfo_t& fieldInfo, IRestore* pRestore)
	{
		CResponseSystem* pRS = *(CResponseSystem**)fieldInfo.pField;
		if (!pRS || pRS == &defaultresponsesytem)
			return;

		int count = pRestore->ReadInt();
		for (int i = 0; i < count; ++i)
		{
			char szResponseGroupBlockName[SIZE_BLOCK_NAME_BUF];
			pRestore->StartBlock(szResponseGroupBlockName);
			if (!Q_stricmp(szResponseGroupBlockName, "ResponseGroup"))
			{

				char groupname[256];
				pRestore->ReadString(groupname, sizeof(groupname), 0);

				// Try and find it
				int idx = pRS->m_Responses.Find(groupname);
				if (idx != pRS->m_Responses.InvalidIndex())
				{
					ResponseGroup* group = &pRS->m_Responses[idx];
					pRestore->ReadAll(group);

					short groupCount = pRestore->ReadShort();
					for (int j = 0; j < groupCount; ++j)
					{
						char szResponseBlockName[SIZE_BLOCK_NAME_BUF];

						char responsename[256];
						pRestore->StartBlock(szResponseBlockName);
						if (!Q_stricmp(szResponseBlockName, "Response"))
						{
							pRestore->ReadString(responsename, sizeof(responsename), 0);

							// Find it by name
							int ri;
							for (ri = 0; ri < group->group.Count(); ++ri)
							{
								ParserResponse* response = &group->group[ri];
								if (!Q_stricmp(response->value, responsename))
								{
									break;
								}
							}

							if (ri < group->group.Count())
							{
								ParserResponse* response = &group->group[ri];
								pRestore->ReadAll(response);
							}
						}

						pRestore->EndBlock();
					}
				}
			}

			pRestore->EndBlock();
		}
	}

} g_ResponseSystemSaveRestoreOps;

ISaveRestoreOps* responseSystemSaveRestoreOps = &g_ResponseSystemSaveRestoreOps;

static short RESPONSESYSTEM_SAVE_RESTORE_VERSION = 2;

// note:  this won't save/restore settings from instanced response systems.  Could add that with a CDefSaveRestoreOps implementation if needed
// 
class CDefaultResponseSystemSaveRestoreBlockHandler : public CDefSaveRestoreBlockHandler
{
public:
	const char *GetBlockName()
	{
		return "ResponseSystem";
	}

	void WriteSaveHeaders( ISave *pSave )
	{
		pSave->WriteShort( &RESPONSESYSTEM_SAVE_RESTORE_VERSION );
	}

	void ReadRestoreHeaders( IRestore *pRestore )
	{
		// No reason why any future version shouldn't try to retain backward compatability. The default here is to not do so.
		//short version;
		pRestore->ReadShort( &m_sVersion );
		m_fDoLoad = (m_sVersion <= RESPONSESYSTEM_SAVE_RESTORE_VERSION );
	}

	void Save( ISave *pSave )
	{
		CDefaultResponseSystem& rs = defaultresponsesytem;

		int count = rs.m_Responses.Count();
		pSave->WriteInt( &count );
		for ( int i = 0; i < count; ++i )
		{
			pSave->StartBlock( "ResponseGroup" );

			pSave->WriteString( rs.m_Responses.GetElementName( i ) );
			const ResponseGroup *group = &rs.m_Responses[ i ];
			pSave->WriteAll( group );

			short groupCount = group->group.Count();
			pSave->WriteShort( &groupCount );
			for ( int j = 0; j < groupCount; ++j )
			{
				const ParserResponse *response = &group->group[ j ];
				pSave->StartBlock( "Response" );
				pSave->WriteString( response->value );
				pSave->WriteAll( response );
				pSave->EndBlock();
			}

			pSave->EndBlock();
		}

		pSave->StartBlock("AlternateSystems");
		int c = rs.GetAlternateSystems().Count();
		pSave->WriteInt(&c);
		for (int i = 0; i < c; i++)
		{
			CInstancedResponseSystem *sys = rs.GetAlternateSystems().Element(i);
			pSave->WriteString(sys->GetScriptFile());
			CResponseSystem* pResp = sys;
			CResponseSystem** pField = &pResp;

			responseSystemSaveRestoreOps->Save(pField, pSave);
		}
		pSave->EndBlock();
	}

	void Restore( IRestore *pRestore, bool createPlayers )
	{
		if ( !m_fDoLoad )
			return;

		CDefaultResponseSystem& rs = defaultresponsesytem;

		int count = pRestore->ReadInt();
		for ( int i = 0; i < count; ++i )
		{
			char szResponseGroupBlockName[SIZE_BLOCK_NAME_BUF];
			pRestore->StartBlock( szResponseGroupBlockName );
			if ( !Q_stricmp( szResponseGroupBlockName, "ResponseGroup" ) )
			{

				char groupname[ 256 ];
				pRestore->ReadString( groupname, sizeof( groupname ), 0 );

				// Try and find it
				int idx = rs.m_Responses.Find( groupname );
				if ( idx != rs.m_Responses.InvalidIndex() )
				{
					ResponseGroup *group = &rs.m_Responses[ idx ];
					pRestore->ReadAll( group );

					short groupCount = pRestore->ReadShort();
					for ( int j = 0; j < groupCount; ++j )
					{
						char szResponseBlockName[SIZE_BLOCK_NAME_BUF];

						char responsename[ 256 ];
						pRestore->StartBlock( szResponseBlockName );
						if ( !Q_stricmp( szResponseBlockName, "Response" ) )
						{
							pRestore->ReadString( responsename, sizeof( responsename ), 0 );

							// Find it by name
							int ri;
							for ( ri = 0; ri < group->group.Count(); ++ri )
							{
								ParserResponse *response = &group->group[ ri ];
								if ( !Q_stricmp( response->value, responsename ) )
								{
									break;
								}
							}

							if ( ri < group->group.Count() )
							{
								ParserResponse *response = &group->group[ ri ];
								pRestore->ReadAll( response );
							}
						}

						pRestore->EndBlock();
					}
				}
			}

			pRestore->EndBlock();
		}

		if (m_sVersion < 2)
			return;

		char szAlternateSystemsBlockName[SIZE_BLOCK_NAME_BUF];
		pRestore->StartBlock(szAlternateSystemsBlockName);
		if (!Q_stricmp(szAlternateSystemsBlockName, "AlternateSystems"))
		{
			int iCount = pRestore->ReadInt();
			for (int i = 0; i < iCount; i++)
			{
				char chScript[MAX_PATH];
				pRestore->ReadString(chScript, MAX_PATH, 0);
				CResponseSystem* sys = (CResponseSystem*)rs.PrecacheAlternateResponseSystem(chScript);
				CResponseSystem** pField = &sys;

				responseSystemSaveRestoreOps->Restore(pField, pRestore);
			}
		}
		pRestore->EndBlock();
	}
private:
	short		m_sVersion;
	bool		m_fDoLoad;

} g_DefaultResponseSystemSaveRestoreBlockHandler;

ISaveRestoreBlockHandler *GetDefaultResponseSystemSaveRestoreBlockHandler()
{
	return &g_DefaultResponseSystemSaveRestoreBlockHandler;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : Returns true on success, false on failure.
//-----------------------------------------------------------------------------
bool CDefaultResponseSystem::Init()
{
	g_ScriptPersistableList.AddToTail(this);

	/*
	Warning( "sizeof( Response ) == %d\n", sizeof( Response ) );
	Warning( "sizeof( ResponseGroup ) == %d\n", sizeof( ResponseGroup ) );
	Warning( "sizeof( Criteria ) == %d\n", sizeof( Criteria ) );
	Warning( "sizeof( AI_ResponseParams ) == %d\n", sizeof( AI_ResponseParams ) );
	*/
	const char *basescript = GetScriptFile();

	LoadRuleSet( basescript );

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CDefaultResponseSystem::Shutdown()
{
	g_ScriptPersistableList.FindAndRemove(this);

	// Wipe instanced versions
	ClearInstanced();

	// Clear outselves
	Clear();
	// IServerSystem chain
	BaseClass::Shutdown();
}


//-----------------------------------------------------------------------------
// Purpose: Instance a custom response system
// Input  : *scriptfile - 
// Output : IResponseSystem
//-----------------------------------------------------------------------------
ResponseRules::IResponseSystem *PrecacheCustomResponseSystem( const char *scriptfile )
{
	return defaultresponsesytem.PrecacheCustomResponseSystem( scriptfile );
}

ResponseRules::IResponseSystem* GetAlternateResponseSystem(const char* scriptfile)
{
	return defaultresponsesytem.PrecacheAlternateResponseSystem(scriptfile);
}

//-----------------------------------------------------------------------------
// Purpose: Instance a custom response system
// Input  : *scriptfile -
//			set - 
// Output : IResponseSystem
//-----------------------------------------------------------------------------
ResponseRules::IResponseSystem *BuildCustomResponseSystemGivenCriteria( const char *pszBaseFile, const char *pszCustomName, AI_CriteriaSet &criteriaSet, float flCriteriaScore )
{
	return defaultresponsesytem.BuildCustomResponseSystemGivenCriteria( pszBaseFile, pszCustomName, criteriaSet, flCriteriaScore );
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void DestroyCustomResponseSystems()
{
	defaultresponsesytem.DestroyCustomResponseSystems();
}
