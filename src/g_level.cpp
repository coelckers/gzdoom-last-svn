/*
** g_level.cpp
** Parses MAPINFO and controls movement between levels
**
**---------------------------------------------------------------------------
** Copyright 1998-2006 Randy Heit
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#include <assert.h>
#include "templates.h"
#include "d_main.h"
#include "m_alloc.h"
#include "g_level.h"
#include "g_game.h"
#include "s_sound.h"
#include "d_event.h"
#include "m_random.h"
#include "doomstat.h"
#include "wi_stuff.h"
#include "r_data.h"
#include "w_wad.h"
#include "am_map.h"
#include "c_dispatch.h"
#include "i_system.h"
#include "p_setup.h"
#include "p_local.h"
#include "r_sky.h"
#include "c_console.h"
#include "f_finale.h"
#include "gstrings.h"
#include "v_video.h"
#include "st_stuff.h"
#include "hu_stuff.h"
#include "p_saveg.h"
#include "p_acs.h"
#include "d_protocol.h"
#include "v_text.h"
#include "s_sndseq.h"
#include "b_bot.h"
#include "sc_man.h"
#include "sbar.h"
#include "a_lightning.h"
#include "m_png.h"
#include "m_random.h"
#include "version.h"
#include "m_menu.h"
#include "statnums.h"
#include "sbarinfo.h"
#include "r_translate.h"
#include "p_lnspec.h"
#include "r_interpolate.h"

#include "gi.h"

#include "g_hub.h"



#include "gl/gl_functions.h"

#ifndef STAT
#define STAT_NEW(map)
#define STAT_END(newl)
#define STAT_SAVE(arc, hub)
#else
void STAT_NEW(const char *lev);
void STAT_END(const char *newl);
void STAT_SAVE(FArchive &arc, bool hubload);
#endif

EXTERN_CVAR (Float, sv_gravity)
EXTERN_CVAR (Float, sv_aircontrol)
EXTERN_CVAR (Int, disableautosave)

// Hey, GCC, these macros better be safe!
#define lioffset(x)		((size_t)&((level_info_t*)1)->x - 1)
#define cioffset(x)		((size_t)&((cluster_info_t*)1)->x - 1)

#define SNAP_ID			MAKE_ID('s','n','A','p')
#define DSNP_ID			MAKE_ID('d','s','N','p')
#define VIST_ID			MAKE_ID('v','i','S','t')
#define ACSD_ID			MAKE_ID('a','c','S','d')
#define RCLS_ID			MAKE_ID('r','c','L','s')
#define PCLS_ID			MAKE_ID('p','c','L','s')

static int FindEndSequence (int type, const char *picname);
static void SetEndSequence (char *nextmap, int type);
static void InitPlayerClasses ();
static void ParseEpisodeInfo (FScanner &sc);
static void G_DoParseMapInfo (int lump);
static void SetLevelNum (level_info_t *info, int num);
static void ClearEpisodes ();
static void ClearLevelInfoStrings (level_info_t *linfo);
static void ClearClusterInfoStrings (cluster_info_t *cinfo);
static void ParseSkill (FScanner &sc);
static void G_VerifySkill();

struct FOptionalMapinfoParser
{
	const char *keyword;
	MIParseFunc parsefunc;
};

static TArray<FOptionalMapinfoParser> optmapinf(TArray<FOptionalMapinfoParser>::NoInit);


void AddOptionalMapinfoParser(const char *keyword, MIParseFunc parsefunc)
{
	FOptionalMapinfoParser mi;
	
	mi.keyword = keyword;
	mi.parsefunc = parsefunc;
	optmapinf.Push(mi);
}

static void ParseOptionalBlock(const char *keyword, FScanner &sc, level_info_t *info)
{
	for(unsigned i=0;i<optmapinf.Size();i++)
	{
		if (!stricmp(keyword, optmapinf[i].keyword))
		{
			optmapinf[i].parsefunc(sc, info);
			return;
		}
	}
	int bracecount = 0;
	
	while (sc.GetString())
	{
		if (sc.Compare("{")) bracecount++;
		else if (sc.Compare("}"))
		{
			if (--bracecount < 0) return;
		}
	}
}
			


static FRandom pr_classchoice ("RandomPlayerClassChoice");

TArray<EndSequence> EndSequences;

EndSequence::EndSequence()
{
	EndType = END_Pic;
	Advanced = false;
	MusicLooping = false;
	PlayTheEnd = false;
}

extern bool timingdemo;

// Start time for timing demos
int starttime;


// ACS variables with world scope
SDWORD ACS_WorldVars[NUM_WORLDVARS];
FWorldGlobalArray ACS_WorldArrays[NUM_WORLDVARS];

// ACS variables with global scope
SDWORD ACS_GlobalVars[NUM_GLOBALVARS];
FWorldGlobalArray ACS_GlobalArrays[NUM_GLOBALVARS];

extern bool netdemo;
extern FString BackupSaveName;

bool savegamerestore;

extern int mousex, mousey;
extern bool sendpause, sendsave, sendturn180, SendLand;
extern const AInventory *SendItemUse, *SendItemDrop;

void *statcopy;					// for statistics driver

FLevelLocals level;			// info about current level

static TArray<cluster_info_t> wadclusterinfos;
TArray<level_info_t> wadlevelinfos;
TArray<FSkillInfo> AllSkills;

// MAPINFO is parsed slightly differently when the map name is just a number.
static bool HexenHack;

static char unnamed[] = "Unnamed";
static level_info_t TheDefaultLevelInfo =
{
 	"",			// mapname
 	0, 			// levelnum
 	"", 		// pname,
 	"", 		// nextmap
 	"",			// secretmap
 	"SKY1",		// skypic1
 	0, 			// cluster
 	0, 			// partime
 	0, 			// sucktime
 	0, 			// flags
 	NULL, 		// music
 	unnamed, 	// level_name
 	"COLORMAP",	// fadetable
 	+8, 		// WallVertLight
 	-8,			// WallHorizLight
	"",			// [RC] F1
};

static cluster_info_t TheDefaultClusterInfo = { 0 };



static const char *MapInfoTopLevel[] =
{
	"map",
	"defaultmap",
	"clusterdef",
	"episode",
	"clearepisodes",
	"skill",
	"clearskills",
	"adddefaultmap",
	NULL
};

enum
{
	MITL_MAP,
	MITL_DEFAULTMAP,
	MITL_CLUSTERDEF,
	MITL_EPISODE,
	MITL_CLEAREPISODES,
	MITL_SKILL,
	MITL_CLEARSKILLS,
	MITL_ADDDEFAULTMAP,
};

static const char *MapInfoMapLevel[] =
{
	"levelnum",
	"next",
	"secretnext",
	"cluster",
	"sky1",
	"sky2",
	"fade",
	"outsidefog",
	"titlepatch",
	"par",
	"sucktime",
	"music",
	"nointermission",
	"intermission",
	"doublesky",
	"nosoundclipping",
	"allowmonstertelefrags",
	"map07special",
	"baronspecial",
	"cyberdemonspecial",
	"spidermastermindspecial",
	"minotaurspecial",
	"dsparilspecial",
	"ironlichspecial",
	"specialaction_exitlevel",
	"specialaction_opendoor",
	"specialaction_lowerfloor",
	"specialaction_killmonsters",
	"lightning",
	"fadetable",
	"evenlighting",
	"noautosequences",
	"forcenoskystretch",
	"allowfreelook",
	"nofreelook",
	"allowjump",
	"nojump",
	"fallingdamage",		// Hexen falling damage
	"oldfallingdamage",		// Lesser ZDoom falling damage
	"forcefallingdamage",	// Skull Tag compatibility name for oldfallingdamage
	"strifefallingdamage",	// Strife's falling damage is really unforgiving
	"nofallingdamage",
	"noallies",
	"cdtrack",
	"cdid",
	"cd_start_track",
	"cd_end1_track",
	"cd_end2_track",
	"cd_end3_track",
	"cd_intermission_track",
	"cd_title_track",
	"warptrans",
	"vertwallshade",
	"horizwallshade",
	"gravity",
	"aircontrol",
	"filterstarts",
	"activateowndeathspecials",
	"killeractivatesdeathspecials",
	"missilesactivateimpactlines",
	"missileshootersactivetimpactlines",
	"noinventorybar",
	"deathslideshow",
	"redirect",
	"strictmonsteractivation",
	"laxmonsteractivation",
	"additive_scrollers",
	"interpic",
	"exitpic",
	"enterpic",
	"intermusic",
	"airsupply",
	"specialaction",
	"keepfullinventory",
	"monsterfallingdamage",
	"nomonsterfallingdamage",
	"sndseq",
	"sndinfo",
	"soundinfo",
	"clipmidtextures",
	"wrapmidtextures",
	"allowcrouch",
	"nocrouch",
	"pausemusicinmenus",
	"compat_shorttex",	
	"compat_stairs",		
	"compat_limitpain",	
	"compat_nopassover",	
	"compat_notossdrops",	
	"compat_useblocking", 
	"compat_nodoorlight",	
	"compat_ravenscroll",	
	"compat_soundtarget",	
	"compat_dehhealth",	
	"compat_trace",		
	"compat_dropoff",
	"compat_boomscroll",
	"compat_invisibility",
	"compat_silent_instant_floors",
	"compat_sectorsounds",
	"bordertexture",
	"f1", // [RC] F1 help
	"noinfighting",
	"normalinfighting",
	"totalinfighting",
	"infiniteflightpowerup",
	"noinfiniteflightpowerup",
	"allowrespawn",
	"teamdamage",
	"teamplayon",
	"teamplayoff",
	"checkswitchrange",
	"nocheckswitchrange",
	"translator",
	"unfreezesingleplayerconversations",
	"nobotnodes",
	NULL
};

enum EMIType
{
	MITYPE_EATNEXT,
	MITYPE_IGNORE,
	MITYPE_INT,
	MITYPE_FLOAT,
	MITYPE_HEX,
	MITYPE_COLOR,
	MITYPE_MAPNAME,
	MITYPE_LUMPNAME,
	MITYPE_SKY,
	MITYPE_SETFLAG,
	MITYPE_CLRFLAG,
	MITYPE_SCFLAGS,
	MITYPE_CLUSTER,
	MITYPE_STRING,
	MITYPE_MUSIC,
	MITYPE_RELLIGHT,
	MITYPE_CLRBYTES,
	MITYPE_REDIRECT,
	MITYPE_SPECIALACTION,
	MITYPE_COMPATFLAG,
	MITYPE_STRINGT,
};

struct MapInfoHandler
{
	EMIType type;
	QWORD data1, data2;
}
MapHandlers[] =
{
	{ MITYPE_INT,		lioffset(levelnum), 0 },
	{ MITYPE_MAPNAME,	lioffset(nextmap), 0 },
	{ MITYPE_MAPNAME,	lioffset(secretmap), 0 },
	{ MITYPE_CLUSTER,	lioffset(cluster), 0 },
	{ MITYPE_SKY,		lioffset(skypic1), lioffset(skyspeed1) },
	{ MITYPE_SKY,		lioffset(skypic2), lioffset(skyspeed2) },
	{ MITYPE_COLOR,		lioffset(fadeto), 0 },
	{ MITYPE_COLOR,		lioffset(outsidefog), 0 },
	{ MITYPE_LUMPNAME,	lioffset(pname), 0 },
	{ MITYPE_INT,		lioffset(partime), 0 },
	{ MITYPE_INT,		lioffset(sucktime), 0 },
	{ MITYPE_MUSIC,		lioffset(music), lioffset(musicorder) },
	{ MITYPE_SETFLAG,	LEVEL_NOINTERMISSION, 0 },
	{ MITYPE_CLRFLAG,	LEVEL_NOINTERMISSION, 0 },
	{ MITYPE_SETFLAG,	LEVEL_DOUBLESKY, 0 },
	{ MITYPE_IGNORE,	0, 0 },	// was nosoundclipping
	{ MITYPE_SETFLAG,	LEVEL_MONSTERSTELEFRAG, 0 },
	{ MITYPE_SETFLAG,	LEVEL_MAP07SPECIAL, 0 },
	{ MITYPE_SETFLAG,	LEVEL_BRUISERSPECIAL, 0 },
	{ MITYPE_SETFLAG,	LEVEL_CYBORGSPECIAL, 0 },
	{ MITYPE_SETFLAG,	LEVEL_SPIDERSPECIAL, 0 },
	{ MITYPE_SETFLAG,	LEVEL_MINOTAURSPECIAL, 0 },
	{ MITYPE_SETFLAG,	LEVEL_SORCERER2SPECIAL, 0 },
	{ MITYPE_SETFLAG,	LEVEL_HEADSPECIAL, 0 },
	{ MITYPE_SCFLAGS,	0, ~LEVEL_SPECACTIONSMASK },
	{ MITYPE_SCFLAGS,	LEVEL_SPECOPENDOOR, ~LEVEL_SPECACTIONSMASK },
	{ MITYPE_SCFLAGS,	LEVEL_SPECLOWERFLOOR, ~LEVEL_SPECACTIONSMASK },
	{ MITYPE_SETFLAG,	LEVEL_SPECKILLMONSTERS, 0 },
	{ MITYPE_SETFLAG,	LEVEL_STARTLIGHTNING, 0 },
	{ MITYPE_LUMPNAME,	lioffset(fadetable), 0 },
	{ MITYPE_CLRBYTES,	lioffset(WallVertLight), lioffset(WallHorizLight) },
	{ MITYPE_SETFLAG,	LEVEL_SNDSEQTOTALCTRL, 0 },
	{ MITYPE_SETFLAG,	LEVEL_FORCENOSKYSTRETCH, 0 },
	{ MITYPE_SCFLAGS,	LEVEL_FREELOOK_YES, ~LEVEL_FREELOOK_NO },
	{ MITYPE_SCFLAGS,	LEVEL_FREELOOK_NO, ~LEVEL_FREELOOK_YES },
	{ MITYPE_CLRFLAG,	LEVEL_JUMP_NO, 0 },
	{ MITYPE_SETFLAG,	LEVEL_JUMP_NO, 0 },
	{ MITYPE_SCFLAGS,	LEVEL_FALLDMG_HX, ~LEVEL_FALLDMG_ZD },
	{ MITYPE_SCFLAGS,	LEVEL_FALLDMG_ZD, ~LEVEL_FALLDMG_HX },
	{ MITYPE_SCFLAGS,	LEVEL_FALLDMG_ZD, ~LEVEL_FALLDMG_HX },
	{ MITYPE_SETFLAG,	LEVEL_FALLDMG_ZD|LEVEL_FALLDMG_HX, 0 },
	{ MITYPE_SCFLAGS,	0, ~(LEVEL_FALLDMG_ZD|LEVEL_FALLDMG_HX) },
	{ MITYPE_SETFLAG,	LEVEL_NOALLIES, 0 },
	{ MITYPE_INT,		lioffset(cdtrack), 0 },
	{ MITYPE_HEX,		lioffset(cdid), 0 },
	{ MITYPE_EATNEXT,	0, 0 },
	{ MITYPE_EATNEXT,	0, 0 },
	{ MITYPE_EATNEXT,	0, 0 },
	{ MITYPE_EATNEXT,	0, 0 },
	{ MITYPE_EATNEXT,	0, 0 },
	{ MITYPE_EATNEXT,	0, 0 },
	{ MITYPE_INT,		lioffset(WarpTrans), 0 },
	{ MITYPE_RELLIGHT,	lioffset(WallVertLight), 0 },
	{ MITYPE_RELLIGHT,	lioffset(WallHorizLight), 0 },
	{ MITYPE_FLOAT,		lioffset(gravity), 0 },
	{ MITYPE_FLOAT,		lioffset(aircontrol), 0 },
	{ MITYPE_SETFLAG,	LEVEL_FILTERSTARTS, 0 },
	{ MITYPE_SETFLAG,	LEVEL_ACTOWNSPECIAL, 0 },
	{ MITYPE_CLRFLAG,	LEVEL_ACTOWNSPECIAL, 0 },
	{ MITYPE_SETFLAG,	LEVEL_MISSILESACTIVATEIMPACT, 0 },
	{ MITYPE_CLRFLAG,	LEVEL_MISSILESACTIVATEIMPACT, 0 },
	{ MITYPE_SETFLAG,	LEVEL_NOINVENTORYBAR, 0 },
	{ MITYPE_SETFLAG,	LEVEL_DEATHSLIDESHOW, 0 },
	{ MITYPE_REDIRECT,	lioffset(RedirectMap), 0 },
	{ MITYPE_CLRFLAG,	LEVEL_LAXMONSTERACTIVATION, LEVEL_LAXACTIVATIONMAPINFO },
	{ MITYPE_SETFLAG,	LEVEL_LAXMONSTERACTIVATION, LEVEL_LAXACTIVATIONMAPINFO },
	{ MITYPE_COMPATFLAG, COMPATF_BOOMSCROLL},
	{ MITYPE_STRING,	lioffset(exitpic), 0 },
	{ MITYPE_STRING,	lioffset(exitpic), 0 },
	{ MITYPE_STRING,	lioffset(enterpic), 0 },
	{ MITYPE_MUSIC,		lioffset(intermusic), lioffset(intermusicorder) },
	{ MITYPE_INT,		lioffset(airsupply), 0 },
	{ MITYPE_SPECIALACTION, lioffset(specialactions), 0 },
	{ MITYPE_SETFLAG,	LEVEL_KEEPFULLINVENTORY, 0 },
	{ MITYPE_SETFLAG,	LEVEL_MONSTERFALLINGDAMAGE, 0 },
	{ MITYPE_CLRFLAG,	LEVEL_MONSTERFALLINGDAMAGE, 0 },
	{ MITYPE_STRING,	lioffset(sndseq), 0 },
	{ MITYPE_STRING,	lioffset(soundinfo), 0 },
	{ MITYPE_STRING,	lioffset(soundinfo), 0 },
	{ MITYPE_SETFLAG,	LEVEL_CLIPMIDTEX, 0 },
	{ MITYPE_SETFLAG,	LEVEL_WRAPMIDTEX, 0 },
	{ MITYPE_CLRFLAG,	LEVEL_CROUCH_NO, 0 },
	{ MITYPE_SETFLAG,	LEVEL_CROUCH_NO, 0 },
	{ MITYPE_SCFLAGS,	LEVEL_PAUSE_MUSIC_IN_MENUS, 0 },
	{ MITYPE_COMPATFLAG, COMPATF_SHORTTEX},
	{ MITYPE_COMPATFLAG, COMPATF_STAIRINDEX},
	{ MITYPE_COMPATFLAG, COMPATF_LIMITPAIN},
	{ MITYPE_COMPATFLAG, COMPATF_NO_PASSMOBJ},
	{ MITYPE_COMPATFLAG, COMPATF_NOTOSSDROPS},
	{ MITYPE_COMPATFLAG, COMPATF_USEBLOCKING},
	{ MITYPE_COMPATFLAG, COMPATF_NODOORLIGHT},
	{ MITYPE_COMPATFLAG, COMPATF_RAVENSCROLL},
	{ MITYPE_COMPATFLAG, COMPATF_SOUNDTARGET},
	{ MITYPE_COMPATFLAG, COMPATF_DEHHEALTH},
	{ MITYPE_COMPATFLAG, COMPATF_TRACE},
	{ MITYPE_COMPATFLAG, COMPATF_DROPOFF},
	{ MITYPE_COMPATFLAG, COMPATF_BOOMSCROLL},
	{ MITYPE_COMPATFLAG, COMPATF_INVISIBILITY},
	{ MITYPE_COMPATFLAG, COMPATF_SILENT_INSTANT_FLOORS},
	{ MITYPE_COMPATFLAG, COMPATF_SECTORSOUNDS},
	{ MITYPE_LUMPNAME,	lioffset(bordertexture), 0 },
	{ MITYPE_LUMPNAME,  lioffset(f1), 0, }, 
	{ MITYPE_SCFLAGS,	LEVEL_NOINFIGHTING, ~LEVEL_TOTALINFIGHTING },
	{ MITYPE_SCFLAGS,	0, ~(LEVEL_NOINFIGHTING|LEVEL_TOTALINFIGHTING)},
	{ MITYPE_SCFLAGS,	LEVEL_TOTALINFIGHTING, ~LEVEL_NOINFIGHTING },
	{ MITYPE_SETFLAG,	LEVEL_INFINITE_FLIGHT, 0 },
	{ MITYPE_CLRFLAG,	LEVEL_INFINITE_FLIGHT, 0 },
	{ MITYPE_SETFLAG,	LEVEL_ALLOWRESPAWN, 0 },
	{ MITYPE_FLOAT,		lioffset(teamdamage), 0 },
	{ MITYPE_SCFLAGS,	LEVEL_FORCETEAMPLAYON, ~LEVEL_FORCETEAMPLAYOFF },
	{ MITYPE_SCFLAGS,	LEVEL_FORCETEAMPLAYOFF, ~LEVEL_FORCETEAMPLAYON },
	{ MITYPE_SETFLAG,	LEVEL_CHECKSWITCHRANGE, 0 },
	{ MITYPE_CLRFLAG,	LEVEL_CHECKSWITCHRANGE, 0 },
	{ MITYPE_STRING,	lioffset(translator), 0 },
	{ MITYPE_SETFLAG,	LEVEL_CONV_SINGLE_UNFREEZE, 0 },
	{ MITYPE_IGNORE,	0, 0 },		// Skulltag option: nobotnodes
};

static const char *MapInfoClusterLevel[] =
{
	"entertext",
	"exittext",
	"music",
	"flat",
	"pic",
	"hub",
	"cdtrack",
	"cdid",
	"entertextislump",
	"exittextislump",
	"name",
	NULL
};

MapInfoHandler ClusterHandlers[] =
{
	{ MITYPE_STRINGT,	cioffset(entertext), CLUSTER_LOOKUPENTERTEXT },
	{ MITYPE_STRINGT,	cioffset(exittext), CLUSTER_LOOKUPEXITTEXT },
	{ MITYPE_MUSIC,		cioffset(messagemusic), cioffset(musicorder) },
	{ MITYPE_LUMPNAME,	cioffset(finaleflat), 0 },
	{ MITYPE_LUMPNAME,	cioffset(finaleflat), CLUSTER_FINALEPIC },
	{ MITYPE_SETFLAG,	CLUSTER_HUB, 0 },
	{ MITYPE_INT,		cioffset(cdtrack), 0 },
	{ MITYPE_HEX,		cioffset(cdid), 0 },
	{ MITYPE_SETFLAG,	CLUSTER_ENTERTEXTINLUMP, 0 },
	{ MITYPE_SETFLAG,	CLUSTER_EXITTEXTINLUMP, 0 },
	{ MITYPE_STRINGT,	cioffset(clustername), CLUSTER_LOOKUPNAME },
};

static void ParseMapInfoLower (FScanner &sc,
							   MapInfoHandler *handlers,
							   const char *strings[],
							   level_info_t *levelinfo,
							   cluster_info_t *clusterinfo,
							   QWORD levelflags);

static int FindWadLevelInfo (const char *name)
{
	for (unsigned int i = 0; i < wadlevelinfos.Size(); i++)
		if (!strnicmp (name, wadlevelinfos[i].mapname, 8))
			return i;
		
	return -1;
}

static int FindWadClusterInfo (int cluster)
{
	for (unsigned int i = 0; i < wadclusterinfos.Size(); i++)
		if (wadclusterinfos[i].cluster == cluster)
			return i;
		
	return -1;
}

static void SetLevelDefaults (level_info_t *levelinfo)
{
	memset (levelinfo, 0, sizeof(*levelinfo));
	levelinfo->snapshot = NULL;
	levelinfo->outsidefog = 0xff000000;
	levelinfo->WallHorizLight = -8;
	levelinfo->WallVertLight = +8;
	strncpy (levelinfo->fadetable, "COLORMAP", 8);
	strcpy (levelinfo->skypic1, "-NOFLAT-");
	strcpy (levelinfo->skypic2, "-NOFLAT-");
	strcpy (levelinfo->bordertexture, gameinfo.borderFlat);
	if (gameinfo.gametype != GAME_Hexen)
	{
		// For maps without a BEHAVIOR, this will be cleared.
		levelinfo->flags |= LEVEL_LAXMONSTERACTIVATION;
	}
	levelinfo->airsupply = 10;

	// new
	levelinfo->airsupply = 20;
}

//
// G_ParseMapInfo
// Parses the MAPINFO lumps of all loaded WADs and generates
// data for wadlevelinfos and wadclusterinfos.
//
void G_ParseMapInfo ()
{
	int lump, lastlump = 0;

	gl_AddMapinfoParser() ;
	atterm (G_UnloadMapInfo);

	// Parse the default MAPINFO for the current game.
	switch (gameinfo.gametype)
	{
	case GAME_Doom:
		G_DoParseMapInfo (Wads.GetNumForFullName ("mapinfo/doomcommon.txt"));
		switch (gamemission)
		{
		case doom:
			G_DoParseMapInfo (Wads.GetNumForFullName ("mapinfo/doom1.txt"));
			break;
		case pack_plut:
			G_DoParseMapInfo (Wads.GetNumForFullName ("mapinfo/plutonia.txt"));
			break;
		case pack_tnt:
			G_DoParseMapInfo (Wads.GetNumForFullName ("mapinfo/tnt.txt"));
			break;
		default:
			G_DoParseMapInfo (Wads.GetNumForFullName ("mapinfo/doom2.txt"));
			break;
		}
		break;

	case GAME_Heretic:
		G_DoParseMapInfo (Wads.GetNumForFullName ("mapinfo/heretic.txt"));
		break;

	case GAME_Hexen:
		G_DoParseMapInfo (Wads.GetNumForFullName ("mapinfo/hexen.txt"));
		break;

	case GAME_Strife:
		G_DoParseMapInfo (Wads.GetNumForFullName ("mapinfo/strife.txt"));
		break;

	default:
		break;
	}

	// Parse any extra MAPINFOs.
	while ((lump = Wads.FindLump ("MAPINFO", &lastlump)) != -1)
	{
		G_DoParseMapInfo (lump);
	}
	EndSequences.ShrinkToFit ();

	if (EpiDef.numitems == 0)
	{
		I_FatalError ("You cannot use clearepisodes in a MAPINFO if you do not define any new episodes after it.");
	}
	if (AllSkills.Size()==0)
	{
		I_FatalError ("You cannot use clearskills in a MAPINFO if you do not define any new skills after it.");
	}
}

static FSpecialAction *CopySpecialActions(FSpecialAction *spec)
{
	FSpecialAction **pSpec = &spec;

	while (*pSpec)
	{
		FSpecialAction *newspec = new FSpecialAction;
		*newspec = **pSpec;
		*pSpec = newspec;
		pSpec = &newspec->Next;
	}
	return spec;
}

static FOptionalMapinfoData *CopyOptData(FOptionalMapinfoData *opdata)
{
	FOptionalMapinfoData **opt = &opdata;

	while (*opt)
	{
		FOptionalMapinfoData *newop = (*opt)->Clone();
		*opt = newop;
		opt = &newop->Next;
	}
	return opdata;
}

static void CopyString (char *& string)
{
	if (string != NULL)
		string = copystring(string);
}

static void SafeDelete(char *&string)
{
	if (string != NULL)
	{
		delete[] string;
		string = NULL;
	}
}

static void ClearLevelInfoStrings(level_info_t *linfo)
{
	SafeDelete(linfo->music);
	SafeDelete(linfo->intermusic);
	SafeDelete(linfo->level_name);
	SafeDelete(linfo->translator);
	SafeDelete(linfo->enterpic);
	SafeDelete(linfo->exitpic);
	SafeDelete(linfo->soundinfo);
	SafeDelete(linfo->sndseq);
	for (FSpecialAction *spac = linfo->specialactions; spac != NULL; )
	{
		FSpecialAction *next = spac->Next;
		delete spac;
		spac = next;
	}
	for (FOptionalMapinfoData *spac = linfo->opdata; spac != NULL; )
	{
		FOptionalMapinfoData *next = spac->Next;
		delete spac;
		spac = next;
	}
}

static void ClearClusterInfoStrings(cluster_info_t *cinfo)
{
	SafeDelete(cinfo->exittext);
	SafeDelete(cinfo->entertext);
	SafeDelete(cinfo->messagemusic);
	SafeDelete(cinfo->clustername);
}


static void G_DoParseMapInfo (int lump)
{
	level_info_t defaultinfo;
	level_info_t *levelinfo;
	int levelindex;
	cluster_info_t *clusterinfo;
	int clusterindex;
	QWORD levelflags;

	FScanner sc(lump);

	SetLevelDefaults (&defaultinfo);
	HexenHack = false;

	while (sc.GetString ())
	{
		switch (sc.MustMatchString (MapInfoTopLevel))
		{
		case MITL_DEFAULTMAP:
			ClearLevelInfoStrings(&defaultinfo);
			SetLevelDefaults (&defaultinfo);
			ParseMapInfoLower (sc, MapHandlers, MapInfoMapLevel, &defaultinfo, NULL, defaultinfo.flags);
			break;

		case MITL_ADDDEFAULTMAP:
			// Same as above but adds to the existing definitions instead of replacing them completely
			ParseMapInfoLower (sc, MapHandlers, MapInfoMapLevel, &defaultinfo, NULL, defaultinfo.flags);
			break;

		case MITL_MAP:		// map <MAPNAME> <Nice Name>
		  {
			char maptemp[8];
			char *mapname;

			levelflags = defaultinfo.flags;
			sc.MustGetString ();
			mapname = sc.String;
			if (IsNum (mapname))
			{	// MAPNAME is a number; assume a Hexen wad
				int mapnum = atoi (mapname);
				mysnprintf (maptemp, countof(maptemp), "MAP%02d", mapnum);
				mapname = maptemp;
				HexenHack = true;
				// Hexen levels are automatically nointermission,
				// no auto sound sequences, falling damage,
				// monsters activate their own specials, and missiles
				// are always the activators of impact lines.
				levelflags |= LEVEL_NOINTERMISSION
							| LEVEL_SNDSEQTOTALCTRL
							| LEVEL_FALLDMG_HX
							| LEVEL_ACTOWNSPECIAL
							| LEVEL_MISSILESACTIVATEIMPACT
							| LEVEL_INFINITE_FLIGHT
							| LEVEL_MONSTERFALLINGDAMAGE
							| LEVEL_HEXENHACK;
			}
			levelindex = FindWadLevelInfo (mapname);
			if (levelindex == -1)
			{
				levelindex = wadlevelinfos.Reserve(1);
			}
			else
			{
				ClearLevelInfoStrings (&wadlevelinfos[levelindex]);
			}
			levelinfo = &wadlevelinfos[levelindex];
			memcpy (levelinfo, &defaultinfo, sizeof(*levelinfo));
			CopyString(levelinfo->music);
			CopyString(levelinfo->intermusic);
			CopyString(levelinfo->translator);
			CopyString(levelinfo->enterpic);
			CopyString(levelinfo->exitpic);
			CopyString(levelinfo->soundinfo);
			CopyString(levelinfo->sndseq);
			levelinfo->specialactions = CopySpecialActions(levelinfo->specialactions);
			levelinfo->opdata = CopyOptData(levelinfo->opdata);
			if (HexenHack)
			{
				levelinfo->WallHorizLight = levelinfo->WallVertLight = 0;
			}
			uppercopy (levelinfo->mapname, mapname);
			sc.MustGetString ();
			if (sc.String[0] == '$')
			{
				// For consistency with other definitions allow $Stringtablename here, too.
				levelflags |= LEVEL_LOOKUPLEVELNAME;
				ReplaceString (&levelinfo->level_name, sc.String + 1);
			}
			else
			{
				if (sc.Compare ("lookup"))
				{
					sc.MustGetString ();
					levelflags |= LEVEL_LOOKUPLEVELNAME;
				}
				ReplaceString (&levelinfo->level_name, sc.String);
			}
			// Set up levelnum now so that you can use Teleport_NewMap specials
			// to teleport to maps with standard names without needing a levelnum.
			if (!strnicmp (levelinfo->mapname, "MAP", 3) && levelinfo->mapname[5] == 0)
			{
				int mapnum = atoi (levelinfo->mapname + 3);

				if (mapnum >= 1 && mapnum <= 99)
					levelinfo->levelnum = mapnum;
			}
			else if (levelinfo->mapname[0] == 'E' &&
				levelinfo->mapname[1] >= '0' && levelinfo->mapname[1] <= '9' &&
				levelinfo->mapname[2] == 'M' &&
				levelinfo->mapname[3] >= '0' && levelinfo->mapname[3] <= '9')
			{
				int epinum = levelinfo->mapname[1] - '1';
				int mapnum = levelinfo->mapname[3] - '0';
				levelinfo->levelnum = epinum*10 + mapnum;
			}
			ParseMapInfoLower (sc, MapHandlers, MapInfoMapLevel, levelinfo, NULL, levelflags);
			// When the second sky is -NOFLAT-, make it a copy of the first sky
			if (strcmp (levelinfo->skypic2, "-NOFLAT-") == 0)
			{
				strcpy (levelinfo->skypic2, levelinfo->skypic1);
			}
			SetLevelNum (levelinfo, levelinfo->levelnum);	// Wipe out matching levelnums from other maps.
			if (levelinfo->pname[0] != 0)
			{
				if (!TexMan.AddPatch(levelinfo->pname).Exists())
				{
					levelinfo->pname[0] = 0;
				}
			}
			break;
		  }

		case MITL_CLUSTERDEF:	// clusterdef <clusternum>
			sc.MustGetNumber ();
			clusterindex = FindWadClusterInfo (sc.Number);
			if (clusterindex == -1)
			{
				clusterindex = wadclusterinfos.Reserve(1);
				clusterinfo = &wadclusterinfos[clusterindex];
			}
			else
			{
				clusterinfo = &wadclusterinfos[clusterindex];
				ClearClusterInfoStrings(clusterinfo);
			}
			memset (clusterinfo, 0, sizeof(cluster_info_t));
			clusterinfo->cluster = sc.Number;
			ParseMapInfoLower (sc, ClusterHandlers, MapInfoClusterLevel, NULL, clusterinfo, 0);
			break;

		case MITL_EPISODE:
			ParseEpisodeInfo(sc);
			break;

		case MITL_CLEAREPISODES:
			ClearEpisodes();
			break;

		case MITL_SKILL:
			ParseSkill(sc);
			break;

		case MITL_CLEARSKILLS:
			AllSkills.Clear();
			break;

		}
	}
	ClearLevelInfoStrings(&defaultinfo);
}

static void ClearEpisodes()
{
	for (int i = 0; i < EpiDef.numitems; ++i)
	{
		delete[] const_cast<char *>(EpisodeMenu[i].name);
		EpisodeMenu[i].name = NULL;
	}
	EpiDef.numitems = 0;
}

static void ParseMapInfoLower (FScanner &sc,
							   MapInfoHandler *handlers,
							   const char *strings[],
							   level_info_t *levelinfo,
							   cluster_info_t *clusterinfo,
							   QWORD flags)
{
	int entry;
	MapInfoHandler *handler;
	BYTE *info;

	info = levelinfo ? (BYTE *)levelinfo : (BYTE *)clusterinfo;

	while (sc.GetString ())
	{
		if (sc.MatchString (MapInfoTopLevel) != -1)
		{
			sc.UnGet ();
			break;
		}
		entry = sc.MatchString (strings);

		if (entry == -1)
		{
			FString keyword = sc.String;
			sc.MustGetString();
			if (levelinfo != NULL)
			{
				if (sc.Compare("{"))
				{
					ParseOptionalBlock(keyword, sc, levelinfo);
					continue;
				}
			}
			sc.ScriptError("Unknown keyword '%s'", keyword.GetChars());
		}

		handler = handlers + entry;
		switch (handler->type)
		{
		case MITYPE_EATNEXT:
			sc.MustGetString ();
			break;

		case MITYPE_IGNORE:
			break;

		case MITYPE_INT:
			sc.MustGetNumber ();
			*((int *)(info + handler->data1)) = sc.Number;
			break;

		case MITYPE_FLOAT:
			sc.MustGetFloat ();
			*((float *)(info + handler->data1)) = sc.Float;
			break;

		case MITYPE_HEX:
			sc.MustGetString ();
			*((int *)(info + handler->data1)) = strtoul (sc.String, NULL, 16);
			break;

		case MITYPE_COLOR:
			sc.MustGetString ();
			*((DWORD *)(info + handler->data1)) = V_GetColor (NULL, sc.String);
			break;

		case MITYPE_REDIRECT:
			sc.MustGetString ();
			levelinfo->RedirectType = sc.String;
			// Intentional fall-through

		case MITYPE_MAPNAME: {
			EndSequence newSeq;
			bool useseq = false;
			char maptemp[8];
			char *mapname;

			sc.MustGetString ();
			mapname = sc.String;
			if (IsNum (mapname))
			{
				int mapnum = atoi (mapname);

				if (HexenHack)
				{
					mysnprintf (maptemp, countof(maptemp), "&wt@%02d", mapnum);
				}
				else
				{
					mysnprintf (maptemp, countof(maptemp), "MAP%02d", mapnum);
				}
				mapname = maptemp;
			}
			if (stricmp (mapname, "endgame") == 0)
			{
				newSeq.Advanced = true;
				newSeq.EndType = END_Pic1;
				newSeq.PlayTheEnd = false;
				newSeq.MusicLooping = true;
				sc.MustGetStringName("{");
				while (!sc.CheckString("}"))
				{
					sc.MustGetString();
					if (sc.Compare("pic"))
					{
						sc.MustGetString();
						newSeq.EndType = END_Pic;
						newSeq.PicName = sc.String;
					}
					else if (sc.Compare("hscroll"))
					{
						newSeq.EndType = END_Bunny;
						sc.MustGetString();
						newSeq.PicName = sc.String;
						sc.MustGetString();
						newSeq.PicName2 = sc.String;
						if (sc.CheckNumber())
							newSeq.PlayTheEnd = !!sc.Number;
					}
					else if (sc.Compare("vscroll"))
					{
						newSeq.EndType = END_Demon;
						sc.MustGetString();
						newSeq.PicName = sc.String;
						sc.MustGetString();
						newSeq.PicName2 = sc.String;
					}
					else if (sc.Compare("cast"))
					{
						newSeq.EndType = END_Cast;
					}
					else if (sc.Compare("music"))
					{
						sc.MustGetString();
						newSeq.Music = sc.String;
						if (sc.CheckNumber())
						{
							newSeq.MusicLooping = !!sc.Number;
						}
					}
				}
				useseq = true;
			}
			else if (strnicmp (mapname, "EndGame", 7) == 0)
			{
				int type;
				switch (sc.String[7])
				{
				case '1':	type = END_Pic1;		break;
				case '2':	type = END_Pic2;		break;
				case '3':	type = END_Bunny;		break;
				case 'C':	type = END_Cast;		break;
				case 'W':	type = END_Underwater;	break;
				case 'S':	type = END_Strife;		break;
				default:	type = END_Pic3;		break;
				}
				newSeq.EndType = type;
				useseq = true;
			}
			else if (stricmp (mapname, "endpic") == 0)
			{
				sc.MustGetString ();
				newSeq.EndType = END_Pic;
				newSeq.PicName = sc.String;
				useseq = true;
			}
			else if (stricmp (mapname, "endbunny") == 0)
			{
				newSeq.EndType = END_Bunny;
				useseq = true;
			}
			else if (stricmp (mapname, "endcast") == 0)
			{
				newSeq.EndType = END_Cast;
				useseq = true;
			}
			else if (stricmp (mapname, "enddemon") == 0)
			{
				newSeq.EndType = END_Demon;
				useseq = true;
			}
			else if (stricmp (mapname, "endchess") == 0)
			{
				newSeq.EndType = END_Chess;
				useseq = true;
			}
			else if (stricmp (mapname, "endunderwater") == 0)
			{
				newSeq.EndType = END_Underwater;
				useseq = true;
			}
			else if (stricmp (mapname, "endbuystrife") == 0)
			{
				newSeq.EndType = END_BuyStrife;
				useseq = true;
			}
			else
			{
				strncpy ((char *)(info + handler->data1), mapname, 8);
			}
			if (useseq)
			{
				int seqnum = -1;
				
				if (!newSeq.Advanced)
				{
					seqnum = FindEndSequence (newSeq.EndType, newSeq.PicName);
				}

				if (seqnum == -1)
				{
					seqnum = (int)EndSequences.Push (newSeq);
				}
				strcpy ((char *)(info + handler->data1), "enDSeQ");
				*((WORD *)(info + handler->data1 + 6)) = (WORD)seqnum;
			}
			break;
		  }

		case MITYPE_LUMPNAME:
			sc.MustGetString ();
			uppercopy ((char *)(info + handler->data1), sc.String);
			flags |= handler->data2;
			break;

		case MITYPE_SKY:
			sc.MustGetString ();	// get texture name;
			uppercopy ((char *)(info + handler->data1), sc.String);
			sc.MustGetFloat ();		// get scroll speed
			if (HexenHack)
			{
				sc.Float /= 256;
			}
			// Sky scroll speed is specified as pixels per tic, but we
			// want pixels per millisecond.
			*((float *)(info + handler->data2)) = sc.Float * 35 / 1000;
			break;

		case MITYPE_SETFLAG:
			flags |= handler->data1;
			flags |= handler->data2;
			break;

		case MITYPE_CLRFLAG:
			flags &= ~handler->data1;
			flags |= handler->data2;
			break;

		case MITYPE_SCFLAGS:
			flags = (flags & handler->data2) | handler->data1;
			break;

		case MITYPE_CLUSTER:
			sc.MustGetNumber ();
			*((int *)(info + handler->data1)) = sc.Number;
			// If this cluster hasn't been defined yet, add it. This is especially needed
			// for Hexen, because it doesn't have clusterdefs. If we don't do this, every
			// level on Hexen will sometimes be considered as being on the same hub,
			// depending on the check done.
			if (FindWadClusterInfo (sc.Number) == -1)
			{
				unsigned int clusterindex = wadclusterinfos.Reserve(1);
				clusterinfo = &wadclusterinfos[clusterindex];
				memset (clusterinfo, 0, sizeof(cluster_info_t));
				clusterinfo->cluster = sc.Number;
				if (HexenHack)
				{
					clusterinfo->flags |= CLUSTER_HUB;
				}
			}
			break;

		case MITYPE_STRINGT:
			sc.MustGetString ();
			if (sc.String[0] == '$')
			{
				// For consistency with other definitions allow $Stringtablename here, too.
				flags |= handler->data2;
				ReplaceString ((char **)(info + handler->data1), sc.String+1);
			}
			else
			{
				if (sc.Compare ("lookup"))
				{
					flags |= handler->data2;
					sc.MustGetString ();
				}
				ReplaceString ((char **)(info + handler->data1), sc.String);
			}
			break;

		case MITYPE_STRING:
			sc.MustGetString();
			ReplaceString ((char **)(info + handler->data1), sc.String);
			break;

		case MITYPE_MUSIC:
			sc.MustGetString ();
			{
				char *colon = strchr (sc.String, ':');
				if (colon)
				{
					*colon = 0;
				}
				ReplaceString ((char **)(info + handler->data1), sc.String);
				*((int *)(info + handler->data2)) = colon ? atoi (colon + 1) : 0;
				if (levelinfo != NULL)
				{
					// Flag the level so that the $MAP command doesn't override this.
					flags|=LEVEL_MUSICDEFINED;
				}
			}
			break;

		case MITYPE_RELLIGHT:
			sc.MustGetNumber ();
			*((SBYTE *)(info + handler->data1)) = (SBYTE)clamp (sc.Number / 2, -128, 127);
			break;

		case MITYPE_CLRBYTES:
			*((BYTE *)(info + handler->data1)) = 0;
			*((BYTE *)(info + handler->data2)) = 0;
			break;

		case MITYPE_SPECIALACTION:
			{
				FSpecialAction **so = (FSpecialAction**)(info + handler->data1);
				FSpecialAction *sa = new FSpecialAction;
				int min_arg, max_arg;
				sa->Next = *so;
				*so = sa;
				sc.SetCMode(true);
				sc.MustGetString();
				sa->Type = FName(sc.String);
				sc.CheckString(",");
				sc.MustGetString();
				sa->Action = P_FindLineSpecial(sc.String, &min_arg, &max_arg);
				if (sa->Action == 0 || min_arg < 0)
				{
					sc.ScriptError("Unknown specialaction '%s'");
				}
				int j = 0;
				while (j < 5 && sc.CheckString(","))
				{
					sc.MustGetNumber();
					sa->Args[j++] = sc.Number;
				}
				/*
				if (j<min || j>max)
				{
					// Should be an error but can't for compatibility.
				}
				*/
				sc.SetCMode(false);
			}
			break;

		case MITYPE_COMPATFLAG:
			if (!sc.CheckNumber()) sc.Number = 1;

			if (levelinfo != NULL)
			{
				if (sc.Number) levelinfo->compatflags |= (DWORD)handler->data1;
				else levelinfo->compatflags &= ~ (DWORD)handler->data1;
				levelinfo->compatmask |= (DWORD)handler->data1;
			}
			break;
		}
	}
	if (levelinfo)
	{
		levelinfo->flags = flags;
	}
	else
	{
		clusterinfo->flags = flags;
	}
}

// Episode definitions start with the header "episode <start-map>"
// and then can be followed by any of the following:
//
// name "Episode name as text"
// picname "Picture to display the episode name"
// key "Shortcut key for the menu"
// noskillmenu
// remove

static void ParseEpisodeInfo (FScanner &sc)
{
	int i;
	char map[9];
	char *pic = NULL;
	bool picisgfx = false;	// Shut up, GCC!!!!
	bool remove = false;
	char key = 0;
	bool noskill = false;
	bool optional = false;

	// Get map name
	sc.MustGetString ();
	uppercopy (map, sc.String);
	map[8] = 0;

	sc.MustGetString ();
	if (sc.Compare ("teaser"))
	{
		sc.MustGetString ();
		if (gameinfo.flags & GI_SHAREWARE)
		{
			uppercopy (map, sc.String);
		}
		sc.MustGetString ();
	}
	do
	{
		if (sc.Compare ("optional"))
		{
			// For M4 in Doom and M4 and M5 in Heretic
			optional = true;
		}
		else if (sc.Compare ("name"))
		{
			sc.MustGetString ();
			ReplaceString (&pic, sc.String);
			picisgfx = false;
		}
		else if (sc.Compare ("picname"))
		{
			sc.MustGetString ();
			ReplaceString (&pic, sc.String);
			picisgfx = true;
		}
		else if (sc.Compare ("remove"))
		{
			remove = true;
		}
		else if (sc.Compare ("key"))
		{
			sc.MustGetString ();
			key = sc.String[0];
		}
		else if (sc.Compare("noskillmenu"))
		{
			noskill = true;
		}
		else
		{
			sc.UnGet ();
			break;
		}
	}
	while (sc.GetString ());

	if (optional && !remove)
	{
		if (!P_CheckMapData(map))
		{
			// If the episode is optional and the map does not exist
			// just ignore this episode definition.
			return;
		}
	}


	for (i = 0; i < EpiDef.numitems; ++i)
	{
		if (strncmp (EpisodeMaps[i], map, 8) == 0)
		{
			break;
		}
	}

	if (remove)
	{
		// If the remove property is given for an episode, remove it.
		if (i < EpiDef.numitems)
		{
			if (i+1 < EpiDef.numitems)
			{
				memmove (&EpisodeMaps[i], &EpisodeMaps[i+1],
					sizeof(EpisodeMaps[0])*(EpiDef.numitems - i - 1));
				memmove (&EpisodeMenu[i], &EpisodeMenu[i+1],
					sizeof(EpisodeMenu[0])*(EpiDef.numitems - i - 1));
				memmove (&EpisodeNoSkill[i], &EpisodeNoSkill[i+1], 
					sizeof(EpisodeNoSkill[0])*(EpiDef.numitems - i - 1));
			}
			EpiDef.numitems--;
		}
	}
	else
	{
		if (pic == NULL)
		{
			pic = copystring (map);
			picisgfx = false;
		}

		if (i == EpiDef.numitems)
		{
			if (EpiDef.numitems == MAX_EPISODES)
			{
				i = EpiDef.numitems - 1;
			}
			else
			{
				i = EpiDef.numitems++;
			}
		}
		else
		{
			delete[] const_cast<char *>(EpisodeMenu[i].name);
		}

		EpisodeMenu[i].name = pic;
		EpisodeMenu[i].alphaKey = tolower(key);
		EpisodeMenu[i].fulltext = !picisgfx;
		EpisodeNoSkill[i] = noskill;
		strncpy (EpisodeMaps[i], map, 8);
	}
}

static int FindEndSequence (int type, const char *picname)
{
	unsigned int i, num;

	num = EndSequences.Size ();
	for (i = 0; i < num; i++)
	{
		if (EndSequences[i].EndType == type && !EndSequences[i].Advanced &&
			(type != END_Pic || stricmp (EndSequences[i].PicName, picname) == 0))
		{
			return (int)i;
		}
	}
	return -1;
}

static void SetEndSequence (char *nextmap, int type)
{
	int seqnum;

	seqnum = FindEndSequence (type, NULL);
	if (seqnum == -1)
	{
		EndSequence newseq;
		newseq.EndType = type;
		seqnum = (int)EndSequences.Push (newseq);
	}
	strcpy (nextmap, "enDSeQ");
	*((WORD *)(nextmap + 6)) = (WORD)seqnum;
}

void G_SetForEndGame (char *nextmap)
{
	if (!strncmp(nextmap, "enDSeQ",6)) return;	// If there is already an end sequence please leave it alone!!!

	if (gameinfo.gametype == GAME_Strife)
	{
		SetEndSequence (nextmap, gameinfo.flags & GI_SHAREWARE ? END_BuyStrife : END_Strife);
	}
	else if (gameinfo.gametype == GAME_Hexen)
	{
		SetEndSequence (nextmap, END_Chess);
	}
	else if (gamemode == commercial)
	{
		SetEndSequence (nextmap, END_Cast);
	}
	else
	{ // The ExMx games actually have different ends based on the episode,
	  // but I want to keep this simple.
		SetEndSequence (nextmap, END_Pic1);
	}
}

void G_UnloadMapInfo ()
{
	unsigned int i;

	G_ClearSnapshots ();

	for (i = 0; i < wadlevelinfos.Size(); ++i)
	{
		ClearLevelInfoStrings (&wadlevelinfos[i]);
	}
	wadlevelinfos.Clear();

	for (i = 0; i < wadclusterinfos.Size(); ++i)
	{
		ClearClusterInfoStrings (&wadclusterinfos[i]);
	}
	wadclusterinfos.Clear();

	ClearEpisodes();
}

level_info_t *FindLevelByWarpTrans (int num)
{
	for (unsigned i = wadlevelinfos.Size(); i-- != 0; )
		if (wadlevelinfos[i].WarpTrans == num)
			return &wadlevelinfos[i];

	return NULL;
}

static void zapDefereds (acsdefered_t *def)
{
	while (def)
	{
		acsdefered_t *next = def->next;
		delete def;
		def = next;
	}
}

void P_RemoveDefereds (void)
{
	// Remove any existing defereds
	for (unsigned int i = 0; i < wadlevelinfos.Size(); i++)
	{
		if (wadlevelinfos[i].defered)
		{
			zapDefereds (wadlevelinfos[i].defered);
			wadlevelinfos[i].defered = NULL;
		}
	}
}

bool CheckWarpTransMap (FString &mapname, bool substitute)
{
	if (mapname[0] == '&' && (mapname[1] & 0xDF) == 'W' &&
		(mapname[2] & 0xDF) == 'T' && mapname[3] == '@')
	{
		level_info_t *lev = FindLevelByWarpTrans (atoi (&mapname[4]));
		if (lev != NULL)
		{
			mapname = lev->mapname;
			return true;
		}
		else if (substitute)
		{
			char a = mapname[4], b = mapname[5];
			mapname = "MAP";
			mapname << a << b;
		}
	}
	return false;
}

//
// G_InitNew
// Can be called by the startup code or the menu task,
// consoleplayer, playeringame[] should be set.
//
static FString d_mapname;
static int d_skill=-1;

void G_DeferedInitNew (const char *mapname, int newskill)
{
	d_mapname = mapname;
	d_skill = newskill;
	CheckWarpTransMap (d_mapname, true);
	gameaction = ga_newgame2;
}

CCMD (map)
{
	if (netgame)
	{
		Printf ("Use "TEXTCOLOR_BOLD"changemap"TEXTCOLOR_NORMAL" instead. "TEXTCOLOR_BOLD"Map"
				TEXTCOLOR_NORMAL" is for single-player only.\n");
		return;
	}
	if (argv.argc() > 1)
	{
		if (!P_CheckMapData(argv[1]))
		{
			Printf ("No map %s\n", argv[1]);
		}
		else
		{
			G_DeferedInitNew (argv[1]);
		}
	}
	else
	{
		Printf ("Usage: map <map name>\n");
	}
}

CCMD (open)
{
	if (netgame)
	{
		Printf ("You cannot use open in multiplayer games.\n");
		return;
	}
	if (argv.argc() > 1)
	{
		d_mapname = "file:";
		d_mapname += argv[1];
		if (!P_CheckMapData(d_mapname))
		{
			Printf ("No map %s\n", d_mapname.GetChars());
		}
		else
		{
			gameaction = ga_newgame2;
			d_skill = -1;
		}
	}
	else
	{
		Printf ("Usage: open <map file>\n");
	}
}


void G_NewInit ()
{
	int i;

	G_ClearSnapshots ();
	SB_state = screen->GetPageCount ();
	netgame = false;
	netdemo = false;
	multiplayer = false;
	if (demoplayback)
	{
		C_RestoreCVars ();
		demoplayback = false;
		D_SetupUserInfo ();
	}
	for (i = 0; i < MAXPLAYERS; ++i)
	{
		player_t *p = &players[i];
		userinfo_t saved_ui = players[i].userinfo;
		int chasecam = p->cheats & CF_CHASECAM;
		p->~player_t();
		::new(p) player_t;
		players[i].cheats |= chasecam;
		players[i].playerstate = PST_DEAD;
		playeringame[i] = 0;
		players[i].userinfo = saved_ui;
	}
	BackupSaveName = "";
	consoleplayer = 0;
	NextSkill = -1;
}

void G_DoNewGame (void)
{
	G_NewInit ();
	playeringame[consoleplayer] = 1;
	if (d_skill != -1)
	{
		gameskill = d_skill;
	}
	G_InitNew (d_mapname, false);
	gameaction = ga_nothing;
}

void G_InitNew (const char *mapname, bool bTitleLevel)
{
	EGameSpeed oldSpeed;
	bool wantFast;
	int i;

	if (!savegamerestore)
	{
		G_ClearSnapshots ();
		P_RemoveDefereds ();

		// [RH] Mark all levels as not visited
		for (unsigned int i = 0; i < wadlevelinfos.Size(); i++)
			wadlevelinfos[i].flags = wadlevelinfos[i].flags & ~LEVEL_VISITED;
	}

	UnlatchCVars ();
	G_VerifySkill();
	UnlatchCVars ();

	if (paused)
	{
		paused = 0;
		S_ResumeSound ();
	}

	if (StatusBar != NULL)
	{
		StatusBar->Destroy();
		StatusBar = NULL;
	}
	if (bTitleLevel)
	{
		StatusBar = new DBaseStatusBar (0);
	}
	else if (SBarInfoScript != NULL)
	{
		int cstype = SBarInfoScript->GetGameType();

		if(cstype == GAME_Doom) //Did the user specify a "base"
		{
			StatusBar = CreateDoomStatusBar ();
		}
		else if(cstype == GAME_Heretic)
		{
			StatusBar = CreateHereticStatusBar();
		}
		else if(cstype == GAME_Hexen)
		{
			StatusBar = CreateHexenStatusBar();
		}
		else if(cstype == GAME_Strife)
		{
			StatusBar = CreateStrifeStatusBar();
		}
		else //Use the default, empty or custom.
		{
			StatusBar = CreateCustomStatusBar();
		}
	}
	if (StatusBar == NULL)
	{
		if (gameinfo.gametype == GAME_Doom)
		{
			StatusBar = CreateDoomStatusBar ();
		}
		else if (gameinfo.gametype == GAME_Heretic)
		{
			StatusBar = CreateHereticStatusBar ();
		}
		else if (gameinfo.gametype == GAME_Hexen)
		{
			StatusBar = CreateHexenStatusBar ();
		}
		else if (gameinfo.gametype == GAME_Strife)
		{
			StatusBar = CreateStrifeStatusBar ();
		}
		else
		{
			StatusBar = new DBaseStatusBar (0);
		}
	}
	GC::WriteBarrier(StatusBar);
	StatusBar->AttachToPlayer (&players[consoleplayer]);
	StatusBar->NewGame ();
	setsizeneeded = true;

	if (gameinfo.gametype == GAME_Strife || (SBarInfoScript != NULL && SBarInfoScript->GetGameType() == GAME_Strife))
	{
		// Set the initial quest log text for Strife.
		for (i = 0; i < MAXPLAYERS; ++i)
		{
			players[i].SetLogText ("Find help");
		}
	}

	// [RH] If this map doesn't exist, bomb out
	if (!P_CheckMapData(mapname))
	{
		I_Error ("Could not find map %s\n", mapname);
	}

	oldSpeed = GameSpeed;
	wantFast = !!G_SkillProperty(SKILLP_FastMonsters);
	GameSpeed = wantFast ? SPEED_Fast : SPEED_Normal;

	if (oldSpeed != GameSpeed)
	{
		FActorInfo::StaticSpeedSet ();
	}

	if (!savegamerestore)
	{
		if (!netgame)
		{ // [RH] Change the random seed for each new single player game
			rngseed = rngseed*3/2;
		}
		FRandom::StaticClearRandom ();
		memset (ACS_WorldVars, 0, sizeof(ACS_WorldVars));
		memset (ACS_GlobalVars, 0, sizeof(ACS_GlobalVars));
		for (i = 0; i < NUM_WORLDVARS; ++i)
		{
			ACS_WorldArrays[i].Clear ();
		}
		for (i = 0; i < NUM_GLOBALVARS; ++i)
		{
			ACS_GlobalArrays[i].Clear ();
		}
		level.time = 0;
		level.maptime = 0;
		level.totaltime = 0;

		if (!multiplayer || !deathmatch)
		{
			InitPlayerClasses ();
		}

		// force players to be initialized upon first level load
		for (i = 0; i < MAXPLAYERS; i++)
			players[i].playerstate = PST_ENTER;	// [BC]

		STAT_NEW(mapname);
	}

	usergame = !bTitleLevel;		// will be set false if a demo
	paused = 0;
	demoplayback = false;
	automapactive = false;
	viewactive = true;
	BorderNeedRefresh = screen->GetPageCount ();

	//Added by MC: Initialize bots.
	if (!deathmatch)
	{
		bglobal.Init ();
	}

	if (mapname != level.mapname)
	{
		strcpy (level.mapname, mapname);
	}
	if (bTitleLevel)
	{
		gamestate = GS_TITLELEVEL;
	}
	else if (gamestate != GS_STARTUP)
	{
		gamestate = GS_LEVEL;
	}
	G_DoLoadLevel (0, false);
}

//
// G_DoCompleted
//
static char		nextlevel[9];
static int		startpos;	// [RH] Support for multiple starts per level
extern int		NoWipe;		// [RH] Don't wipe when travelling in hubs
static bool		startkeepfacing;	// [RH] Support for keeping your facing angle
static bool		resetinventory;	// Reset the inventory to the player's default for the next level
static bool		unloading;
static bool		g_nomonsters;

// [RH] The position parameter to these next three functions should
//		match the first parameter of the single player start spots
//		that should appear in the next map.

void G_ChangeLevel(const char * levelname, int position, bool keepFacing, int nextSkill, 
				   bool nointermission, bool resetinv, bool nomonsters)
{
	if (unloading)
	{
		Printf (TEXTCOLOR_RED "Unloading scripts cannot exit the level again.\n");
		return;
	}

	strncpy (nextlevel, levelname, 8);
	nextlevel[8] = 0;

	if (strncmp(nextlevel, "enDSeQ", 6))
	{
		level_info_t *nextinfo = CheckLevelRedirect (FindLevelInfo (nextlevel));
		if (nextinfo)
		{
			strncpy(nextlevel, nextinfo->mapname, 8);
		}
	}

	if (nextSkill != -1) NextSkill = nextSkill;

	g_nomonsters = nomonsters;

	if (nointermission) level.flags |= LEVEL_NOINTERMISSION;

	cluster_info_t *thiscluster = FindClusterInfo (level.cluster);
	cluster_info_t *nextcluster = FindClusterInfo (FindLevelInfo (nextlevel)->cluster);

	startpos = position;
	startkeepfacing = keepFacing;
	gameaction = ga_completed;
	resetinventory = resetinv;

	bglobal.End();	//Added by MC:

	// [RH] Give scripts a chance to do something
	unloading = true;
	FBehavior::StaticStartTypedScripts (SCRIPT_Unloading, NULL, false, 0, true);
	unloading = false;

	STAT_END(nextlevel);

	if (thiscluster && (thiscluster->flags & CLUSTER_HUB))
	{
		if ((level.flags & LEVEL_NOINTERMISSION) || (nextcluster == thiscluster))
			NoWipe = 35;
		D_DrawIcon = "TELEICON";
	}

	for(int i = 0; i < MAXPLAYERS; i++)
	{
		if (playeringame[i])
		{
			player_t *player = &players[i];

			// Un-crouch all players here.
			player->Uncrouch();

			// If this is co-op, respawn any dead players now so they can
			// keep their inventory on the next map.
			if (multiplayer && !deathmatch && player->playerstate == PST_DEAD)
			{
				// Copied from the end of P_DeathThink [[
				player->cls = NULL;		// Force a new class if the player is using a random class
				player->playerstate = PST_REBORN;
				if (player->mo->special1 > 2)
				{
					player->mo->special1 = 0;
				}
				// ]]
				G_DoReborn(i, false);
			}
		}
	}
}

const char *G_GetExitMap()
{
	return level.nextmap;
}

const char *G_GetSecretExitMap()
{
	const char *nextmap = level.nextmap;

	if (level.secretmap[0] != 0)
	{
		if (P_CheckMapData(level.secretmap))
		{
			nextmap = level.secretmap;
		}
	}
	return nextmap;
}

void G_ExitLevel (int position, bool keepFacing)
{
	G_ChangeLevel(G_GetExitMap(), position, keepFacing);
}

void G_SecretExitLevel (int position) 
{
	G_ChangeLevel(G_GetSecretExitMap(), position, false);
}

void G_DoCompleted (void)
{
	int i; 

	gl_DeleteAllAttachedLights();

	gameaction = ga_nothing;

	if (gamestate == GS_TITLELEVEL)
	{
		strncpy (level.mapname, nextlevel, 8);
		G_DoLoadLevel (startpos, false);
		startpos = 0;
		viewactive = true;
		return;
	}

	// [RH] Mark this level as having been visited
	if (!(level.flags & LEVEL_CHANGEMAPCHEAT))
		FindLevelInfo (level.mapname)->flags |= LEVEL_VISITED;

	if (automapactive)
		AM_Stop ();

	wminfo.finished_ep = level.cluster - 1;
	wminfo.lname0 = level.info->pname;
	wminfo.current = level.mapname;

	if (deathmatch &&
		(dmflags & DF_SAME_LEVEL) &&
		!(level.flags & LEVEL_CHANGEMAPCHEAT))
	{
		wminfo.next = level.mapname;
		wminfo.lname1 = level.info->pname;
	}
	else
	{
		if (strncmp (nextlevel, "enDSeQ", 6) == 0)
		{
			wminfo.next = FString(nextlevel, 8);
			wminfo.lname1 = "";
		}
		else
		{
			level_info_t *nextinfo = FindLevelInfo (nextlevel);
			wminfo.next = nextinfo->mapname;
			wminfo.lname1 = nextinfo->pname;
		}
	}

	CheckWarpTransMap (wminfo.next, true);

	wminfo.next_ep = FindLevelInfo (nextlevel)->cluster - 1;
	wminfo.maxkills = level.total_monsters;
	wminfo.maxitems = level.total_items;
	wminfo.maxsecret = level.total_secrets;
	wminfo.maxfrags = 0;
	wminfo.partime = TICRATE * level.partime;
	wminfo.sucktime = level.sucktime;
	wminfo.pnum = consoleplayer;
	wminfo.totaltime = level.totaltime;

	for (i=0 ; i<MAXPLAYERS ; i++)
	{
		wminfo.plyr[i].in = playeringame[i];
		wminfo.plyr[i].skills = players[i].killcount;
		wminfo.plyr[i].sitems = players[i].itemcount;
		wminfo.plyr[i].ssecret = players[i].secretcount;
		wminfo.plyr[i].stime = level.time;
		memcpy (wminfo.plyr[i].frags, players[i].frags
				, sizeof(wminfo.plyr[i].frags));
		wminfo.plyr[i].fragcount = players[i].fragcount;
	}

	// [RH] If we're in a hub and staying within that hub, take a snapshot
	//		of the level. If we're traveling to a new hub, take stuff from
	//		the player and clear the world vars. If this is just an
	//		ordinary cluster (not a hub), take stuff from the player, but
	//		leave the world vars alone.
	cluster_info_t *thiscluster = FindClusterInfo (level.cluster);
	cluster_info_t *nextcluster = FindClusterInfo (wminfo.next_ep+1);	// next_ep is cluster-1
	EFinishLevelType mode;

	if (thiscluster != nextcluster || deathmatch ||
		!(thiscluster->flags & CLUSTER_HUB))
	{
		if (nextcluster->flags & CLUSTER_HUB)
		{
			mode = FINISH_NextHub;
		}
		else
		{
			mode = FINISH_NoHub;
		}
	}
	else
	{
		mode = FINISH_SameHub;
	}

	// Intermission stats for entire hubs
	G_LeavingHub(mode, thiscluster, &wminfo);

	for (i = 0; i < MAXPLAYERS; i++)
	{
		if (playeringame[i])
		{ // take away appropriate inventory
			G_PlayerFinishLevel (i, mode, resetinventory);
		}
	}

	if (mode == FINISH_SameHub)
	{ // Remember the level's state for re-entry.
		G_SnapshotLevel ();
	}
	else
	{ // Forget the states of all existing levels.
		G_ClearSnapshots ();

		if (mode == FINISH_NextHub)
		{ // Reset world variables for the new hub.
			memset (ACS_WorldVars, 0, sizeof(ACS_WorldVars));
			for (i = 0; i < NUM_WORLDVARS; ++i)
			{
				ACS_WorldArrays[i].Clear ();
			}
		}
		// With hub statistics the time should be per hub.
		// Additionally there is a global time counter now so nothing is missed by changing it
		//else if (mode == FINISH_NoHub)
		{ // Reset time to zero if not entering/staying in a hub.
			level.time = 0;
		}
		level.maptime = 0;
	}

	if (!deathmatch &&
		((level.flags & LEVEL_NOINTERMISSION) ||
		((nextcluster == thiscluster) && (thiscluster->flags & CLUSTER_HUB))))
	{
		G_WorldDone ();
		return;
	}

	gamestate = GS_INTERMISSION;
	viewactive = false;
	automapactive = false;

// [RH] If you ever get a statistics driver operational, adapt this.
//	if (statcopy)
//		memcpy (statcopy, &wminfo, sizeof(wminfo));

	WI_Start (&wminfo);
}

class DAutosaver : public DThinker
{
	DECLARE_CLASS (DAutosaver, DThinker)
public:
	void Tick ();
};

IMPLEMENT_CLASS (DAutosaver)

void DAutosaver::Tick ()
{
	Net_WriteByte (DEM_CHECKAUTOSAVE);
	Destroy ();
}

//
// G_DoLoadLevel 
//
extern gamestate_t 	wipegamestate; 
 
void G_DoLoadLevel (int position, bool autosave)
{ 
	static int lastposition = 0;
	gamestate_t oldgs = gamestate;
	int i;

	if (NextSkill >= 0)
	{
		UCVarValue val;
		val.Int = NextSkill;
		gameskill.ForceSet (val, CVAR_Int);
		NextSkill = -1;
	}

	if (position == -1)
		position = lastposition;
	else
		lastposition = position;

	G_InitLevelLocals ();
	StatusBar->DetachAllMessages ();

	// Force 'teamplay' to 'true' if need be.
	if (level.flags & LEVEL_FORCETEAMPLAYON)
		teamplay = true;

	// Force 'teamplay' to 'false' if need be.
	if (level.flags & LEVEL_FORCETEAMPLAYOFF)
		teamplay = false;

	Printf (
			"\n\35\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36"
			"\36\36\36\36\36\36\36\36\36\36\36\36\37\n\n"
			TEXTCOLOR_BOLD "%s - %s\n\n",
			level.mapname, level.level_name);

	if (wipegamestate == GS_LEVEL)
		wipegamestate = GS_FORCEWIPE;

	if (gamestate != GS_TITLELEVEL)
	{
		gamestate = GS_LEVEL; 
	}

	// Set the sky map.
	// First thing, we have a dummy sky texture name,
	//	a flat. The data is in the WAD only because
	//	we look for an actual index, instead of simply
	//	setting one.
	skyflatnum = TexMan.GetTexture (gameinfo.SkyFlatName, FTexture::TEX_Flat, FTextureManager::TEXMAN_Overridable);

	// DOOM determines the sky texture to be used
	// depending on the current episode and the game version.
	// [RH] Fetch sky parameters from FLevelLocals.
	sky1texture = TexMan.GetTexture (level.skypic1, FTexture::TEX_Wall, FTextureManager::TEXMAN_Overridable|FTextureManager::TEXMAN_ReturnFirst);
	sky2texture = TexMan.GetTexture (level.skypic2, FTexture::TEX_Wall, FTextureManager::TEXMAN_Overridable|FTextureManager::TEXMAN_ReturnFirst);

	// [RH] Set up details about sky rendering
	R_InitSkyMap ();

	for (i = 0; i < MAXPLAYERS; i++)
	{ 
		if (playeringame[i] && (deathmatch || players[i].playerstate == PST_DEAD))
			players[i].playerstate = PST_ENTER;	// [BC]
		memset (players[i].frags,0,sizeof(players[i].frags));
		if (!(dmflags2 & DF2_YES_KEEPFRAGS) && (alwaysapplydmflags || deathmatch))
			players[i].fragcount = 0;
	}

	if (g_nomonsters)
	{
		level.flags |= LEVEL_NOMONSTERS;
	}
	else
	{
		level.flags &= ~LEVEL_NOMONSTERS;
	}

	P_SetupLevel (level.mapname, position);
	AM_LevelInit();

	// [RH] Start lightning, if MAPINFO tells us to
	if (level.flags & LEVEL_STARTLIGHTNING)
	{
		P_StartLightning ();
	}

	gameaction = ga_nothing; 

	// clear cmd building stuff
	ResetButtonStates ();

	SendItemUse = NULL;
	SendItemDrop = NULL;
	mousex = mousey = 0; 
	sendpause = sendsave = sendturn180 = SendLand = false;
	LocalViewAngle = 0;
	LocalViewPitch = 0;
	paused = 0;

	//Added by MC: Initialize bots.
	if (deathmatch)
	{
		bglobal.Init ();
	}

	if (timingdemo)
	{
		static bool firstTime = true;

		if (firstTime)
		{
			starttime = I_GetTime (false);
			firstTime = false;
		}
	}

	level.starttime = gametic;
	level.maptime = 0;
	G_UnSnapshotLevel (!savegamerestore);	// [RH] Restore the state of the level.
	G_FinishTravel ();
	if (players[consoleplayer].camera == NULL ||
		players[consoleplayer].camera->player != NULL)
	{ // If we are viewing through a player, make sure it is us.
        players[consoleplayer].camera = players[consoleplayer].mo;
	}
	StatusBar->AttachToPlayer (&players[consoleplayer]);
	P_DoDeferedScripts ();	// [RH] Do script actions that were triggered on another map.
	
	if (demoplayback || oldgs == GS_STARTUP || oldgs == GS_TITLELEVEL)
		C_HideConsole ();

	C_FlushDisplay ();

	// [RH] Always save the game when entering a new level.
	if (autosave && !savegamerestore && disableautosave < 1)
	{
		DAutosaver GCCNOWARN *dummy = new DAutosaver;
	}
}


//
// G_WorldDone 
//
void G_WorldDone (void) 
{ 
	cluster_info_t *nextcluster;
	cluster_info_t *thiscluster;

	gameaction = ga_worlddone; 

	if (level.flags & LEVEL_CHANGEMAPCHEAT)
		return;

	thiscluster = FindClusterInfo (level.cluster);

	if (strncmp (nextlevel, "enDSeQ", 6) == 0)
	{
		F_StartFinale (thiscluster->messagemusic, thiscluster->musicorder,
			thiscluster->cdtrack, thiscluster->cdid,
			thiscluster->finaleflat, thiscluster->exittext,
			thiscluster->flags & CLUSTER_EXITTEXTINLUMP,
			thiscluster->flags & CLUSTER_FINALEPIC,
			thiscluster->flags & CLUSTER_LOOKUPEXITTEXT,
			true);
	}
	else
	{
		nextcluster = FindClusterInfo (FindLevelInfo (nextlevel)->cluster);

		if (nextcluster->cluster != level.cluster && !deathmatch)
		{
			// Only start the finale if the next level's cluster is different
			// than the current one and we're not in deathmatch.
			if (nextcluster->entertext)
			{
				F_StartFinale (nextcluster->messagemusic, nextcluster->musicorder,
					nextcluster->cdtrack, nextcluster->cdid,
					nextcluster->finaleflat, nextcluster->entertext,
					nextcluster->flags & CLUSTER_ENTERTEXTINLUMP,
					nextcluster->flags & CLUSTER_FINALEPIC,
					nextcluster->flags & CLUSTER_LOOKUPENTERTEXT,
					false);
			}
			else if (thiscluster->exittext)
			{
				F_StartFinale (thiscluster->messagemusic, thiscluster->musicorder,
					thiscluster->cdtrack, nextcluster->cdid,
					thiscluster->finaleflat, thiscluster->exittext,
					thiscluster->flags & CLUSTER_EXITTEXTINLUMP,
					thiscluster->flags & CLUSTER_FINALEPIC,
					thiscluster->flags & CLUSTER_LOOKUPEXITTEXT,
					false);
			}
		}
	}
} 
 
void G_DoWorldDone (void) 
{		 
	gamestate = GS_LEVEL;
	if (wminfo.next[0] == 0)
	{
		// Don't crash if no next map is given. Just repeat the current one.
		Printf ("No next map specified.\n");
	}
	else
	{
		strncpy (level.mapname, nextlevel, 8);
	}
	G_StartTravel ();
	G_DoLoadLevel (startpos, true);
	startpos = 0;
	gameaction = ga_nothing;
	viewactive = true; 
}

//==========================================================================
//
// G_StartTravel
//
// Moves players (and eventually their inventory) to a different statnum,
// so they will not be destroyed when switching levels. This only applies
// to real players, not voodoo dolls.
//
//==========================================================================

void G_StartTravel ()
{
	if (deathmatch)
		return;

	for (int i = 0; i < MAXPLAYERS; ++i)
	{
		if (playeringame[i])
		{
			AActor *pawn = players[i].mo;
			AInventory *inv;

			// Only living players travel. Dead ones get a new body on the new level.
			if (players[i].health > 0)
			{
				pawn->UnlinkFromWorld ();
				P_DelSector_List ();
				int tid = pawn->tid;	// Save TID
				pawn->RemoveFromHash ();
				pawn->tid = tid;		// Restore TID (but no longer linked into the hash chain)
				pawn->ChangeStatNum (STAT_TRAVELLING);

				for (inv = pawn->Inventory; inv != NULL; inv = inv->Inventory)
				{
					inv->ChangeStatNum (STAT_TRAVELLING);
					inv->UnlinkFromWorld ();
					P_DelSector_List ();
				}
			}
		}
	}
}

//==========================================================================
//
// G_FinishTravel
//
// Moves any travelling players so that they occupy their newly-spawned
// copies' locations, destroying the new players in the process (because
// they are really fake placeholders to show where the travelling players
// should go).
//
//==========================================================================

void G_FinishTravel ()
{
	TThinkerIterator<APlayerPawn> it (STAT_TRAVELLING);
	APlayerPawn *pawn, *pawndup, *oldpawn, *next;
	AInventory *inv;

	next = it.Next ();
	while ( (pawn = next) != NULL)
	{
		next = it.Next ();
		pawn->ChangeStatNum (STAT_PLAYER);
		pawndup = pawn->player->mo;
		assert (pawn != pawndup);
		if (pawndup == NULL)
		{ // Oh no! there was no start for this player!
			pawn->flags |= MF_NOSECTOR|MF_NOBLOCKMAP;
			pawn->Destroy ();
		}
		else
		{
			oldpawn = pawndup;

			// The player being spawned here is a short lived dummy and
			// must not start any ENTER script or big problems will happen.
			pawndup = P_SpawnPlayer (&playerstarts[pawn->player - players], true);
			if (!startkeepfacing)
			{
				pawn->angle = pawndup->angle;
				pawn->pitch = pawndup->pitch;
			}
			pawn->x = pawndup->x;
			pawn->y = pawndup->y;
			pawn->z = pawndup->z;
			pawn->momx = pawndup->momx;
			pawn->momy = pawndup->momy;
			pawn->momz = pawndup->momz;
			pawn->Sector = pawndup->Sector;
			pawn->floorz = pawndup->floorz;
			pawn->ceilingz = pawndup->ceilingz;
			pawn->dropoffz = pawndup->dropoffz;
			pawn->floorsector = pawndup->floorsector;
			pawn->floorpic = pawndup->floorpic;
			pawn->ceilingsector = pawndup->ceilingsector;
			pawn->ceilingpic = pawndup->ceilingpic;
			pawn->floorclip = pawndup->floorclip;
			pawn->waterlevel = pawndup->waterlevel;
			pawn->target = NULL;
			pawn->lastenemy = NULL;
			pawn->player->mo = pawn;
			DObject::StaticPointerSubstitution (oldpawn, pawn);
			oldpawn->Destroy();
			pawndup->Destroy ();
			pawn->LinkToWorld ();
			pawn->AddToHash ();
			pawn->dynamiclights.Clear();	// remove all dynamic lights from the previous level
			pawn->SetState(pawn->SpawnState);

			for (inv = pawn->Inventory; inv != NULL; inv = inv->Inventory)
			{
				inv->ChangeStatNum (STAT_INVENTORY);
				inv->LinkToWorld ();
				inv->Travelled ();
				inv->dynamiclights.Clear();	// remove all dynamic lights from the previous level
			}
			if (level.FromSnapshot)
			{
				FBehavior::StaticStartTypedScripts (SCRIPT_Return, pawn, true);
			}
		}
	}
}
 
void G_InitLevelLocals ()
{
	level_info_t *info;

	BaseBlendA = 0.0f;		// Remove underwater blend effect, if any
	NormalLight.Maps = realcolormaps;

	// [BB] Instead of just setting the color, we also have to reset Desaturate and build the lights.
	NormalLight.ChangeColor (PalEntry (255, 255, 255), 0);

	level.gravity = sv_gravity * 35/TICRATE;
	level.aircontrol = (fixed_t)(sv_aircontrol * 65536.f);
	level.teamdamage = teamdamage;
	level.flags = 0;

	info = FindLevelInfo (level.mapname);

	level.info = info;
	level.skyspeed1 = info->skyspeed1;
	level.skyspeed2 = info->skyspeed2;
	info = (level_info_t *)info;
	strncpy (level.skypic2, info->skypic2, 8);
	level.fadeto = info->fadeto;
	level.cdtrack = info->cdtrack;
	level.cdid = info->cdid;
	level.FromSnapshot = false;
	if (level.fadeto == 0)
	{
		R_SetDefaultColormap (info->fadetable);
		if (strnicmp (info->fadetable, "COLORMAP", 8) != 0)
		{
			level.flags |= LEVEL_HASFADETABLE;
		}
		/*
	}
	else
	{
		NormalLight.ChangeFade (level.fadeto);
		*/
	}
	level.airsupply = info->airsupply*TICRATE;
	level.outsidefog = info->outsidefog;
	level.WallVertLight = info->WallVertLight*2;
	level.WallHorizLight = info->WallHorizLight*2;
	if (info->gravity != 0.f)
	{
		level.gravity = info->gravity * 35/TICRATE;
	}
	if (info->aircontrol != 0.f)
	{
		level.aircontrol = (fixed_t)(info->aircontrol * 65536.f);
	}
	if (info->teamdamage != 0.f)
	{
		level.teamdamage = info->teamdamage;
	}

	G_AirControlChanged ();

	if (info->level_name)
	{
		cluster_info_t *clus = FindClusterInfo (info->cluster);

		level.partime = info->partime;
		level.sucktime = info->sucktime;
		level.cluster = info->cluster;
		level.clusterflags = clus ? clus->flags : 0;
		level.flags |= info->flags;
		level.levelnum = info->levelnum;
		level.music = info->music;
		level.musicorder = info->musicorder;

		strncpy (level.level_name, info->level_name, 63);
		G_MaybeLookupLevelName (NULL);
		strncpy (level.nextmap, info->nextmap, 8);
		level.nextmap[8] = 0;
		strncpy (level.secretmap, info->secretmap, 8);
		level.secretmap[8] = 0;
		strncpy (level.skypic1, info->skypic1, 8);
		level.skypic1[8] = 0;
		if (!level.skypic2[0])
			strncpy (level.skypic2, level.skypic1, 8);
		level.skypic2[8] = 0;
	}
	else
	{
		level.partime = level.cluster = 0;
		level.sucktime = 0;
		strcpy (level.level_name, "Unnamed");
		level.nextmap[0] =
			level.secretmap[0] = 0;
		level.music = NULL;
		strcpy (level.skypic1, "SKY1");
		strcpy (level.skypic2, "SKY1");
		level.flags = 0;
		level.levelnum = 1;
	}

	compatflags.Callback();

	NormalLight.ChangeFade (level.fadeto);
}

bool FLevelLocals::IsJumpingAllowed() const
{
	if (dmflags & DF_NO_JUMP)
		return false;
	if (dmflags & DF_YES_JUMP)
		return true;
	return !(level.flags & LEVEL_JUMP_NO);
}

bool FLevelLocals::IsCrouchingAllowed() const
{
	if (dmflags & DF_NO_CROUCH)
		return false;
	if (dmflags & DF_YES_CROUCH)
		return true;
	return !(level.flags & LEVEL_CROUCH_NO);
}

bool FLevelLocals::IsFreelookAllowed() const
{
	if (level.flags & LEVEL_FREELOOK_NO)
		return false;
	if (level.flags & LEVEL_FREELOOK_YES)
		return true;
	return !(dmflags & DF_NO_FREELOOK);
}

FString CalcMapName (int episode, int level)
{
	FString lumpname;

	if (gameinfo.flags & GI_MAPxx)
	{
		lumpname.Format("MAP%02d", level);
	}
	else
	{
		lumpname = "";
		lumpname << 'E' << ('0' + episode) << 'M' << ('0' + level);
	}
	return lumpname;
}

level_info_t *FindLevelInfo (const char *mapname)
{
	int i;

	if ((i = FindWadLevelInfo (mapname)) > -1)
		return &wadlevelinfos[i];
	else
		return &TheDefaultLevelInfo;
}

level_info_t *FindLevelByNum (int num)
{
	for (unsigned int i = 0; i < wadlevelinfos.Size(); i++)
		if (wadlevelinfos[i].levelnum == num)
			return &wadlevelinfos[i];

	return NULL;
}

level_info_t *CheckLevelRedirect (level_info_t *info)
{
	if (info->RedirectType != NAME_None)
	{
		const PClass *type = PClass::FindClass(info->RedirectType);
		if (type != NULL)
		{
			for (int i = 0; i < MAXPLAYERS; ++i)
			{
				if (playeringame[i] && players[i].mo->FindInventory (type))
				{
					// check for actual presence of the map.
					if (P_CheckMapData(info->RedirectMap))
					{
						return FindLevelInfo(info->RedirectMap);
					}
					break;
				}
			}
		}
	}
	return NULL;
}

static void SetLevelNum (level_info_t *info, int num)
{
	// Avoid duplicate levelnums. The level being set always has precedence.
	for (unsigned int i = 0; i < wadlevelinfos.Size(); ++i)
	{
		if (wadlevelinfos[i].levelnum == num)
			wadlevelinfos[i].levelnum = 0;
	}
	info->levelnum = num;
}

cluster_info_t *FindClusterInfo (int cluster)
{
	int i;

	if ((i = FindWadClusterInfo (cluster)) > -1)
		return &wadclusterinfos[i];
	else
		return &TheDefaultClusterInfo;
}

const char *G_MaybeLookupLevelName (level_info_t *ininfo)
{
	level_info_t *info;

	if (ininfo == NULL)
	{
		info = level.info;
	}
	else
	{
		info = ininfo;
	}

	if (info != NULL && info->flags & LEVEL_LOOKUPLEVELNAME)
	{
		const char *thename;
		const char *lookedup;

		lookedup = GStrings[info->level_name];
		if (lookedup == NULL)
		{
			thename = info->level_name;
		}
		else
		{
			char checkstring[32];

			// Strip out the header from the localized string
			if (info->mapname[0] == 'E' && info->mapname[2] == 'M')
			{
				mysnprintf (checkstring, countof(checkstring), "%s: ", info->mapname);
			}
			else if (info->mapname[0] == 'M' && info->mapname[1] == 'A' && info->mapname[2] == 'P')
			{
				mysnprintf (checkstring, countof(checkstring), "%d: ", atoi(info->mapname + 3));
			}
			thename = strstr (lookedup, checkstring);
			if (thename == NULL)
			{
				thename = lookedup;
			}
			else
			{
				thename += strlen (checkstring);
			}
		}
		if (ininfo == NULL)
		{
			strncpy (level.level_name, thename, 63);
		}
		return thename;
	}
	return info != NULL ? info->level_name : NULL;
}

void G_AirControlChanged ()
{
	if (level.aircontrol <= 256)
	{
		level.airfriction = FRACUNIT;
	}
	else
	{
		// Friction is inversely proportional to the amount of control
		float fric = ((float)level.aircontrol/65536.f) * -0.0941f + 1.0004f;
		level.airfriction = (fixed_t)(fric * 65536.f);
	}
}

void G_SerializeLevel (FArchive &arc, bool hubLoad)
{
	int i = level.totaltime;
	
	gl_DeleteAllAttachedLights();

	arc << level.flags
		<< level.fadeto
		<< level.found_secrets
		<< level.found_items
		<< level.killed_monsters
		<< level.gravity
		<< level.aircontrol
		<< level.teamdamage
		<< level.maptime
		<< i;

	// Hub transitions must keep the current total time
	if (!hubLoad)
		level.totaltime=i;

	if (arc.IsStoring ())
	{
		arc.WriteName (level.skypic1);
		arc.WriteName (level.skypic2);
	}
	else
	{
		strncpy (level.skypic1, arc.ReadName(), 8);
		strncpy (level.skypic2, arc.ReadName(), 8);
		sky1texture = TexMan.GetTexture (level.skypic1, FTexture::TEX_Wall, FTextureManager::TEXMAN_Overridable);
		sky2texture = TexMan.GetTexture (level.skypic2, FTexture::TEX_Wall, FTextureManager::TEXMAN_Overridable);
		R_InitSkyMap ();
	}

	G_AirControlChanged ();

	BYTE t;

	// Does this level have scrollers?
	if (arc.IsStoring ())
	{
		t = level.Scrolls ? 1 : 0;
		arc << t;
	}
	else
	{
		arc << t;
		if (level.Scrolls)
		{
			delete[] level.Scrolls;
			level.Scrolls = NULL;
		}
		if (t)
		{
			level.Scrolls = new FSectorScrollValues[numsectors];
			memset (level.Scrolls, 0, sizeof(level.Scrolls)*numsectors);
		}
	}

	FBehavior::StaticSerializeModuleStates (arc);
	if (arc.IsLoading()) interpolator.ClearInterpolations();
	P_SerializeThinkers (arc, hubLoad);
	P_SerializeWorld (arc);
	P_SerializePolyobjs (arc);
	StatusBar->Serialize (arc);
	//SerializeInterpolations (arc);

	arc << level.total_monsters << level.total_items << level.total_secrets;

	// Does this level have custom translations?
	FRemapTable *trans;
	WORD w;
	if (arc.IsStoring ())
	{
		for (unsigned int i = 0; i < translationtables[TRANSLATION_LevelScripted].Size(); ++i)
		{
			trans = translationtables[TRANSLATION_LevelScripted][i];
			if (trans != NULL && !trans->IsIdentity())
			{
				w = WORD(i);
				arc << w;
				trans->Serialize(arc);
			}
		}
		w = 0xffff;
		arc << w;
	}
	else
	{
		while (arc << w, w != 0xffff)
		{
			trans = translationtables[TRANSLATION_LevelScripted].GetVal(w);
			if (trans == NULL)
			{
				trans = new FRemapTable;
				translationtables[TRANSLATION_LevelScripted].SetVal(w, trans);
			}
			trans->Serialize(arc);
		}
	}

	// This must be saved, too, of course!
	FCanvasTextureInfo::Serialize (arc);
	AM_SerializeMarkers(arc);

	P_SerializePlayers (arc, hubLoad);
	P_SerializeSounds (arc);

	STAT_SAVE(arc, hubLoad);
	if (arc.IsLoading()) for(i=0;i<numsectors;i++)
	{
		P_Recalculate3DFloors(&sectors[i]);
	}
	gl_RecreateAllAttachedLights();
}

// Archives the current level
void G_SnapshotLevel ()
{
	if (level.info->snapshot)
		delete level.info->snapshot;

	if (level.info->mapname[0] != 0 || level.info == &TheDefaultLevelInfo)
	{
		level.info->snapshotVer = SAVEVER;
		level.info->snapshot = new FCompressedMemFile;
		level.info->snapshot->Open ();

		FArchive arc (*level.info->snapshot);

		SaveVersion = SAVEVER;
		G_SerializeLevel (arc, false);
	}
}

// Unarchives the current level based on its snapshot
// The level should have already been loaded and setup.
void G_UnSnapshotLevel (bool hubLoad)
{
	if (level.info->snapshot == NULL)
		return;

	if (level.info->mapname[0] != 0 || level.info == &TheDefaultLevelInfo)
	{
		SaveVersion = level.info->snapshotVer;
		level.info->snapshot->Reopen ();
		FArchive arc (*level.info->snapshot);
		if (hubLoad)
			arc.SetHubTravel ();
		G_SerializeLevel (arc, hubLoad);
		arc.Close ();
		level.FromSnapshot = true;

		TThinkerIterator<APlayerPawn> it;
		APlayerPawn *pawn, *next;

		next = it.Next();
		while ((pawn = next) != 0)
		{
			next = it.Next();
			if (pawn->player == NULL || pawn->player->mo == NULL || !playeringame[pawn->player - players])
			{
				int i;

				// If this isn't the unmorphed original copy of a player, destroy it, because it's extra.
				for (i = 0; i < MAXPLAYERS; ++i)
				{
					if (playeringame[i] && players[i].morphTics && players[i].mo->tracer == pawn)
					{
						break;
					}
				}
				if (i == MAXPLAYERS)
				{
					pawn->Destroy ();
				}
			}
		}
	}
	// No reason to keep the snapshot around once the level's been entered.
	delete level.info->snapshot;
	level.info->snapshot = NULL;
}

void G_ClearSnapshots (void)
{
	for (unsigned int i = 0; i < wadlevelinfos.Size(); i++)
	{
		if (wadlevelinfos[i].snapshot)
		{
			delete wadlevelinfos[i].snapshot;
			wadlevelinfos[i].snapshot = NULL;
		}
	}
}

static void writeMapName (FArchive &arc, const char *name)
{
	BYTE size;
	if (name[7] != 0)
	{
		size = 8;
	}
	else
	{
		size = (BYTE)strlen (name);
	}
	arc << size;
	arc.Write (name, size);
}

static void writeSnapShot (FArchive &arc, level_info_t *i)
{
	arc << i->snapshotVer;
	writeMapName (arc, i->mapname);
	i->snapshot->Serialize (arc);
}

void G_WriteSnapshots (FILE *file)
{
	unsigned int i;

	for (i = 0; i < wadlevelinfos.Size(); i++)
	{
		if (wadlevelinfos[i].snapshot)
		{
			FPNGChunkArchive arc (file, SNAP_ID);
			writeSnapShot (arc, (level_info_t *)&wadlevelinfos[i]);
		}
	}
	if (TheDefaultLevelInfo.snapshot != NULL)
	{
		FPNGChunkArchive arc (file, DSNP_ID);
		writeSnapShot(arc, &TheDefaultLevelInfo);
	}

	FPNGChunkArchive *arc = NULL;
	
	// Write out which levels have been visited
	for (i = 0; i < wadlevelinfos.Size(); ++i)
	{
		if (wadlevelinfos[i].flags & LEVEL_VISITED)
		{
			if (arc == NULL)
			{
				arc = new FPNGChunkArchive (file, VIST_ID);
			}
			writeMapName (*arc, wadlevelinfos[i].mapname);
		}
	}

	if (arc != NULL)
	{
		BYTE zero = 0;
		*arc << zero;
		delete arc;
	}

	// Store player classes to be used when spawning a random class
	if (multiplayer)
	{
		FPNGChunkArchive arc2 (file, RCLS_ID);
		for (i = 0; i < MAXPLAYERS; ++i)
		{
			SBYTE cnum = SinglePlayerClass[i];
			arc2 << cnum;
		}
	}

	// Store player classes that are currently in use
	FPNGChunkArchive arc3 (file, PCLS_ID);
	for (i = 0; i < MAXPLAYERS; ++i)
	{
		BYTE pnum;
		if (playeringame[i])
		{
			pnum = i;
			arc3 << pnum;
			arc3.UserWriteClass (players[i].cls);
		}
		pnum = 255;
		arc3 << pnum;
	}
}

void G_ReadSnapshots (PNGHandle *png)
{
	DWORD chunkLen;
	BYTE namelen;
	char mapname[256];
	level_info_t *i;

	G_ClearSnapshots ();

	chunkLen = (DWORD)M_FindPNGChunk (png, SNAP_ID);
	while (chunkLen != 0)
	{
		FPNGChunkArchive arc (png->File->GetFile(), SNAP_ID, chunkLen);
		DWORD snapver;

		arc << snapver;
		arc << namelen;
		arc.Read (mapname, namelen);
		mapname[namelen] = 0;
		i = FindLevelInfo (mapname);
		i->snapshotVer = snapver;
		i->snapshot = new FCompressedMemFile;
		i->snapshot->Serialize (arc);
		chunkLen = (DWORD)M_NextPNGChunk (png, SNAP_ID);
	}

	chunkLen = (DWORD)M_FindPNGChunk (png, DSNP_ID);
	if (chunkLen != 0)
	{
		FPNGChunkArchive arc (png->File->GetFile(), DSNP_ID, chunkLen);
		DWORD snapver;

		arc << snapver;
		arc << namelen;
		arc.Read (mapname, namelen);
		TheDefaultLevelInfo.snapshotVer = snapver;
		TheDefaultLevelInfo.snapshot = new FCompressedMemFile;
		TheDefaultLevelInfo.snapshot->Serialize (arc);
	}

	chunkLen = (DWORD)M_FindPNGChunk (png, VIST_ID);
	if (chunkLen != 0)
	{
		FPNGChunkArchive arc (png->File->GetFile(), VIST_ID, chunkLen);

		arc << namelen;
		while (namelen != 0)
		{
			arc.Read (mapname, namelen);
			mapname[namelen] = 0;
			i = FindLevelInfo (mapname);
			i->flags |= LEVEL_VISITED;
			arc << namelen;
		}
	}

	chunkLen = (DWORD)M_FindPNGChunk (png, RCLS_ID);
	if (chunkLen != 0)
	{
		FPNGChunkArchive arc (png->File->GetFile(), PCLS_ID, chunkLen);
		SBYTE cnum;

		for (DWORD j = 0; j < chunkLen; ++j)
		{
			arc << cnum;
			SinglePlayerClass[j] = cnum;
		}
	}

	chunkLen = (DWORD)M_FindPNGChunk (png, PCLS_ID);
	if (chunkLen != 0)
	{
		FPNGChunkArchive arc (png->File->GetFile(), RCLS_ID, chunkLen);
		BYTE pnum;

		arc << pnum;
		while (pnum != 255)
		{
			arc.UserReadClass (players[pnum].cls);
			arc << pnum;
		}
	}
	png->File->ResetFilePtr();
}


static void writeDefereds (FArchive &arc, level_info_t *i)
{
	writeMapName (arc, i->mapname);
	arc << i->defered;
}

void P_WriteACSDefereds (FILE *file)
{
	FPNGChunkArchive *arc = NULL;

	for (unsigned int i = 0; i < wadlevelinfos.Size(); i++)
	{
		if (wadlevelinfos[i].defered)
		{
			if (arc == NULL)
			{
				arc = new FPNGChunkArchive (file, ACSD_ID);
			}
			writeDefereds (*arc, (level_info_t *)&wadlevelinfos[i]);
		}
	}

	if (arc != NULL)
	{
		// Signal end of defereds
		BYTE zero = 0;
		*arc << zero;
		delete arc;
	}
}

void P_ReadACSDefereds (PNGHandle *png)
{
	BYTE namelen;
	char mapname[256];
	size_t chunklen;

	P_RemoveDefereds ();

	if ((chunklen = M_FindPNGChunk (png, ACSD_ID)) != 0)
	{
		FPNGChunkArchive arc (png->File->GetFile(), ACSD_ID, chunklen);

		arc << namelen;
		while (namelen)
		{
			arc.Read (mapname, namelen);
			mapname[namelen] = 0;
			level_info_t *i = FindLevelInfo (mapname);
			if (i == NULL)
			{
				I_Error ("Unknown map '%s' in savegame", mapname);
			}
			arc << i->defered;
			arc << namelen;
		}
	}
	png->File->ResetFilePtr();
}


void FLevelLocals::Tick ()
{
	// Reset carry sectors
	if (Scrolls != NULL)
	{
		memset (Scrolls, 0, sizeof(*Scrolls)*numsectors);
	}
}

void FLevelLocals::AddScroller (DScroller *scroller, int secnum)
{
	if (secnum < 0)
	{
		return;
	}
	if (Scrolls == NULL)
	{
		Scrolls = new FSectorScrollValues[numsectors];
		memset (Scrolls, 0, sizeof(*Scrolls)*numsectors);
	}
}

// Initializes player classes in case they are random.
// This gets called at the start of a new game, and the classes
// chosen here are used for the remainder of a single-player
// or coop game. These are ignored for deathmatch.

static void InitPlayerClasses ()
{
	if (!savegamerestore)
	{
		for (int i = 0; i < MAXPLAYERS; ++i)
		{
			SinglePlayerClass[i] = players[i].userinfo.PlayerClass;
			if (SinglePlayerClass[i] < 0 || !playeringame[i])
			{
				SinglePlayerClass[i] = (pr_classchoice()) % PlayerClasses.Size ();
			}
			players[i].cls = NULL;
			players[i].CurrentPlayerClass = SinglePlayerClass[i];
		}
	}
}


static void ParseSkill (FScanner &sc)
{
	FSkillInfo skill;

	skill.AmmoFactor = FRACUNIT;
	skill.DoubleAmmoFactor = 2*FRACUNIT;
	skill.DamageFactor = FRACUNIT;
	skill.FastMonsters = false;
	skill.DisableCheats = false;
	skill.EasyBossBrain = false;
	skill.AutoUseHealth = false;
	skill.RespawnCounter = 0;
	skill.RespawnLimit = 0;
	skill.Aggressiveness = FRACUNIT;
	skill.SpawnFilter = 0;
	skill.ACSReturn = AllSkills.Size();
	skill.MenuNameIsLump = false;
	skill.MustConfirm = false;
	skill.Shortcut = 0;
	skill.TextColor = "";

	sc.MustGetString();
	skill.Name = sc.String;

	while (sc.GetString ())
	{
		if (sc.Compare ("ammofactor"))
		{
			sc.MustGetFloat ();
			skill.AmmoFactor = FLOAT2FIXED(sc.Float);
		}
		else if (sc.Compare ("doubleammofactor"))
		{
			sc.MustGetFloat ();
			skill.DoubleAmmoFactor = FLOAT2FIXED(sc.Float);
		}
		else if (sc.Compare ("damagefactor"))
		{
			sc.MustGetFloat ();
			skill.DamageFactor = FLOAT2FIXED(sc.Float);
		}
		else if (sc.Compare ("fastmonsters"))
		{
			skill.FastMonsters = true;
		}
		else if (sc.Compare ("disablecheats"))
		{
			skill.DisableCheats = true;
		}
		else if (sc.Compare ("easybossbrain"))
		{
			skill.EasyBossBrain = true;
		}
		else if (sc.Compare("autousehealth"))
		{
			skill.AutoUseHealth = true;
		}
		else if (sc.Compare("respawntime"))
		{
			sc.MustGetFloat ();
			skill.RespawnCounter = int(sc.Float*TICRATE);
		}
		else if (sc.Compare("respawnlimit"))
		{
			sc.MustGetNumber ();
			skill.RespawnLimit = sc.Number;
		}
		else if (sc.Compare("Aggressiveness"))
		{
			sc.MustGetFloat ();
			skill.Aggressiveness = FRACUNIT - FLOAT2FIXED(clamp<float>(sc.Float, 0,1));
		}
		else if (sc.Compare("SpawnFilter"))
		{
			if (sc.CheckNumber())
			{
				if (sc.Number > 0) skill.SpawnFilter |= (1<<(sc.Number-1));
			}
			else
			{
				sc.MustGetString ();
				if (sc.Compare("baby")) skill.SpawnFilter |= 1;
				else if (sc.Compare("easy")) skill.SpawnFilter |= 2;
				else if (sc.Compare("normal")) skill.SpawnFilter |= 4;
				else if (sc.Compare("hard")) skill.SpawnFilter |= 8;
				else if (sc.Compare("nightmare")) skill.SpawnFilter |= 16;
			}
		}
		else if (sc.Compare("ACSReturn"))
		{
			sc.MustGetNumber ();
			skill.ACSReturn = sc.Number;
		}
		else if (sc.Compare("Name"))
		{
			sc.MustGetString ();
			skill.MenuName = sc.String;
			skill.MenuNameIsLump = false;
		}
		else if (sc.Compare("PlayerClassName"))
		{
			sc.MustGetString ();
			FName pc = sc.String;
			sc.MustGetString ();
			skill.MenuNamesForPlayerClass[pc]=sc.String;
		}
		else if (sc.Compare("PicName"))
		{
			sc.MustGetString ();
			skill.MenuName = sc.String;
			skill.MenuNameIsLump = true;
		}
		else if (sc.Compare("MustConfirm"))
		{
			skill.MustConfirm = true;
			if (sc.CheckToken(TK_StringConst))
			{
				skill.MustConfirmText = sc.String;
			}
		}
		else if (sc.Compare("Key"))
		{
			sc.MustGetString();
			skill.Shortcut = tolower(sc.String[0]);
		}
		else if (sc.Compare("TextColor"))
		{
			sc.MustGetString();
			skill.TextColor = '[';
			skill.TextColor << sc.String << ']';
		}
		else
		{
			sc.UnGet ();
			break;
		}
	}
	for(unsigned int i = 0; i < AllSkills.Size(); i++)
	{
		if (AllSkills[i].Name == skill.Name)
		{
			AllSkills[i] = skill;
			return;
		}
	}
	AllSkills.Push(skill);
}

int G_SkillProperty(ESkillProperty prop)
{
	if (AllSkills.Size() > 0)
	{
		switch(prop)
		{
		case SKILLP_AmmoFactor:
			if (dmflags2 & DF2_YES_DOUBLEAMMO)
			{
				return AllSkills[gameskill].DoubleAmmoFactor;
			}
			return AllSkills[gameskill].AmmoFactor;

		case SKILLP_DamageFactor:
			return AllSkills[gameskill].DamageFactor;

		case SKILLP_FastMonsters:
			return AllSkills[gameskill].FastMonsters  || (dmflags & DF_FAST_MONSTERS);

		case SKILLP_Respawn:
			if (dmflags & DF_MONSTERS_RESPAWN && AllSkills[gameskill].RespawnCounter==0) 
				return TICRATE * (gameinfo.gametype != GAME_Strife ? 12 : 16);
			return AllSkills[gameskill].RespawnCounter;

		case SKILLP_RespawnLimit:
			return AllSkills[gameskill].RespawnLimit;

		case SKILLP_Aggressiveness:
			return AllSkills[gameskill].Aggressiveness;

		case SKILLP_DisableCheats:
			return AllSkills[gameskill].DisableCheats;

		case SKILLP_AutoUseHealth:
			return AllSkills[gameskill].AutoUseHealth;

		case SKILLP_EasyBossBrain:
			return AllSkills[gameskill].EasyBossBrain;

		case SKILLP_SpawnFilter:
			return AllSkills[gameskill].SpawnFilter;

		case SKILLP_ACSReturn:
			return AllSkills[gameskill].ACSReturn;
		}
	}
	return 0;
}


void G_VerifySkill()
{
	if (gameskill >= (int)AllSkills.Size())
		gameskill = AllSkills.Size()-1;
	else if (gameskill < 0)
		gameskill = 0;
}

FSkillInfo &FSkillInfo::operator=(const FSkillInfo &other)
{
	Name = other.Name;
	AmmoFactor = other.AmmoFactor;
	DoubleAmmoFactor = other.DoubleAmmoFactor;
	DamageFactor = other.DamageFactor;
	FastMonsters = other.FastMonsters;
	DisableCheats = other.DisableCheats;
	AutoUseHealth = other.AutoUseHealth;
	EasyBossBrain = other.EasyBossBrain;
	RespawnCounter= other.RespawnCounter;
	RespawnLimit= other.RespawnLimit;
	Aggressiveness= other.Aggressiveness;
	SpawnFilter = other.SpawnFilter;
	ACSReturn = other.ACSReturn;
	MenuName = other.MenuName;
	MenuNamesForPlayerClass = other.MenuNamesForPlayerClass;
	MenuNameIsLump = other.MenuNameIsLump;
	MustConfirm = other.MustConfirm;
	MustConfirmText = other.MustConfirmText;
	Shortcut = other.Shortcut;
	TextColor = other.TextColor;
	return *this;
}

int FSkillInfo::GetTextColor() const
{
	if (TextColor.IsEmpty())
	{
		return CR_UNTRANSLATED;
	}
	const BYTE *cp = (const BYTE *)TextColor.GetChars();
	int color = V_ParseFontColor(cp, 0, 0);
	if (color == CR_UNDEFINED)
	{
		Printf("Undefined color '%s' in definition of skill %s\n", TextColor.GetChars(), Name.GetChars());
		color = CR_UNTRANSLATED;
	}
	return color;
}

CCMD(listmaps)
{
	for(unsigned i = 0; i < wadlevelinfos.Size(); i++)
	{
		level_info_t *info = &wadlevelinfos[i];

		if (P_CheckMapData(info->mapname))
		{
			Printf("%s: '%s'\n", info->mapname, G_MaybeLookupLevelName(info));
		}
	}
}
