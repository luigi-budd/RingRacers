// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2020 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  d_clisrv.c
/// \brief SRB2 Network game communication and protocol, all OS independent parts.

#include <time.h>
#ifdef __GNUC__
#include <unistd.h> //for unlink
#endif

#include "i_time.h"
#include "i_net.h"
#include "i_system.h"
#include "i_video.h"
#include "d_net.h"
#include "d_netfil.h" // fileneedednum
#include "d_main.h"
#include "g_game.h"
#include "st_stuff.h"
#include "hu_stuff.h"
#include "keys.h"
#include "g_input.h" // JOY1
#include "k_menu.h"
#include "console.h"
#include "d_netfil.h"
#include "byteptr.h"
#include "p_saveg.h"
#include "z_zone.h"
#include "p_local.h"
#include "m_misc.h"
#include "am_map.h"
#include "m_random.h"
#include "mserv.h"
#include "y_inter.h"
#include "r_local.h"
#include "m_argv.h"
#include "p_setup.h"
#include "lzf.h"
#include "lua_script.h"
#include "lua_hook.h"
#include "md5.h"
#include "m_perfstats.h"
#include "monocypher/monocypher.h"
#include "stun.h"

// SRB2Kart
#include "k_kart.h"
#include "k_battle.h"
#include "k_pwrlv.h"
#include "k_bot.h"
#include "k_grandprix.h"
#include "doomstat.h"
#include "s_sound.h" // sfx_syfail
#include "m_cond.h" // netUnlocked

// cl loading screen
#include "v_video.h"
#include "f_finale.h"

#ifdef HAVE_DISCORDRPC
#include "discord.h"
#endif

//
// NETWORKING
//
// gametic is the tic about to (or currently being) run
// Server:
//   maketic is the tic that hasn't had control made for it yet
//   nettics is the tic for each node
//   firstticstosend is the lowest value of nettics
// Client:
//   neededtic is the tic needed by the client to run the game
//   firstticstosend is used to optimize a condition
// Normally maketic >= gametic > 0

#define MAX_REASONLENGTH 30
#define FORCECLOSE 0x8000

boolean server = true; // true or false but !server == client
#define client (!server)
boolean nodownload = false;
boolean serverrunning = false;
INT32 serverplayer = 0;
char motd[254], server_context[8]; // Message of the Day, Unique Context (even without Mumble support)

UINT8 playerconsole[MAXPLAYERS];

// Server specific vars
UINT8 playernode[MAXPLAYERS];

// Minimum timeout for sending the savegame
// The actual timeout will be longer depending on the savegame length
tic_t jointimeout = (3*TICRATE);
static boolean sendingsavegame[MAXNETNODES]; // Are we sending the savegame?
static boolean resendingsavegame[MAXNETNODES]; // Are we resending the savegame?
static tic_t savegameresendcooldown[MAXNETNODES]; // How long before we can resend again?
static tic_t freezetimeout[MAXNETNODES]; // Until when can this node freeze the server before getting a timeout?

// Incremented by cv_joindelay when a client joins, decremented each tic.
// If higher than cv_joindelay * 2 (3 joins in a short timespan), joins are temporarily disabled.
static tic_t joindelay = 0;

UINT16 pingmeasurecount = 1;
UINT32 realpingtable[MAXPLAYERS]; //the base table of ping where an average will be sent to everyone.
UINT32 playerpingtable[MAXPLAYERS]; //table of player latency values.

static tic_t lowest_lag;
boolean server_lagless;
static CV_PossibleValue_t mindelay_cons_t[] = {{0, "MIN"}, {30, "MAX"}, {0, NULL}};
consvar_t cv_mindelay = CVAR_INIT ("mindelay", "2", CV_SAVE, mindelay_cons_t, NULL);

SINT8 nodetoplayer[MAXNETNODES];
SINT8 nodetoplayer2[MAXNETNODES]; // say the numplayer for this node if any (splitscreen)
SINT8 nodetoplayer3[MAXNETNODES]; // say the numplayer for this node if any (splitscreen == 2)
SINT8 nodetoplayer4[MAXNETNODES]; // say the numplayer for this node if any (splitscreen == 3)
UINT8 playerpernode[MAXNETNODES]; // used specialy for splitscreen
boolean nodeingame[MAXNETNODES]; // set false as nodes leave game
boolean nodeneedsauth[MAXNETNODES];

tic_t servermaxping = 20; // server's max delay, in frames. Defaults to 20
static tic_t nettics[MAXNETNODES]; // what tic the client have received
static tic_t supposedtics[MAXNETNODES]; // nettics prevision for smaller packet
static UINT8 nodewaiting[MAXNETNODES];
static tic_t firstticstosend; // min of the nettics
static tic_t tictoclear = 0; // optimize d_clearticcmd
static tic_t maketic;

static INT16 consistancy[BACKUPTICS];

static UINT8 player_joining = false;
UINT8 hu_redownloadinggamestate = 0;

// kart, true when a player is connecting or disconnecting so that the gameplay has stopped in its tracks
boolean hu_stopped = false;

UINT8 adminpassmd5[16];
boolean adminpasswordset = false;

// Client specific
static ticcmd_t localcmds[MAXSPLITSCREENPLAYERS][MAXGENTLEMENDELAY];
static boolean cl_packetmissed;
// here it is for the secondary local player (splitscreen)
static UINT8 mynode; // my address pointofview server
static boolean cl_redownloadinggamestate = false;

static UINT8 localtextcmd[MAXSPLITSCREENPLAYERS][MAXTEXTCMD];
static tic_t neededtic;
SINT8 servernode = 0; // the number of the server node
char connectedservername[MAXSERVERNAME];
/// \brief do we accept new players?
/// \todo WORK!
boolean acceptnewnode = true;

UINT32 ourIP; // Used when populating PT_SERVERCHALLENGE (guards against signature reuse)
uint8_t lastReceivedKey[MAXNETNODES][MAXSPLITSCREENPLAYERS][32]; // Player's public key (join process only! active players have it on player_t)
uint8_t lastSentChallenge[MAXNETNODES][32]; // The random message we asked them to sign in PT_SERVERCHALLENGE, check it in PT_CLIENTJOIN
uint8_t lastChallengeAll[64]; // The message we asked EVERYONE to sign for client-to-client identity proofs
uint8_t lastReceivedSignature[MAXPLAYERS][64]; // Everyone's response to lastChallengeAll
uint8_t knownWhenChallenged[MAXPLAYERS][32]; // Everyone a client saw at the moment a challenge should be initiated
boolean expectChallenge = false; // Were we in-game before a client-to-client challenge should have been sent?

uint8_t priorKeys[MAXPLAYERS][32]; // Make a note of keys before consuming a new gamestate, and if the server tries to send us a gamestate where keys differ, assume shenanigans

boolean serverisfull = false; //lets us be aware if the server was full after we check files, but before downloading, so we can ask if the user still wants to download or not
tic_t firstconnectattempttime = 0;

uint8_t awaitingChallenge[32];
consvar_t cv_allowguests = CVAR_INIT ("allowguests", "On", CV_SAVE, CV_OnOff, NULL);

#ifdef DEVELOP
	consvar_t cv_badjoin = CVAR_INIT ("badjoin", "0", 0, CV_Unsigned, NULL);
	consvar_t cv_badtraffic = CVAR_INIT ("badtraffic", "0", 0, CV_Unsigned, NULL);
	consvar_t cv_badresponse = CVAR_INIT ("badresponse", "0", 0, CV_Unsigned, NULL);
	consvar_t cv_noresponse = CVAR_INIT ("noresponse", "0", 0, CV_Unsigned, NULL);
	consvar_t cv_nochallenge = CVAR_INIT ("nochallenge", "0", 0, CV_Unsigned, NULL);
	consvar_t cv_badresults = CVAR_INIT ("badresults", "0", 0, CV_Unsigned, NULL);
	consvar_t cv_noresults = CVAR_INIT ("noresults", "0", 0, CV_Unsigned, NULL);
	consvar_t cv_badtime = CVAR_INIT ("badtime", "0", 0, CV_Unsigned, NULL);
	consvar_t cv_badip = CVAR_INIT ("badip", "0", 0, CV_Unsigned, NULL);
#endif

// engine

// Must be a power of two
#define TEXTCMD_HASH_SIZE 4

typedef struct textcmdplayer_s
{
	INT32 playernum;
	UINT8 cmd[MAXTEXTCMD];
	struct textcmdplayer_s *next;
} textcmdplayer_t;

typedef struct textcmdtic_s
{
	tic_t tic;
	textcmdplayer_t *playercmds[TEXTCMD_HASH_SIZE];
	struct textcmdtic_s *next;
} textcmdtic_t;

ticcmd_t netcmds[BACKUPTICS][MAXPLAYERS];
static textcmdtic_t *textcmds[TEXTCMD_HASH_SIZE] = {NULL};


consvar_t cv_showjoinaddress = CVAR_INIT ("showjoinaddress", "Off", CV_SAVE|CV_NETVAR, CV_OnOff, NULL);

static CV_PossibleValue_t playbackspeed_cons_t[] = {{1, "MIN"}, {10, "MAX"}, {0, NULL}};
consvar_t cv_playbackspeed = CVAR_INIT ("playbackspeed", "1", 0, playbackspeed_cons_t, NULL);

consvar_t cv_httpsource = CVAR_INIT ("http_source", "", CV_SAVE, NULL, NULL);

consvar_t cv_kicktime = CVAR_INIT ("kicktime", "10", CV_SAVE, CV_Unsigned, NULL);

// https://github.com/jameds/holepunch/blob/master/holepunch.c#L75
static int IsExternalAddress (const void *p)
{
	const int a = ((const unsigned char*)p)[0];
	const int b = ((const unsigned char*)p)[1];

	if (*(const int*)p == ~0)/* 255.255.255.255 */
		return 0;

	switch (a)
	{
		case 0:
		case 10:
		case 127:
			return 0;
		case 172:
			return (b & ~15) != 16;/* 16 - 31 */
		case 192:
			return b != 168;
		default:
			return 1;
	}
}


// Generate a message for an authenticating client to sign, with some guarantees about who we are.
void GenerateChallenge(uint8_t *buf)
{
	time_t now = time(NULL);
	csprng(buf, sizeof(&buf)); // Random noise as a baseline, but...
	memcpy(buf, &now, sizeof(now)); // Timestamp limits the reuse window.
	memcpy(buf + sizeof(now), &ourIP, sizeof(ourIP)); // IP prevents captured signatures from being used elsewhere.

	#ifdef DEVELOP
		if (cv_badtime.value)
		{
			CV_AddValue(&cv_badtime, -1);
			CONS_Alert(CONS_WARNING, "cv_badtime enabled, trashing time in auth message\n");
			memset(buf, 0, sizeof(now));
		}

		if (cv_badip.value)
		{
			CV_AddValue(&cv_badip, -1);
			CONS_Alert(CONS_WARNING, "cv_badip enabled, trashing IP in auth message\n");
			memset(buf + sizeof(now), 0, sizeof(ourIP));
		}
	#endif
}

// Modified servers can throw softballs or reuse challenges.
// Don't sign anything that wasn't generated just for us!
shouldsign_t ShouldSignChallenge(uint8_t *message)
{
	time_t then, now;
	UINT32 claimedIP, realIP;

	now = time(NULL);
	memcpy(&then, message, sizeof(then));
	memcpy(&claimedIP, message + sizeof(then), sizeof(claimedIP));
	realIP = *I_GetNodeAddressInt(servernode);

	if (abs(now - then) > 60*5)
		return SIGN_BADTIME;

	if (realIP != claimedIP && IsExternalAddress(&realIP))
		return SIGN_BADIP;

	return SIGN_OK;
}

static inline void *G_DcpyTiccmd(void* dest, const ticcmd_t* src, const size_t n)
{
	const size_t d = n / sizeof(ticcmd_t);
	const size_t r = n % sizeof(ticcmd_t);
	UINT8 *ret = dest;

	if (r)
		M_Memcpy(dest, src, n);
	else if (d)
		G_MoveTiccmd(dest, src, d);
	return ret+n;
}

static inline void *G_ScpyTiccmd(ticcmd_t* dest, void* src, const size_t n)
{
	const size_t d = n / sizeof(ticcmd_t);
	const size_t r = n % sizeof(ticcmd_t);
	UINT8 *ret = src;

	if (r)
		M_Memcpy(dest, src, n);
	else if (d)
		G_MoveTiccmd(dest, src, d);
	return ret+n;
}



// Some software don't support largest packet
// (original sersetup, not exactely, but the probability of sending a packet
// of 512 bytes is like 0.1)
UINT16 software_MAXPACKETLENGTH;

/** Guesses the full value of a tic from its lowest byte, for a specific node
  *
  * \param low The lowest byte of the tic value
  * \param basetic The last full tic value to compare against
  * \return The full tic value
  *
  */
tic_t ExpandTics(INT32 low, tic_t basetic)
{
	INT32 delta;

	delta = low - (basetic & UINT8_MAX);

	if (delta >= -64 && delta <= 64)
		return (basetic & ~UINT8_MAX) + low;
	else if (delta > 64)
		return (basetic & ~UINT8_MAX) - 256 + low;
	else //if (delta < -64)
		return (basetic & ~UINT8_MAX) + 256 + low;
}

// -----------------------------------------------------------------
// Some extra data function for handle textcmd buffer
// -----------------------------------------------------------------

static void (*listnetxcmd[MAXNETXCMD])(UINT8 **p, INT32 playernum);

void RegisterNetXCmd(netxcmd_t id, void (*cmd_f)(UINT8 **p, INT32 playernum))
{
#ifdef PARANOIA
	if (id >= MAXNETXCMD)
		I_Error("Command id %d too big", id);
	if (listnetxcmd[id] != 0)
		I_Error("Command id %d already used", id);
#endif
	listnetxcmd[id] = cmd_f;
}

void SendNetXCmdForPlayer(UINT8 playerid, netxcmd_t id, const void *param, size_t nparam)
{
	if (localtextcmd[playerid][0]+2+nparam > MAXTEXTCMD)
	{
		// for future reference: if (cht_debug) != debug disabled.
		CONS_Alert(CONS_ERROR, M_GetText("NetXCmd buffer full, cannot add netcmd %d! (size: %d, needed: %s)\n"), id, localtextcmd[playerid][0], sizeu1(nparam));
		return;
	}

	localtextcmd[playerid][0]++;
	localtextcmd[playerid][localtextcmd[playerid][0]] = (UINT8)id;

	if (param && nparam)
	{
		M_Memcpy(&localtextcmd[playerid][localtextcmd[playerid][0] + 1], param, nparam);
		localtextcmd[playerid][0] = (UINT8)(localtextcmd[playerid][0] + (UINT8)nparam);
	}
}

UINT8 GetFreeXCmdSize(UINT8 playerid)
{
	// -1 for the size and another -1 for the ID.
	return (UINT8)(localtextcmd[playerid][0] - 2);
}

// Frees all textcmd memory for the specified tic
static void D_FreeTextcmd(tic_t tic)
{
	textcmdtic_t **tctprev = &textcmds[tic & (TEXTCMD_HASH_SIZE - 1)];
	textcmdtic_t *textcmdtic = *tctprev;

	while (textcmdtic && textcmdtic->tic != tic)
	{
		tctprev = &textcmdtic->next;
		textcmdtic = textcmdtic->next;
	}

	if (textcmdtic)
	{
		INT32 i;

		// Remove this tic from the list.
		*tctprev = textcmdtic->next;

		// Free all players.
		for (i = 0; i < TEXTCMD_HASH_SIZE; i++)
		{
			textcmdplayer_t *textcmdplayer = textcmdtic->playercmds[i];

			while (textcmdplayer)
			{
				textcmdplayer_t *tcpnext = textcmdplayer->next;
				Z_Free(textcmdplayer);
				textcmdplayer = tcpnext;
			}
		}

		// Free this tic's own memory.
		Z_Free(textcmdtic);
	}
}

// Gets the buffer for the specified ticcmd, or NULL if there isn't one
static UINT8* D_GetExistingTextcmd(tic_t tic, INT32 playernum)
{
	textcmdtic_t *textcmdtic = textcmds[tic & (TEXTCMD_HASH_SIZE - 1)];
	while (textcmdtic && textcmdtic->tic != tic) textcmdtic = textcmdtic->next;

	// Do we have an entry for the tic? If so, look for player.
	if (textcmdtic)
	{
		textcmdplayer_t *textcmdplayer = textcmdtic->playercmds[playernum & (TEXTCMD_HASH_SIZE - 1)];
		while (textcmdplayer && textcmdplayer->playernum != playernum) textcmdplayer = textcmdplayer->next;

		if (textcmdplayer) return textcmdplayer->cmd;
	}

	return NULL;
}

// Gets the buffer for the specified ticcmd, creating one if necessary
static UINT8* D_GetTextcmd(tic_t tic, INT32 playernum)
{
	textcmdtic_t *textcmdtic = textcmds[tic & (TEXTCMD_HASH_SIZE - 1)];
	textcmdtic_t **tctprev = &textcmds[tic & (TEXTCMD_HASH_SIZE - 1)];
	textcmdplayer_t *textcmdplayer, **tcpprev;

	// Look for the tic.
	while (textcmdtic && textcmdtic->tic != tic)
	{
		tctprev = &textcmdtic->next;
		textcmdtic = textcmdtic->next;
	}

	// If we don't have an entry for the tic, make it.
	if (!textcmdtic)
	{
		textcmdtic = *tctprev = Z_Calloc(sizeof (textcmdtic_t), PU_STATIC, NULL);
		textcmdtic->tic = tic;
	}

	tcpprev = &textcmdtic->playercmds[playernum & (TEXTCMD_HASH_SIZE - 1)];
	textcmdplayer = *tcpprev;

	// Look for the player.
	while (textcmdplayer && textcmdplayer->playernum != playernum)
	{
		tcpprev = &textcmdplayer->next;
		textcmdplayer = textcmdplayer->next;
	}

	// If we don't have an entry for the player, make it.
	if (!textcmdplayer)
	{
		textcmdplayer = *tcpprev = Z_Calloc(sizeof (textcmdplayer_t), PU_STATIC, NULL);
		textcmdplayer->playernum = playernum;
	}

	return textcmdplayer->cmd;
}

static void ExtraDataTicker(void)
{
	INT32 i;

	for (i = 0; i < MAXPLAYERS; i++)
		if (playeringame[i] || i == 0)
		{
			UINT8 *bufferstart = D_GetExistingTextcmd(gametic, i);

			if (bufferstart)
			{
				UINT8 *curpos = bufferstart;
				UINT8 *bufferend = &curpos[curpos[0]+1];

				curpos++;
				while (curpos < bufferend)
				{
					if (*curpos < MAXNETXCMD && listnetxcmd[*curpos])
					{
						const UINT8 id = *curpos;
						curpos++;
						DEBFILE(va("executing x_cmd %s ply %u ", netxcmdnames[id - 1], i));
						(listnetxcmd[id])(&curpos, i);
						DEBFILE("done\n");
					}
					else
					{
						if (server)
						{
							SendKick(i, KICK_MSG_CON_FAIL);
							DEBFILE(va("player %d kicked [gametic=%u] reason as follows:\n", i, gametic));
						}
						CONS_Alert(CONS_WARNING, M_GetText("Got unknown net command [%s]=%d (max %d)\n"), sizeu1(curpos - bufferstart), *curpos, bufferstart[0]);
						break;
					}
				}
			}
		}

	// If you are a client, you can safely forget the net commands for this tic
	// If you are the server, you need to remember them until every client has been acknowledged,
	// because if you need to resend a PT_SERVERTICS packet, you will need to put the commands in it
	if (client)
		D_FreeTextcmd(gametic);
}

static void D_Clearticcmd(tic_t tic)
{
	INT32 i;

	D_FreeTextcmd(tic);

	for (i = 0; i < MAXPLAYERS; i++)
		netcmds[tic%BACKUPTICS][i].flags = 0;

	DEBFILE(va("clear tic %5u (%2u)\n", tic, tic%BACKUPTICS));
}

void D_ResetTiccmds(void)
{
	INT32 i, j;

	for (i = 0; i < MAXSPLITSCREENPLAYERS; i++)
	{
		for (j = 0; j < MAXGENTLEMENDELAY; j++)
		{
			memset(&localcmds[i][j], 0, sizeof(ticcmd_t));
		}
	}

	// Reset the net command list
	for (i = 0; i < TEXTCMD_HASH_SIZE; i++)
		while (textcmds[i])
			D_Clearticcmd(textcmds[i]->tic);
}

void SendKick(UINT8 playernum, UINT8 msg)
{
	UINT8 buf[2];

	buf[0] = playernum;
	buf[1] = msg;
	SendNetXCmd(XD_KICK, &buf, 2);
}

// -----------------------------------------------------------------
// end of extra data function
// -----------------------------------------------------------------

// -----------------------------------------------------------------
// extra data function for lmps
// -----------------------------------------------------------------

// if extradatabit is set, after the ziped tic you find this:
//
//   type   |  description
// ---------+--------------
//   byte   | size of the extradata
//   byte   | the extradata (xd) bits: see XD_...
//            with this byte you know what parameter folow
// if (xd & XDNAMEANDCOLOR)
//   byte   | color
//   char[MAXPLAYERNAME] | name of the player
// endif
// if (xd & XD_WEAPON_PREF)
//   byte   | original weapon switch: boolean, true if use the old
//          | weapon switch methode
//   char[NUMWEAPONS] | the weapon switch priority
//   byte   | autoaim: true if use the old autoaim system
// endif
/*boolean AddLmpExtradata(UINT8 **demo_point, INT32 playernum)
{
	UINT8 *textcmd = D_GetExistingTextcmd(gametic, playernum);

	if (!textcmd)
		return false;

	M_Memcpy(*demo_point, textcmd, textcmd[0]+1);
	*demo_point += textcmd[0]+1;
	return true;
}

void ReadLmpExtraData(UINT8 **demo_pointer, INT32 playernum)
{
	UINT8 nextra;
	UINT8 *textcmd;

	if (!demo_pointer)
		return;

	textcmd = D_GetTextcmd(gametic, playernum);
	nextra = **demo_pointer;
	M_Memcpy(textcmd, *demo_pointer, nextra + 1);
	// increment demo pointer
	*demo_pointer += nextra + 1;
}*/

// -----------------------------------------------------------------
// end extra data function for lmps
// -----------------------------------------------------------------

static INT16 Consistancy(void);

typedef enum
{
	CL_SEARCHING,
	CL_CHECKFILES,
	CL_DOWNLOADFILES,
	CL_DOWNLOADFAILED,
	CL_ASKJOIN,
	CL_LOADFILES,
	CL_SETUPFILES,
	CL_WAITJOINRESPONSE,
	CL_DOWNLOADSAVEGAME,
	CL_CONNECTED,
	CL_ABORTED,
	CL_ASKFULLFILELIST,
	CL_CONFIRMCONNECT,
#ifdef HAVE_CURL
	CL_PREPAREHTTPFILES,
	CL_DOWNLOADHTTPFILES,
#endif
	CL_SENDKEY,
	CL_WAITCHALLENGE,
} cl_mode_t;

static void GetPackets(void);

static cl_mode_t cl_mode = CL_SEARCHING;

#ifdef HAVE_CURL
char http_source[MAX_MIRROR_LENGTH];
#endif

static UINT16 cl_lastcheckedfilecount = 0;	// used for full file list

//
// CL_DrawConnectionStatus
//
// Keep the local client informed of our status.
//
static inline void CL_DrawConnectionStatus(void)
{
	INT32 ccstime = I_GetTime();

	// Draw background fade
	if (!menuactive) // menu already draws its own fade
		V_DrawFadeScreen(0xFF00, 16); // force default

	if (cl_mode != CL_DOWNLOADFILES && cl_mode != CL_LOADFILES && cl_mode != CL_CHECKFILES
#ifdef HAVE_CURL
	&& cl_mode != CL_DOWNLOADHTTPFILES
#endif
	)
	{
		INT32 i, animtime = ((ccstime / 4) & 15) + 16;
		UINT8 palstart = (cl_mode == CL_SEARCHING) ? 32 : 96;
		// 15 pal entries total.
		const char *cltext;

		// Draw bottom box
		M_DrawTextBox(BASEVIDWIDTH/2-128-8, BASEVIDHEIGHT-24-8, 32, 1);
		V_DrawCenteredString(BASEVIDWIDTH/2, BASEVIDHEIGHT-24-24, V_YELLOWMAP, "Press (B) to abort");

		for (i = 0; i < 16; ++i)
			V_DrawFill((BASEVIDWIDTH/2-128) + (i * 16), BASEVIDHEIGHT-24, 16, 8, palstart + ((animtime - i) & 15));

		switch (cl_mode)
		{
			case CL_DOWNLOADSAVEGAME:
				if (lastfilenum != -1)
				{
					UINT32 currentsize = fileneeded[lastfilenum].currentsize;
					UINT32 totalsize = fileneeded[lastfilenum].totalsize;
					INT32 dldlength;

					cltext = M_GetText("Downloading game state...");
					Net_GetNetStat();

					dldlength = (INT32)((currentsize/(double)totalsize) * 256);
					if (dldlength > 256)
						dldlength = 256;
					V_DrawFill(BASEVIDWIDTH/2-128, BASEVIDHEIGHT-16, 256, 8, 111);
					V_DrawFill(BASEVIDWIDTH/2-128, BASEVIDHEIGHT-16, dldlength, 8, 96);

					V_DrawString(BASEVIDWIDTH/2-128, BASEVIDHEIGHT-16, V_20TRANS|V_MONOSPACE,
						va(" %4uK/%4uK",currentsize>>10,totalsize>>10));

					V_DrawRightAlignedString(BASEVIDWIDTH/2+128, BASEVIDHEIGHT-16, V_20TRANS|V_MONOSPACE,
						va("%3.1fK/s ", ((double)getbps)/1024));
				}
				else
					cltext = M_GetText("Waiting to download game state...");
				break;
			case CL_ASKFULLFILELIST:
			case CL_CONFIRMCONNECT:
			case CL_DOWNLOADFAILED:
				cltext = "";
				break;
			case CL_SETUPFILES:
				cltext = M_GetText("Configuring addons...");
				break;
			case CL_ASKJOIN:
			case CL_WAITJOINRESPONSE:
				if (serverisfull)
					cltext = M_GetText("Server full, waiting for a slot...");
				else
					cltext = M_GetText("Requesting to join...");

				break;
#ifdef HAVE_CURL
			case CL_PREPAREHTTPFILES:
				cltext = M_GetText("Waiting to download files...");
				break;
#endif
			default:
				cltext = M_GetText("Connecting to server...");
				break;
		}
		V_DrawCenteredString(BASEVIDWIDTH/2, BASEVIDHEIGHT-16-24, V_YELLOWMAP, cltext);
	}
	else
	{
		if (cl_mode == CL_CHECKFILES)
		{
			INT32 totalfileslength;
			INT32 checkednum = 0;
			INT32 i;

			V_DrawCenteredString(BASEVIDWIDTH/2, BASEVIDHEIGHT-24-24, V_YELLOWMAP, "Press (B) to abort");

			//ima just count files here
			for (i = 0; i < fileneedednum; i++)
				if (fileneeded[i].status != FS_NOTCHECKED)
					checkednum++;

			// Loading progress
			V_DrawCenteredString(BASEVIDWIDTH/2, BASEVIDHEIGHT-24-32, V_YELLOWMAP, "Checking server addons...");
			totalfileslength = (INT32)((checkednum/(double)(fileneedednum)) * 256);
			M_DrawTextBox(BASEVIDWIDTH/2-128-8, BASEVIDHEIGHT-24-8, 32, 1);
			V_DrawFill(BASEVIDWIDTH/2-128, BASEVIDHEIGHT-24, 256, 8, 111);
			V_DrawFill(BASEVIDWIDTH/2-128, BASEVIDHEIGHT-24, totalfileslength, 8, 96);
			V_DrawCenteredString(BASEVIDWIDTH/2, BASEVIDHEIGHT-24, V_20TRANS|V_MONOSPACE,
				va(" %2u/%2u Files",checkednum,fileneedednum));
		}
		else if (cl_mode == CL_LOADFILES)
		{
			INT32 totalfileslength;
			INT32 loadcompletednum = 0;
			INT32 i;

			V_DrawCenteredString(BASEVIDWIDTH/2, BASEVIDHEIGHT-24-24, V_YELLOWMAP, "Press (B) to abort");

			//ima just count files here
			for (i = 0; i < fileneedednum; i++)
				if (fileneeded[i].status == FS_OPEN)
					loadcompletednum++;

			// Loading progress
			V_DrawCenteredString(BASEVIDWIDTH/2, BASEVIDHEIGHT-24-32, V_YELLOWMAP, "Loading server addons...");
			totalfileslength = (INT32)((loadcompletednum/(double)(fileneedednum)) * 256);
			M_DrawTextBox(BASEVIDWIDTH/2-128-8, BASEVIDHEIGHT-24-8, 32, 1);
			V_DrawFill(BASEVIDWIDTH/2-128, BASEVIDHEIGHT-24, 256, 8, 111);
			V_DrawFill(BASEVIDWIDTH/2-128, BASEVIDHEIGHT-24, totalfileslength, 8, 96);
			V_DrawCenteredString(BASEVIDWIDTH/2, BASEVIDHEIGHT-24, V_20TRANS|V_MONOSPACE,
				va(" %2u/%2u Files",loadcompletednum,fileneedednum));
		}
		else if (lastfilenum != -1)
		{
			INT32 dldlength;
			INT32 totalfileslength;
			UINT32 totaldldsize;
			static char tempname[28];
			fileneeded_t *file = &fileneeded[lastfilenum];
			char *filename = file->filename;

			// Draw the bottom box.
			M_DrawTextBox(BASEVIDWIDTH/2-128-8, BASEVIDHEIGHT-58-8, 32, 1);
			V_DrawCenteredString(BASEVIDWIDTH/2, BASEVIDHEIGHT-58-14, V_YELLOWMAP, "Press (B) to abort");

			Net_GetNetStat();
			dldlength = (INT32)((file->currentsize/(double)file->totalsize) * 256);
			if (dldlength > 256)
				dldlength = 256;

			V_DrawFill(BASEVIDWIDTH/2-128, BASEVIDHEIGHT-58, 256, 8, 111);
			V_DrawFill(BASEVIDWIDTH/2-128, BASEVIDHEIGHT-58, dldlength, 8, 96);

			memset(tempname, 0, sizeof(tempname));
			// offset filename to just the name only part
			filename += strlen(filename) - nameonlylength(filename);

			if (strlen(filename) > sizeof(tempname)-1) // too long to display fully
			{
				size_t endhalfpos = strlen(filename)-10;
				// display as first 14 chars + ... + last 10 chars
				// which should add up to 27 if our math(s) is correct
				snprintf(tempname, sizeof(tempname), "%.14s...%.10s", filename, filename+endhalfpos);
			}
			else // we can copy the whole thing in safely
			{
				strncpy(tempname, filename, sizeof(tempname)-1);
			}

			V_DrawCenteredString(BASEVIDWIDTH/2, BASEVIDHEIGHT-58-30, 0,
				va(M_GetText("%s downloading"), ((cl_mode == CL_DOWNLOADHTTPFILES) ? "\x82""HTTP" : "\x85""Direct")));
			V_DrawCenteredString(BASEVIDWIDTH/2, BASEVIDHEIGHT-58-22, V_YELLOWMAP,
				va(M_GetText("\"%s\""), tempname));
			V_DrawString(BASEVIDWIDTH/2-128, BASEVIDHEIGHT-58, V_20TRANS|V_MONOSPACE,
				va(" %4uK/%4uK",fileneeded[lastfilenum].currentsize>>10,file->totalsize>>10));
			V_DrawRightAlignedString(BASEVIDWIDTH/2+128, BASEVIDHEIGHT-58, V_20TRANS|V_MONOSPACE,
				va("%3.1fK/s ", ((double)getbps)/1024));

			// Download progress

			if (fileneeded[lastfilenum].currentsize != fileneeded[lastfilenum].totalsize)
				totaldldsize = downloadcompletedsize+fileneeded[lastfilenum].currentsize; //Add in single file progress download if applicable
			else
				totaldldsize = downloadcompletedsize;

			V_DrawCenteredString(BASEVIDWIDTH/2, BASEVIDHEIGHT-24-14, V_YELLOWMAP, "Overall Download Progress");
			totalfileslength = (INT32)((totaldldsize/(double)totalfilesrequestedsize) * 256);
			M_DrawTextBox(BASEVIDWIDTH/2-128-8, BASEVIDHEIGHT-24-8, 32, 1);
			V_DrawFill(BASEVIDWIDTH/2-128, BASEVIDHEIGHT-24, 256, 8, 111);
			V_DrawFill(BASEVIDWIDTH/2-128, BASEVIDHEIGHT-24, totalfileslength, 8, 96);

			if (totalfilesrequestedsize>>20 >= 10) //display in MB if over 10MB
				V_DrawString(BASEVIDWIDTH/2-128, BASEVIDHEIGHT-24, V_20TRANS|V_MONOSPACE,
					va(" %4uM/%4uM",totaldldsize>>20,totalfilesrequestedsize>>20));
			else
				V_DrawString(BASEVIDWIDTH/2-128, BASEVIDHEIGHT-24, V_20TRANS|V_MONOSPACE,
					va(" %4uK/%4uK",totaldldsize>>10,totalfilesrequestedsize>>10));

			V_DrawRightAlignedString(BASEVIDWIDTH/2+128, BASEVIDHEIGHT-24, V_20TRANS|V_MONOSPACE,
					va("%2u/%2u Files ",downloadcompletednum,totalfilesrequestednum));
		}
		else
		{
			INT32 i, animtime = ((ccstime / 4) & 15) + 16;
			UINT8 palstart = (cl_mode == CL_SEARCHING) ? 128 : 160;
			// 15 pal entries total.

			//Draw bottom box
			M_DrawTextBox(BASEVIDWIDTH/2-128-8, BASEVIDHEIGHT-24-8, 32, 1);
			V_DrawCenteredString(BASEVIDWIDTH/2, BASEVIDHEIGHT-24-24, V_YELLOWMAP, "Press (B) to abort");

			for (i = 0; i < 16; ++i)
				V_DrawFill((BASEVIDWIDTH/2-128) + (i * 16), BASEVIDHEIGHT-24, 16, 8, palstart + ((animtime - i) & 15));

			V_DrawCenteredString(BASEVIDWIDTH/2, BASEVIDHEIGHT-24-32, V_YELLOWMAP,
				M_GetText("Waiting to download files..."));
		}
	}
}

static boolean CL_AskFileList(INT32 firstfile)
{
	netbuffer->packettype = PT_TELLFILESNEEDED;
	netbuffer->u.filesneedednum = firstfile;

	return HSendPacket(servernode, false, 0, sizeof (INT32));
}

/** Sends a special packet to declare how many players in local
  * Used only in arbitratrenetstart()
  * Sends a PT_CLIENTJOIN packet to the server
  *
  * \return True if the packet was successfully sent
  * \todo Improve the description...
  *       Because to be honest, I have no idea what arbitratrenetstart is...
  *       Is it even used...?
  *
  */
static boolean CL_SendJoin(void)
{
	UINT8 localplayers = 1;
	UINT8 i;

	if (netgame)
		CONS_Printf(M_GetText("Sending join request...\n"));
	netbuffer->packettype = PT_CLIENTJOIN;

	if (splitscreen)
		localplayers += splitscreen;

	netbuffer->u.clientcfg.localplayers = localplayers;
	netbuffer->u.clientcfg._255 = 255;
	netbuffer->u.clientcfg.packetversion = PACKETVERSION;
	netbuffer->u.clientcfg.version = VERSION;
	netbuffer->u.clientcfg.subversion = SUBVERSION;
	strncpy(netbuffer->u.clientcfg.application, SRB2APPLICATION,
			sizeof netbuffer->u.clientcfg.application);

	for (i = 0; i <= splitscreen; i++)
	{
		// the MAXPLAYERS addition is necessary to communicate that g_localplayers is not yet safe to reference
		CleanupPlayerName(MAXPLAYERS+i, cv_playername[i].zstring);
		strncpy(netbuffer->u.clientcfg.names[i], cv_playername[i].zstring, MAXPLAYERNAME);
	}
	// privacy shield for the local players not joining this session
	for (; i < MAXSPLITSCREENPLAYERS; i++)
		strncpy(netbuffer->u.clientcfg.names[i], va("Player %c", 'A' + i), MAXPLAYERNAME);

	memcpy(&netbuffer->u.clientcfg.availabilities, R_GetSkinAvailabilities(false, false), MAXAVAILABILITY*sizeof(UINT8));

	// Don't leak old signatures from prior sessions.
	memset(&netbuffer->u.clientcfg.challengeResponse, 0, sizeof(((clientconfig_pak *)0)->challengeResponse));

	if (client && netgame)
	{
		shouldsign_t safe = ShouldSignChallenge(awaitingChallenge);

		if (safe != SIGN_OK)
		{
			if (safe == SIGN_BADIP)
			{
				I_Error("External server IP didn't match the message it sent.");
			}
			else if (safe == SIGN_BADTIME)
			{
				I_Error("External server sent a message with an unusual timestamp.\nCheck your clocks!");
			}
			else
			{
				I_Error("External server asked for a signature on something strange.\nPlease notify a developer if you've seen this more than once.");
			}
			return false;
		}
	}

	for (i = 0; i <= splitscreen; i++)
	{
		uint8_t signature[64];
		profile_t *localProfile = PR_GetLocalPlayerProfile(i);

		if (PR_IsLocalPlayerGuest(i)) // GUESTS don't have keys
		{
			memset(signature, 0, 64);
		}
		else
		{
			// If our keys are garbage (corrupted profile?), fail here instead of when the server boots us, so the player knows what's going on.
			crypto_eddsa_sign(signature, localProfile->secret_key, awaitingChallenge, 32);
			if (crypto_eddsa_check(signature, localProfile->public_key, awaitingChallenge, 32) != 0)
				I_Error("Couldn't self-verify key associated with player %d, profile %d.\nProfile data may be corrupted.", i, cv_lastprofile[i].value); // I guess this is the most reasonable way to catch a malformed key.
		}

		#ifdef DEVELOP
			if (cv_badjoin.value)
			{
				CV_AddValue(&cv_badjoin, -1);
				CONS_Alert(CONS_WARNING, "cv_badjoin enabled, scrubbing signature from CL_SendJoin\n");
				memset(signature, 0, 64);
			}
		#endif

		// Testing
		// memset(signature, 0, sizeof(signature));

		memcpy(&netbuffer->u.clientcfg.challengeResponse[i], signature, sizeof(signature));
	}

	return HSendPacket(servernode, false, 0, sizeof (clientconfig_pak));
}

static boolean CL_SendKey(void)
{
	int i;
	netbuffer->packettype = PT_CLIENTKEY;

	memset(netbuffer->u.clientkey.key, 0, sizeof(((clientkey_pak *)0)->key));
	for (i = 0; i <= splitscreen; i++)
	{
		// GUEST profiles have all-zero keys. This will be handled at the end of the challenge process, don't worry about it.
		memcpy(netbuffer->u.clientkey.key[i], PR_GetProfile(cv_lastprofile[i].value)->public_key, 32);
	}
	return HSendPacket(servernode, false, 0, sizeof (clientkey_pak) );
}

static void
CopyCaretColors (char *p, const char *s, int n)
{
	char *t;
	int   m;
	int   c;
	if (!n)
		return;
	while (( t = strchr(s, '^') ))
	{
		m = ( t - s );

		if (m >= n)
		{
			memcpy(p, s, n);
			return;
		}
		else
			memcpy(p, s, m);

		p += m;
		n -= m;
		s += m;

		if (!n)
			return;

		if (s[1])
		{
			c = toupper(s[1]);
			if (isdigit(c))
				c = 0x80 + ( c - '0' );
			else if (c >= 'A' && c <= 'F')
				c = 0x80 + ( c - 'A' );
			else
				c = 0;

			if (c)
			{
				*p++ = c;
				n--;

				if (!n)
					return;
			}
			else
			{
				if (n < 2)
					break;

				memcpy(p, s, 2);

				p += 2;
				n -= 2;
			}

			s += 2;
		}
		else
			break;
	}
	strncpy(p, s, n);
}

static void SV_SendServerInfo(INT32 node, tic_t servertime)
{
	UINT8 *p;
	size_t mirror_length;
	const char *httpurl = cv_httpsource.string;

	netbuffer->packettype = PT_SERVERINFO;
	netbuffer->u.serverinfo._255 = 255;
	netbuffer->u.serverinfo.packetversion = PACKETVERSION;

	netbuffer->u.serverinfo.version = VERSION;
	netbuffer->u.serverinfo.subversion = SUBVERSION;

#ifdef DEVELOP
	memcpy(netbuffer->u.serverinfo.commit,
			comprevision_abbrev_bin, GIT_SHA_ABBREV);
#endif

	strncpy(netbuffer->u.serverinfo.application, SRB2APPLICATION,
			sizeof netbuffer->u.serverinfo.application);
	// return back the time value so client can compute their ping
	netbuffer->u.serverinfo.time = (tic_t)LONG(servertime);
	netbuffer->u.serverinfo.leveltime = (tic_t)LONG(leveltime);

	netbuffer->u.serverinfo.numberofplayer = (UINT8)D_NumPlayers();
	netbuffer->u.serverinfo.maxplayer = (UINT8)(min((dedicated ? MAXPLAYERS-1 : MAXPLAYERS), cv_maxconnections.value));

	if (!node)
		netbuffer->u.serverinfo.refusereason = 0;
	else if (!cv_allownewplayer.value)
		netbuffer->u.serverinfo.refusereason = 1;
	else if (D_NumPlayers() >= cv_maxconnections.value)
		netbuffer->u.serverinfo.refusereason = 2;
	else
		netbuffer->u.serverinfo.refusereason = 0;

	strncpy(netbuffer->u.serverinfo.gametypename, gametypes[gametype]->name,
			sizeof netbuffer->u.serverinfo.gametypename);
	netbuffer->u.serverinfo.modifiedgame = (UINT8)modifiedgame;
	netbuffer->u.serverinfo.cheatsenabled = CV_CheatsEnabled();

	netbuffer->u.serverinfo.kartvars = (UINT8) (
		(gamespeed & SV_SPEEDMASK) |
		(dedicated ? SV_DEDICATED : 0)
	);

	CopyCaretColors(netbuffer->u.serverinfo.servername, cv_servername.string,
		MAXSERVERNAME);

	M_Memcpy(netbuffer->u.serverinfo.mapmd5, mapmd5, 16);

	if (!(mapheaderinfo[gamemap-1]->levelflags & LF_NOZONE) && !(mapheaderinfo[prevmap]->zonttl[0]))
		netbuffer->u.serverinfo.iszone = 1;
	else
		netbuffer->u.serverinfo.iszone = 0;

	memset(netbuffer->u.serverinfo.maptitle, 0, sizeof netbuffer->u.serverinfo.maptitle);

	if (!(mapheaderinfo[gamemap-1]->menuflags & LF2_HIDEINMENU) && mapheaderinfo[gamemap-1]->lvlttl[0])
	{
		//strncpy(netbuffer->u.serverinfo.maptitle, (char *)mapheaderinfo[gamemap-1]->lvlttl, sizeof netbuffer->u.serverinfo.maptitle);
		// set up the levelstring
		if (netbuffer->u.serverinfo.iszone || (mapheaderinfo[gamemap-1]->levelflags & LF_NOZONE))
		{
			if (snprintf(netbuffer->u.serverinfo.maptitle,
				sizeof netbuffer->u.serverinfo.maptitle,
				"%s",
				mapheaderinfo[gamemap-1]->lvlttl) < 0)
			{
				// If there's an encoding error, send "Unknown", we accept that the above may be truncated
				strncpy(netbuffer->u.serverinfo.maptitle, "Unknown", sizeof netbuffer->u.serverinfo.maptitle);
			}
		}
		else
		{
			if (snprintf(netbuffer->u.serverinfo.maptitle,
				sizeof netbuffer->u.serverinfo.maptitle,
				"%s %s",
				mapheaderinfo[gamemap-1]->lvlttl, mapheaderinfo[gamemap-1]->zonttl) < 0)
			{
				// If there's an encoding error, send "Unknown", we accept that the above may be truncated
				strncpy(netbuffer->u.serverinfo.maptitle, "Unknown", sizeof netbuffer->u.serverinfo.maptitle);
			}
		}
	}
	else
		strncpy(netbuffer->u.serverinfo.maptitle, "Unknown", sizeof netbuffer->u.serverinfo.maptitle);

	netbuffer->u.serverinfo.actnum = mapheaderinfo[gamemap-1]->actnum;

	memset(netbuffer->u.serverinfo.httpsource, 0, MAX_MIRROR_LENGTH);

	mirror_length = strlen(httpurl);
	if (mirror_length > MAX_MIRROR_LENGTH)
		mirror_length = MAX_MIRROR_LENGTH;

	if (snprintf(netbuffer->u.serverinfo.httpsource, mirror_length+1, "%s", httpurl) < 0)
		// If there's an encoding error, send nothing, we accept that the above may be truncated
		strncpy(netbuffer->u.serverinfo.httpsource, "", mirror_length);

	netbuffer->u.serverinfo.httpsource[MAX_MIRROR_LENGTH-1] = '\0';

	if (cv_kartusepwrlv.value)
		netbuffer->u.serverinfo.avgpwrlv = K_CalculatePowerLevelAvg();
	else
		netbuffer->u.serverinfo.avgpwrlv = -1;

	p = PutFileNeeded(0);

	HSendPacket(node, false, 0, p - ((UINT8 *)&netbuffer->u));
}

static void SV_SendPlayerInfo(INT32 node)
{
	UINT8 i;
	netbuffer->packettype = PT_PLAYERINFO;

	for (i = 0; i < MSCOMPAT_MAXPLAYERS; i++)
	{
		if (i >= MAXPLAYERS)
		{
			netbuffer->u.playerinfo[i].num = 255; // Master Server compat
			continue;
		}

		if (!playeringame[i])
		{
			netbuffer->u.playerinfo[i].num = 255; // This slot is empty.
			continue;
		}

		netbuffer->u.playerinfo[i].num = i;
		strncpy(netbuffer->u.playerinfo[i].name, (const char *)&player_names[i], MAXPLAYERNAME+1);
		netbuffer->u.playerinfo[i].name[MAXPLAYERNAME] = '\0';

		//fetch IP address
		//No, don't do that, you fuckface.
		memset(netbuffer->u.playerinfo[i].address, 0, 4);

		if (G_GametypeHasTeams())
		{
			if (!players[i].ctfteam)
				netbuffer->u.playerinfo[i].team = 255;
			else
				netbuffer->u.playerinfo[i].team = (UINT8)players[i].ctfteam;
		}
		else
		{
			if (players[i].spectator)
				netbuffer->u.playerinfo[i].team = 255;
			else
				netbuffer->u.playerinfo[i].team = 0;
		}

		netbuffer->u.playerinfo[i].score = LONG(players[i].score);
		netbuffer->u.playerinfo[i].timeinserver = SHORT((UINT16)(players[i].jointime / TICRATE));
		netbuffer->u.playerinfo[i].skin = (UINT8)(players[i].skin
#ifdef DEVELOP // it's safe to do this only because PLAYERINFO isn't read by the game itself
		% 3
#endif
		);

		// Extra data
		netbuffer->u.playerinfo[i].data = 0; //players[i].skincolor;
	}

	HSendPacket(node, false, 0, sizeof(plrinfo) * MSCOMPAT_MAXPLAYERS);
}

/** Sends a PT_SERVERCFG packet
  *
  * \param node The destination
  * \return True if the packet was successfully sent
  *
  */
static boolean SV_SendServerConfig(INT32 node)
{
	boolean waspacketsent;

	memset(&netbuffer->u.servercfg, 0, sizeof netbuffer->u.servercfg);

	netbuffer->packettype = PT_SERVERCFG;

	netbuffer->u.servercfg.version = VERSION;
	netbuffer->u.servercfg.subversion = SUBVERSION;

	netbuffer->u.servercfg.serverplayer = (UINT8)serverplayer;
	netbuffer->u.servercfg.totalslotnum = (UINT8)(doomcom->numslots);
	netbuffer->u.servercfg.gametic = (tic_t)LONG(gametic);
	netbuffer->u.servercfg.clientnode = (UINT8)node;
	netbuffer->u.servercfg.gamestate = (UINT8)gamestate;
	netbuffer->u.servercfg.gametype = (UINT8)gametype;
	netbuffer->u.servercfg.modifiedgame = (UINT8)modifiedgame;

	netbuffer->u.servercfg.maxplayer = (UINT8)(min((dedicated ? MAXPLAYERS-1 : MAXPLAYERS), cv_maxconnections.value));
	netbuffer->u.servercfg.allownewplayer = cv_allownewplayer.value;
	netbuffer->u.servercfg.discordinvites = (boolean)cv_discordinvites.value;

	memcpy(netbuffer->u.servercfg.server_context, server_context, 8);

	{
		const size_t len = sizeof (serverconfig_pak);

#ifdef DEBUGFILE
		if (debugfile)
		{
			fprintf(debugfile, "ServerConfig Packet about to be sent, size of packet:%s to node:%d\n",
				sizeu1(len), node);
		}
#endif

		waspacketsent = HSendPacket(node, true, 0, len);
	}

#ifdef DEBUGFILE
	if (debugfile)
	{
		if (waspacketsent)
		{
			fprintf(debugfile, "ServerConfig Packet was sent\n");
		}
		else
		{
			fprintf(debugfile, "ServerConfig Packet could not be sent right now\n");
		}
	}
#endif

	return waspacketsent;
}

static boolean SV_ResendingSavegameToAnyone(void)
{
	INT32 i;

	for (i = 0; i < MAXNETNODES; i++)
		if (resendingsavegame[i])
			return true;
	return false;
}

static void SV_SendSaveGame(INT32 node, boolean resending)
{
	size_t length, compressedlen;
	savebuffer_t save = {0};
	UINT8 *compressedsave;
	UINT8 *buffertosend;

	// first save it in a malloced buffer
	if (P_SaveBufferAlloc(&save, NETSAVEGAMESIZE) == false)
	{
		CONS_Alert(CONS_ERROR, M_GetText("No more free memory for savegame\n"));
		return;
	}

	// Leave room for the uncompressed length.
	save.p += sizeof(UINT32);

	P_SaveNetGame(&save, resending);

	length = save.p - save.buffer;
	if (length > NETSAVEGAMESIZE)
	{
		P_SaveBufferFree(&save);
		I_Error("Savegame buffer overrun");
	}

	// Allocate space for compressed save: one byte fewer than for the
	// uncompressed data to ensure that the compression is worthwhile.
	compressedsave = Z_Malloc(length - 1, PU_STATIC, NULL);
	if (!compressedsave)
	{
		CONS_Alert(CONS_ERROR, M_GetText("No more free memory for savegame\n"));
		return;
	}

	// Attempt to compress it.
	if ((compressedlen = lzf_compress(save.buffer + sizeof(UINT32), length - sizeof(UINT32), compressedsave + sizeof(UINT32), length - sizeof(UINT32) - 1)))
	{
		// Compressing succeeded; send compressed data
		P_SaveBufferFree(&save);

		// State that we're compressed.
		buffertosend = compressedsave;
		WRITEUINT32(compressedsave, length - sizeof(UINT32));
		length = compressedlen + sizeof(UINT32);
	}
	else
	{
		// Compression failed to make it smaller; send original
		Z_Free(compressedsave);

		// State that we're not compressed
		buffertosend = save.buffer;
		WRITEUINT32(save.buffer, 0);
	}

	AddRamToSendQueue(node, buffertosend, length, SF_Z_RAM, 0);

	// Remember when we started sending the savegame so we can handle timeouts
	sendingsavegame[node] = true;
	freezetimeout[node] = I_GetTime() + jointimeout + length / 1024; // 1 extra tic for each kilobyte
}

#ifdef DUMPCONSISTENCY
#define TMPSAVENAME "badmath.sav"
static consvar_t cv_dumpconsistency = CVAR_INIT ("dumpconsistency", "Off", CV_SAVE|CV_NETVAR, CV_OnOff, NULL);

static void SV_SavedGame(void)
{
	size_t length;
	savebuffer_t save = {0};
	char tmpsave[256];

	if (!cv_dumpconsistency.value)
		return;

	sprintf(tmpsave, "%s" PATHSEP TMPSAVENAME, srb2home);

	// first save it in a malloced buffer
	if (P_SaveBufferAlloc(&save, NETSAVEGAMESIZE) == false)
	{
		CONS_Alert(CONS_ERROR, M_GetText("No more free memory for savegame\n"));
		return;
	}

	P_SaveNetGame(&save, false);

	length = save.p - save.buffer;
	if (length > NETSAVEGAMESIZE)
	{
		P_SaveBufferFree(&save);
		I_Error("Savegame buffer overrun");
	}

	// then save it!
	if (!FIL_WriteFile(tmpsave, save.buffer, length))
		CONS_Printf(M_GetText("Didn't save %s for netgame"), tmpsave);

	P_SaveBufferFree(&save);
}

#undef  TMPSAVENAME
#endif
#define TMPSAVENAME "$$$.sav"


static void CL_LoadReceivedSavegame(boolean reloading)
{
	savebuffer_t save = {0};
	size_t length, decompressedlen;
	char tmpsave[256];

	sprintf(tmpsave, "%s" PATHSEP TMPSAVENAME, srb2home);

	if (P_SaveBufferFromFile(&save, tmpsave) == false)
	{
		I_Error("Can't read savegame sent");
		return;
	}

	length = save.size;
	CONS_Printf(M_GetText("Loading savegame length %s\n"), sizeu1(length));

	// Decompress saved game if necessary.
	decompressedlen = READUINT32(save.p);
	if (decompressedlen > 0)
	{
		UINT8 *decompressedbuffer = Z_Malloc(decompressedlen, PU_STATIC, NULL);

		lzf_decompress(save.p, length - sizeof(UINT32), decompressedbuffer, decompressedlen);

		P_SaveBufferFree(&save);
		P_SaveBufferFromExisting(&save, decompressedbuffer, decompressedlen);
	}

	paused = false;
	demo.playback = false;
	demo.title = false;
	titlemapinaction = false;
	automapactive = false;

	// load a base level
	if (P_LoadNetGame(&save, reloading))
	{
		if (!reloading)
		{
			CON_LogMessage(va(M_GetText("Map is now \"%s"), G_BuildMapName(gamemap)));

			if (strlen(mapheaderinfo[gamemap-1]->lvlttl) > 0)
			{
				CON_LogMessage(va(": %s", mapheaderinfo[gamemap-1]->lvlttl));
				if (strlen(mapheaderinfo[gamemap-1]->zonttl) > 0)
					CON_LogMessage(va(" %s", mapheaderinfo[gamemap-1]->zonttl));
				else if (!(mapheaderinfo[gamemap-1]->levelflags & LF_NOZONE))
					CON_LogMessage(M_GetText(" Zone"));
				if (mapheaderinfo[gamemap-1]->actnum > 0)
					CON_LogMessage(va(" %d", mapheaderinfo[gamemap-1]->actnum));
			}

			CON_LogMessage("\"\n");
		}
	}

	// done
	P_SaveBufferFree(&save);

	if (unlink(tmpsave) == -1)
	{
		CONS_Alert(CONS_ERROR, M_GetText("Can't delete %s\n"), tmpsave);
	}

	consistancy[gametic%BACKUPTICS] = Consistancy();
	CON_ToggleOff();

	// Tell the server we have received and reloaded the gamestate
	// so they know they can resume the game
	netbuffer->packettype = PT_RECEIVEDGAMESTATE;
	HSendPacket(servernode, true, 0, 0);

	if (reloading)
	{
		int i;
		for (i = 0; i < MAXPLAYERS; i++)
		{
			if (memcmp(priorKeys[i], players[i].public_key, sizeof(priorKeys[i])) != 0)
			{
				HandleSigfail("Gamestate reload contained new keys");
				break;
			}
		}
	}
}

static void CL_ReloadReceivedSavegame(void)
{
	INT32 i;

	for (i = 0; i < MAXPLAYERS; i++)
	{
		LUA_InvalidatePlayer(&players[i]);
		sprintf(player_names[i], "Player %c", 'A' + i);
	}

	CL_LoadReceivedSavegame(true);

	if (neededtic < gametic)
		neededtic = gametic;
	maketic = neededtic;

	for (i = 0; i <= r_splitscreen; i++)
	{
		P_ForceLocalAngle(&players[displayplayers[i]], players[displayplayers[i]].angleturn);
	}

	for (i = 0; i < MAXSPLITSCREENPLAYERS; i++)
	{
		camera[i].subsector = R_PointInSubsector(camera[i].x, camera[i].y);
	}

	cl_redownloadinggamestate = false;

	CONS_Printf(M_GetText("Game state reloaded\n"));
}

static void SendAskInfo(INT32 node)
{
	tic_t asktime;

	if (node != 0 && node != BROADCASTADDR &&
			cv_rendezvousserver.string[0])
	{
		I_NetRequestHolePunch(node);
	}

	asktime = I_GetTime();

	netbuffer->packettype = PT_ASKINFO;
	netbuffer->u.askinfo.version = VERSION;
	netbuffer->u.askinfo.time = (tic_t)LONG(asktime);

	// Even if this never arrives due to the host being firewalled, we've
	// now allowed traffic from the host to us in, so once the MS relays
	// our address to the host, it'll be able to speak to us.
	HSendPacket(node, false, 0, sizeof (askinfo_pak));
}

serverelem_t serverlist[MAXSERVERLIST];
UINT32 serverlistcount = 0;

static void SL_ClearServerList(INT32 connectedserver)
{
	UINT32 i;

	for (i = 0; i < serverlistcount; i++)
		if (connectedserver != serverlist[i].node)
		{
			Net_CloseConnection(serverlist[i].node|FORCECLOSE);
			serverlist[i].node = 0;
		}
	serverlistcount = 0;
}

static UINT32 SL_SearchServer(INT32 node)
{
	UINT32 i;
	for (i = 0; i < serverlistcount; i++)
		if (serverlist[i].node == node)
			return i;

	return UINT32_MAX;
}

static void SL_InsertServer(serverinfo_pak* info, SINT8 node)
{
	UINT32 i;

	// search if not already on it
	i = SL_SearchServer(node);
	if (i == UINT32_MAX)
	{
		// not found add it
		if (serverlistcount >= MAXSERVERLIST)
			return; // list full

		if (info->_255 != 255)
			return;/* old packet format */

		if (info->packetversion != PACKETVERSION)
			return;/* old new packet format */

		if (info->version != VERSION)
			return; // Not same version.

		if (info->subversion != SUBVERSION)
			return; // Close, but no cigar.

		if (strcmp(info->application, SRB2APPLICATION))
			return;/* that's a different mod */

		i = serverlistcount++;
	}

	serverlist[i].info = *info;
	serverlist[i].node = node;

	// resort server list
	M_SortServerList();
}

void CL_QueryServerList (msg_server_t *server_list)
{
	INT32 i;

	CL_UpdateServerList();

	for (i = 0; server_list[i].header.buffer[0]; i++)
	{
		// Make sure MS version matches our own, to
		// thwart nefarious servers who lie to the MS.

		/* lol bruh, that version COMES from the servers */
		//if (strcmp(version, server_list[i].version) == 0)
		{
			INT32 node = I_NetMakeNodewPort(server_list[i].ip, server_list[i].port);
			if (node == -1)
				break; // no more node free
			SendAskInfo(node);
			// Force close the connection so that servers can't eat
			// up nodes forever if we never get a reply back from them
			// (usually when they've not forwarded their ports).
			//
			// Don't worry, we'll get in contact with the working
			// servers again when they send SERVERINFO to us later!
			//
			// (Note: as a side effect this probably means every
			// server in the list will probably be using the same node (e.g. node 1),
			// not that it matters which nodes they use when
			// the connections are closed afterwards anyway)
			// -- Monster Iestyn 12/11/18
			Net_CloseConnection(node|FORCECLOSE);
		}
	}
}

void CL_UpdateServerList (void)
{
	SL_ClearServerList(0);

	if (!netgame && I_NetOpenSocket)
	{
		if (I_NetOpenSocket())
		{
			netgame = true;
			multiplayer = true;
		}
	}

	// search for local servers
	if (netgame)
		SendAskInfo(BROADCASTADDR);
}

static void M_ConfirmConnect(void)
{
	if (G_PlayerInputDown(0, gc_a, 1) || G_GetDeviceGameKeyDownArray(0)[KEY_ENTER])
	{
		if (totalfilesrequestednum > 0)
		{
#ifdef HAVE_CURL
			if (http_source[0] == '\0' || curl_failedwebdownload)
#endif
			{
				if (CL_SendFileRequest())
				{
					cl_mode = CL_DOWNLOADFILES;
				}
				else
				{
					cl_mode = CL_DOWNLOADFAILED;
				}
			}
#ifdef HAVE_CURL
			else
				cl_mode = CL_PREPAREHTTPFILES;
#endif
		}
		else
			cl_mode = CL_LOADFILES;

		M_StopMessage(0);
	}
	else if (G_PlayerInputDown(0, gc_b, 1) || G_PlayerInputDown(0, gc_x, 1) || G_GetDeviceGameKeyDownArray(0)[KEY_ESCAPE])
	{
		cl_mode = CL_ABORTED;
		M_StopMessage(0);
	}
}

static boolean CL_FinishedFileList(void)
{
	INT32 i;
	char *downloadsize = NULL;
	//CONS_Printf(M_GetText("Checking files...\n"));
	i = CL_CheckFiles();
	if (i == 4) // still checking ...
	{
		return true;
	}
	else if (i == 3) // too many files
	{
		D_QuitNetGame();
		CL_Reset();
		D_ClearState();
		M_StartControlPanel();
		M_StartMessage(M_GetText(
			"You have too many WAD files loaded\n"
			"to add ones the server is using.\n"
			"Please restart Ring Racers before connecting.\n\n"
			"Press (B)\n"
		), NULL, MM_NOTHING);
		return false;
	}
	else if (i == 2) // cannot join for some reason
	{
		D_QuitNetGame();
		CL_Reset();
		D_ClearState();
		M_StartControlPanel();
		M_StartMessage(M_GetText(
			"You have the wrong addons loaded.\n\n"
			"To play on this server, restart\n"
			"the game and don't load any addons.\n"
			"Ring Racers will automatically add\n"
			"everything you need when you join.\n\n"
			"Press (B)\n"
		), NULL, MM_NOTHING);
		return false;
	}
	else if (i == 1)
	{
		if (serverisfull)
		{
			M_StartMessage(M_GetText(
				"This server is full!\n"
				"\n"
				"You may load server addons (if any), and wait for a slot.\n"
				"\n"
				"Press (A) to continue or (B) to cancel\n"
			), NULL, MM_NOTHING);
			cl_mode = CL_CONFIRMCONNECT;
		}
		else
			cl_mode = CL_LOADFILES;
	}
	else
	{
		// must download something
		// can we, though?
#ifdef HAVE_CURL
		if (http_source[0] == '\0' || curl_failedwebdownload)
#endif
		{
			if (!CL_CheckDownloadable()) // nope!
			{
				D_QuitNetGame();
				CL_Reset();
				D_ClearState();
				M_StartControlPanel();
				M_StartMessage(M_GetText(
					"An error occured when trying to\n"
					"download missing addons.\n"
					"(This is almost always a problem\n"
					"with the server, not your game.)\n\n"
					"See the console or log file\n"
					"for additional details.\n\n"
					"Press (B)\n"
				), NULL, MM_NOTHING);
				return false;
			}
		}

#ifdef HAVE_CURL
		if (!curl_failedwebdownload)
#endif
		{
			downloadcompletednum = 0;
			downloadcompletedsize = 0;
			totalfilesrequestednum = 0;
			totalfilesrequestedsize = 0;

			for (i = 0; i < fileneedednum; i++)
				if (fileneeded[i].status == FS_NOTFOUND || fileneeded[i].status == FS_MD5SUMBAD)
				{
					totalfilesrequestednum++;
					totalfilesrequestedsize += fileneeded[i].totalsize;
				}

			if (totalfilesrequestedsize>>20 >= 10)
				downloadsize = Z_StrDup(va("%uM",totalfilesrequestedsize>>20));
			else
				downloadsize = Z_StrDup(va("%uK",totalfilesrequestedsize>>10));

			if (serverisfull)
				M_StartMessage(va(M_GetText(
					"This server is full!\n"
					"Download of %s additional content\n"
					"is required to join.\n"
					"\n"
					"You may download, load server addons,\n"
					"and wait for a slot.\n"
					"\n"
					"Press (A) to continue or (B) to cancel\n"
				), downloadsize), NULL, MM_NOTHING);
			else
				M_StartMessage(va(M_GetText(
					"Download of %s additional content\n"
					"is required to join.\n"
					"\n"
					"Press (A) to continue or (B) to cancel\n"
				), downloadsize), NULL, MM_NOTHING);

			Z_Free(downloadsize);
			cl_mode = CL_CONFIRMCONNECT;
		}
#ifdef HAVE_CURL
		else
		{
			if (CL_SendFileRequest())
			{
				cl_mode = CL_DOWNLOADFILES;
			}
			else
			{
				cl_mode = CL_DOWNLOADFAILED;
			}
		}
#endif
	}
	return true;
}

/** Called by CL_ServerConnectionTicker
  *
  * \param asksent The last time we asked the server to join. We re-ask every second in case our request got lost in transmit.
  * \return False if the connection was aborted
  * \sa CL_ServerConnectionTicker
  * \sa CL_ConnectToServer
  *
  */
static boolean CL_ServerConnectionSearchTicker(tic_t *asksent)
{
	INT32 i;

	// serverlist is updated by GetPacket function
	if (serverlistcount > 0)
	{
		// this can be a responce to our broadcast request
		if (servernode == -1 || servernode >= MAXNETNODES)
		{
			i = 0;
			servernode = serverlist[i].node;
			CONS_Printf(M_GetText("Found, "));
		}
		else
		{
			i = SL_SearchServer(servernode);
			if (i < 0)
				return true;
		}

		// Quit here rather than downloading files and being refused later.
		if (serverlist[i].info.refusereason)
		{
			serverisfull = true;
		}

		if (client)
		{
#ifdef DEVELOP
			// Commits do not match? Do not connect!
			if (memcmp(serverlist[i].info.commit,
						comprevision_abbrev_bin,
						GIT_SHA_ABBREV))
			{
				char theirs[GIT_SHA_ABBREV * 2 + 1];
				UINT8 n;

				for (n = 0; n < GIT_SHA_ABBREV; ++n)
				{
					sprintf(&theirs[n * 2], "%02hhx",
							serverlist[i].info.commit[n]);
				}

				D_QuitNetGame();
				CL_Reset();
				D_ClearState();
				M_StartControlPanel();

				M_StartMessage(va(
							"Your EXE differs from the server.\n"
							"  Yours: %.*s\n"
							"Theirs: %s\n\n"
							"Press ESC\n",
							GIT_SHA_ABBREV * 2, comprevision, theirs), NULL, MM_NOTHING);
				return false;
			}
#endif

#ifdef HAVE_CURL
			if (serverlist[i].info.httpsource[0])
				strncpy(http_source, serverlist[i].info.httpsource, MAX_MIRROR_LENGTH);
			else
				http_source[0] = '\0';
#else
			if (serverlist[i].info.httpsource[0])
				CONS_Printf("We received a http url from the server, however it will not be used as this build lacks curl support (%s)\n", serverlist[i].info.httpsource);
#endif
			D_ParseFileneeded(serverlist[i].info.fileneedednum, serverlist[i].info.fileneeded, 0);
			if (serverlist[i].info.kartvars & SV_LOTSOFADDONS)
			{
				cl_mode = CL_ASKFULLFILELIST;
				cl_lastcheckedfilecount = 0;
				return true;
			}

			cl_mode = CL_CHECKFILES;
		}
		else
		{
			cl_mode = CL_ASKJOIN; // files need not be checked for the server.
			*asksent = 0;
		}

		return true;
	}

	// Ask the info to the server (askinfo packet)
	if (I_GetTime() >= *asksent)
	{
		SendAskInfo(servernode);
		*asksent = I_GetTime() + NEWTICRATE;
	}

	return true;
}

/** Called by CL_ConnectToServer
  *
  * \param tmpsave The name of the gamestate file???
  * \param oldtic Used for knowing when to poll events and redraw
  * \param asksent The last time we asked the server to join. We re-ask every second in case our request got lost in transmit.
  * \return False if the connection was aborted
  * \sa CL_ServerConnectionSearchTicker
  * \sa CL_ConnectToServer
  *
  */
static boolean CL_ServerConnectionTicker(const char *tmpsave, tic_t *oldtic, tic_t *asksent)
{
	boolean waitmore;
	INT32 i;

	switch (cl_mode)
	{
		case CL_SEARCHING:
			if (!CL_ServerConnectionSearchTicker(asksent))
				return false;
			break;

		case CL_ASKFULLFILELIST:
			if (cl_lastcheckedfilecount == UINT16_MAX) // All files retrieved
				cl_mode = CL_CHECKFILES;
			else if (fileneedednum != cl_lastcheckedfilecount || I_GetTime() >= *asksent)
			{
				if (CL_AskFileList(fileneedednum))
				{
					cl_lastcheckedfilecount = fileneedednum;
					*asksent = I_GetTime() + NEWTICRATE;
				}
			}
			break;
		case CL_CHECKFILES:
			if (!CL_FinishedFileList())
				return false;
			break;
#ifdef HAVE_CURL
		case CL_PREPAREHTTPFILES:
			if (http_source[0])
			{
				for (i = 0; i < fileneedednum; i++)
					if (fileneeded[i].status == FS_NOTFOUND || fileneeded[i].status == FS_MD5SUMBAD)
					{
						curl_transfers++;
					}

				cl_mode = CL_DOWNLOADHTTPFILES;
			}
			break;

		case CL_DOWNLOADHTTPFILES:
			waitmore = false;
			for (i = 0; i < fileneedednum; i++)
				if (fileneeded[i].status == FS_NOTFOUND || fileneeded[i].status == FS_MD5SUMBAD)
				{
					if (!curl_running)
						CURLPrepareFile(http_source, i);
					waitmore = true;
					break;
				}

			if (curl_running)
				CURLGetFile();

			if (waitmore)
				break; // exit the case

			if (curl_failedwebdownload && !curl_transfers)
			{
				CONS_Printf("One or more files failed to download, falling back to internal downloader\n");
				cl_mode = CL_CHECKFILES;
				break;
			}

			if (!curl_transfers)
				cl_mode = CL_LOADFILES;

			break;
#endif
		case CL_DOWNLOADFILES:
			waitmore = false;
			for (i = 0; i < fileneedednum; i++)
				if (fileneeded[i].status == FS_DOWNLOADING
					|| fileneeded[i].status == FS_REQUESTED)
				{
					waitmore = true;
					break;
				}
			if (waitmore)
				break; // exit the case

			cl_mode = CL_LOADFILES;
			break;
		case CL_DOWNLOADFAILED:
			{
				CONS_Printf(M_GetText("Legacy downloader request packet failed.\n"));
				CONS_Printf(M_GetText("Network game synchronization aborted.\n"));
				D_QuitNetGame();
				CL_Reset();
				D_ClearState();
				M_StartControlPanel();
				M_StartMessage(M_GetText(
					"The direct download encountered an error.\n"
					"See the logfile for more info.\n"
					"\n"
					"Press (B)\n"
				), NULL, MM_NOTHING);
				return false;
			}
		case CL_LOADFILES:
			if (CL_LoadServerFiles())
				cl_mode = CL_SETUPFILES;

			break;
		case CL_SETUPFILES:
			if (P_PartialAddGetStage() < 0 || P_MultiSetupWadFiles(false))
			{
				*asksent = 0; //This ensure the first join ask is right away
				firstconnectattempttime = I_GetTime();
				cl_mode = CL_SENDKEY;
			}
			break;
		case CL_ASKJOIN:
			if (firstconnectattempttime + NEWTICRATE*300 < I_GetTime() && !server)
			{
				CONS_Printf(M_GetText("5 minute wait time exceeded.\n"));
				CONS_Printf(M_GetText("Network game synchronization aborted.\n"));
				D_QuitNetGame();
				CL_Reset();
				D_ClearState();
				M_StartControlPanel();
				M_StartMessage(M_GetText(
					"5 minute wait time exceeded.\n"
					"You may retry connection.\n"
					"\n"
					"Press (B)\n"
				), NULL, MM_NOTHING);
				return false;
			}
			// prepare structures to save the file
			// WARNING: this can be useless in case of server not in GS_LEVEL
			// but since the network layer doesn't provide ordered packets...
			CL_PrepareDownloadSaveGame(tmpsave);
			if (I_GetTime() >= *asksent && CL_SendJoin())
			{
				*asksent = I_GetTime() + NEWTICRATE*3;
				cl_mode = CL_WAITJOINRESPONSE;
			}
			break;
		case CL_WAITJOINRESPONSE:
			if (I_GetTime() >= *asksent)
			{
				cl_mode = CL_ASKJOIN;
			}
			break;
		case CL_SENDKEY:
			if (I_GetTime() >= *asksent && CL_SendKey())
			{
				*asksent = I_GetTime() + NEWTICRATE*3;
				cl_mode = CL_WAITCHALLENGE;
			}
			break;
		case CL_WAITCHALLENGE:
			if (I_GetTime() >= *asksent)
			{
				cl_mode = CL_SENDKEY;
			}
			break;
		case CL_DOWNLOADSAVEGAME:
			// At this state, the first (and only) needed file is the gamestate
			if (fileneeded[0].status == FS_FOUND)
			{
				// Gamestate is now handled within CL_LoadReceivedSavegame()
				CL_LoadReceivedSavegame(false);
				cl_mode = CL_CONNECTED;
				break;
			} // don't break case continue to CL_CONNECTED
			else
				break;
		case CL_CONNECTED:
		case CL_CONFIRMCONNECT: //logic is handled by M_ConfirmConnect
		default:
			break;

		// Connection closed by cancel, timeout or refusal.
		case CL_ABORTED:
			cl_mode = CL_SEARCHING;
			return false;

	}

	GetPackets();
	Net_AckTicker();

	// Call it only once by tic
	if (*oldtic != I_GetTime())
	{
		I_OsPolling();

		// Needs to be updated here for M_DrawEggaChannel
		renderdeltatics = FRACUNIT;
		rendertimefrac = FRACUNIT;

		G_ResetAllDeviceResponding();

		if (netgame)
		{
			for (; eventtail != eventhead; eventtail = (eventtail+1) & (MAXEVENTS-1))
			{
				HandleGamepadDeviceEvents(&events[eventtail]);
				G_MapEventsToControls(&events[eventtail]);
			}

			if (cl_mode == CL_CONFIRMCONNECT)
			{
				M_ConfirmConnect();
			}
			else
			{
				if (G_PlayerInputDown(0, gc_b, 1)
					|| G_PlayerInputDown(0, gc_x, 1)
					|| G_GetDeviceGameKeyDownArray(0)[KEY_ESCAPE])
					cl_mode = CL_ABORTED;
			}
		}

		if (cl_mode == CL_ABORTED)
		{
			CONS_Printf(M_GetText("Network game synchronization aborted.\n"));
//				M_StartMessage(M_GetText("Network game synchronization aborted.\n\nPress (B)\n"), NULL, MM_NOTHING);

			D_QuitNetGame();
			CL_Reset();
			D_ClearState();
			M_StartControlPanel();
			return false;
		}

		if (client && (cl_mode == CL_DOWNLOADFILES || cl_mode == CL_DOWNLOADSAVEGAME))
			FileReceiveTicker();

		// why are these here? this is for servers, we're a client
		//if (key == 's' && server)
		//	doomcom->numnodes = (INT16)pnumnodes;
		//FileSendTicker();
		*oldtic = I_GetTime();

		if (client && cl_mode != CL_CONNECTED && cl_mode != CL_ABORTED)
		{
			if (cl_mode != CL_DOWNLOADFILES && cl_mode != CL_DOWNLOADSAVEGAME)
			{
				M_DrawEggaChannel();
			}
			CL_DrawConnectionStatus();

			if (cl_mode == CL_CONFIRMCONNECT)
			{
#ifdef HAVE_THREADS
				I_lock_mutex(&k_menu_mutex);
#endif
				M_DrawMenuMessage();
#ifdef HAVE_THREADS
				I_unlock_mutex(k_menu_mutex);
#endif
			}
			I_UpdateNoVsync(); // page flip or blit buffer
			I_NewTwodeeFrame();
#ifdef HWRENDER
			if (moviemode && rendermode == render_opengl)
				M_LegacySaveFrame();
#endif
			S_UpdateSounds();
			S_UpdateClosedCaptions();
		}
	}
	else
	{
		I_Sleep(cv_sleep.value);
		I_UpdateTime(cv_timescale.value);
	}

	return true;
}

/** Use adaptive send using net_bandwidth and stat.sendbytes
  *
  * \todo Better description...
  *
  */
static void CL_ConnectToServer(void)
{
	INT32 pnumnodes, nodewaited = doomcom->numnodes, i;
	tic_t oldtic;
	tic_t asksent;
	char tmpsave[256];

	sprintf(tmpsave, "%s" PATHSEP TMPSAVENAME, srb2home);

	lastfilenum = -1;

	cl_mode = CL_SEARCHING;

	// Don't get a corrupt savegame error because tmpsave already exists
	if (FIL_FileExists(tmpsave) && unlink(tmpsave) == -1)
		I_Error("Can't delete %s\n", tmpsave);

	if (netgame)
	{
		if (servernode < 0 || servernode >= MAXNETNODES)
			CONS_Printf(M_GetText("Searching for a server...\n"));
		else
			CONS_Printf(M_GetText("Contacting the server...\n"));
	}

	if (cv_currprofile.value == -1 && !demo.playback)
	{
		PR_ApplyProfilePretend(cv_ttlprofilen.value, 0);
		for (i = 1; i < cv_splitplayers.value; i++)
		{
			PR_ApplyProfile(cv_lastprofile[i].value, i);
		}
	}
	if (gamestate == GS_INTERMISSION)
		Y_EndIntermission(); // clean up intermission graphics etc
	if (gamestate == GS_VOTING)
		Y_EndVote();

	DEBFILE(va("waiting %d nodes\n", doomcom->numnodes));
	G_SetGamestate(GS_WAITINGPLAYERS);
	if (wipegamestate == GS_MENU)
		M_ClearMenus(true);
	wipegamestate = GS_WAITINGPLAYERS;

	ClearAdminPlayers();
	Schedule_Clear();
	Automate_Clear();
	K_ClearClientPowerLevels();

	pnumnodes = 1;
	oldtic = 0;
	asksent = 0;
	firstconnectattempttime = I_GetTime();

	i = SL_SearchServer(servernode);

	if (i != -1)
	{
		char *gametypestr = serverlist[i].info.gametypename;

		CON_LogMessage(va(M_GetText("Connecting to: %s\n"), serverlist[i].info.servername));

		gametypestr[sizeof serverlist[i].info.gametypename - 1] = '\0';
		CON_LogMessage(va(M_GetText("Gametype: %s\n"), gametypestr));

		CON_LogMessage(va(M_GetText("Version: %d.%d\n"),
		 serverlist[i].info.version, serverlist[i].info.subversion));
	}
	SL_ClearServerList(servernode);

	do
	{
		// If the connection was aborted for some reason, leave
		if (!CL_ServerConnectionTicker(tmpsave, &oldtic, &asksent))
		{
			if (P_PartialAddGetStage() >= 0)
				P_MultiSetupWadFiles(true); // in case any partial adds were done

			return;
		}

		if (server)
		{
			pnumnodes = 0;
			for (i = 0; i < MAXNETNODES; i++)
				if (nodeingame[i])
					pnumnodes++;
		}
	}
	while (!(cl_mode == CL_CONNECTED && (client || (server && nodewaited <= pnumnodes))));

	if (netgame)
		F_StartWaitingPlayers();
	DEBFILE(va("Synchronisation Finished\n"));

	displayplayers[0] = consoleplayer;

	// At this point we've succesfully joined the server, if we joined by IP (ie: a valid joinedIP string), save it!
	// @TODO: Save the proper server name, right now it doesn't seem like we can consistently retrieve it from the serverlist....?
	// It works... sometimes but not always which is weird.

	tmpsave[0] = '\0'; // TEMPORARY -- connectedservername is currently only set for YOUR server
	if (joinedIP[0])	// false if we have "" which is \0
		M_AddToJoinedIPs(joinedIP, tmpsave); //connectedservername); -- as above

	joinedIP[0] = '\0';	// And empty this for good measure regardless of whether or not we actually used it.

}

static void Command_ShowBan(void) //Print out ban list
{
	size_t i;
	const char *address, *mask, *reason, *username;
	time_t unbanTime = NO_BAN_TIME;
	const time_t curTime = time(NULL);

	if (I_GetBanAddress)
		CONS_Printf(M_GetText("Ban List:\n"));
	else
		return;

	for (i = 0; (address = I_GetBanAddress(i)) != NULL; i++)
	{
		unbanTime = NO_BAN_TIME;
		if (I_GetUnbanTime)
			unbanTime = I_GetUnbanTime(i);

		if (unbanTime != NO_BAN_TIME && curTime >= unbanTime)
			continue;

		CONS_Printf("%s: ", sizeu1(i+1));

		if (I_GetBanUsername && (username = I_GetBanUsername(i)) != NULL)
			CONS_Printf("%s - ", username);

		if (!I_GetBanMask || (mask = I_GetBanMask(i)) == NULL)
			CONS_Printf("%s", address);
		else
			CONS_Printf("%s/%s", address, mask);

		if (I_GetBanReason && (reason = I_GetBanReason(i)) != NULL)
			CONS_Printf(" - %s", reason);

		if (unbanTime != NO_BAN_TIME)
		{
			 // these are fudged a little to match what a joiner sees
			int minutes = ((unbanTime - curTime) + 30) / 60;
			int hours = (minutes + 1) / 60;
			int days = (hours + 1) / 24;
			if (days)
				CONS_Printf(" (%d day%s)", days, days > 1 ? "s" : "");
			else if (hours)
				CONS_Printf(" (%d hour%s)", hours, hours > 1 ? "s" : "");
			else if (minutes)
				CONS_Printf(" (%d minute%s)", minutes, minutes > 1 ? "s" : "");
			else
				CONS_Printf(" (<1 minute)");
		}

		CONS_Printf("\n");
	}

	if (i == 0 && !address)
		CONS_Printf(M_GetText("(empty)\n"));
}

static boolean bansLoaded = false;
// If you're a community contributor looking to improve how bans are written, please
// offer your changes back to our Git repository. Kart Krew reserve the right to
// utilise format numbers in use by community builds for different layouts.
#define BANFORMAT 1

void D_SaveBan(void)
{
	FILE *f;
	size_t i;
	const char *address, *mask;
	const char *username, *reason;
	const time_t curTime = time(NULL);
	time_t unbanTime = NO_BAN_TIME;
	const char *path = va("%s"PATHSEP"%s", srb2home, "ban.txt");

	if (bansLoaded != true)
	{
		// You didn't even get to ATTEMPT to load bans.txt.
		// Don't immediately save nothing over it.
		return;
	}

	f = fopen(path, "w");

	if (!f)
	{
		CONS_Alert(CONS_WARNING, M_GetText("Could not save ban list into ban.txt\n"));
		return;
	}

	// Add header.
	fprintf(f, "BANFORMAT %d\n", BANFORMAT);

	for (i = 0; (address = I_GetBanAddress(i)) != NULL; i++)
	{
		if (I_GetUnbanTime)
		{
			unbanTime = I_GetUnbanTime(i);
		}
		else
		{
			unbanTime = NO_BAN_TIME;
		}

		if (unbanTime != NO_BAN_TIME && curTime >= unbanTime)
		{
			// This one has served their sentence.
			// We don't need to save them in the file anymore.
			continue;
		}

		mask = NULL;
		if (!I_GetBanMask || (mask = I_GetBanMask(i)) == NULL)
			fprintf(f, "%s/0", address);
		else
			fprintf(f, "%s/%s", address, mask);

		// TODO: it'd be nice to convert this to an actual date-time,
		// so it'd be easier to edit outside of the game.
		fprintf(f, " %ld", (long)unbanTime);

		username = NULL;
		if (I_GetBanUsername && (username = I_GetBanUsername(i)) != NULL)
			fprintf(f, " \"%s\"", username);
		else
			fprintf(f, " \"%s\"", "Direct IP ban");

		reason = NULL;
		if (I_GetBanReason && (reason = I_GetBanReason(i)) != NULL)
			fprintf(f, " \"%s\"\n", reason);
		else
			fprintf(f, " \"%s\"\n", "No reason given");
	}

	fclose(f);
}

static void Command_ClearBans(void)
{
	if (!I_ClearBans)
		return;

	I_ClearBans();
	D_SaveBan();
}

void D_LoadBan(boolean warning)
{
	FILE *f;
	size_t i, j;
	char *address, *mask;
	char *username, *reason;
	time_t unbanTime = NO_BAN_TIME;
	char buffer[MAX_WADPATH];
	UINT8 banmode = 0;
	boolean malformed = false;

	if (!I_ClearBans)
		return;

	// We at least attempted loading bans.txt
	bansLoaded = true;

	f = fopen(va("%s"PATHSEP"%s", srb2home, "ban.txt"), "r");

	if (!f)
	{
		if (warning)
			CONS_Alert(CONS_WARNING, M_GetText("Could not open ban.txt for ban list\n"));
		return;
	}

	I_ClearBans();

	for (i = 0; fgets(buffer, (int)sizeof(buffer), f); i++)
	{
		address = strtok(buffer, " /\t\r\n");
		mask = strtok(NULL, " \t\r\n");

		if (i == 0 && !strncmp(address, "BANFORMAT", 9))
		{
			if (mask)
			{
				banmode = atoi(mask);
			}
			switch (banmode)
			{
				case BANFORMAT: // currently supported format
				//case 0: -- permitted only when BANFORMAT string not present
					break;
				default:
				{
					fclose(f);
					CONS_Alert(CONS_WARNING, "Could not load unknown ban.txt for ban list (BANFORMAT %s, expected %d)\n", mask, BANFORMAT);
					return;
				}
			}
			continue;
		}

		if (I_SetBanAddress(address, mask) == false) // invalid IP input?
		{
			CONS_Alert(CONS_WARNING, "\"%s/%s\" is not a valid IP address, discarding...\n", address, mask);
			continue;
		}

		// One-way legacy format conversion -- the game will crash otherwise
		if (banmode == 0)
		{
			unbanTime = NO_BAN_TIME;
			username = NULL; // not guaranteed to be accurate, but only sane substitute
			reason = strtok(NULL, "\r\n");
			if (reason && reason[0] == 'N' && reason[1] == 'A' && reason[2] == '\0')
			{
				reason = NULL;
			}
		}
		else
		{
			reason = strtok(NULL, " \"\t\r\n");
			if (reason)
			{
				unbanTime = atoi(reason);
				reason = NULL;
			}
			else
			{
				unbanTime = NO_BAN_TIME;
				malformed = true;
			}

			username = strtok(NULL, "\"\t\r\n"); // go until next "
			if (!username)
			{
				malformed = true;
			}

			strtok(NULL, "\"\t\r\n"); // remove first "
			reason = strtok(NULL, "\"\r\n"); // go until next "
			if (!reason)
			{
				malformed = true;
			}
		}

		// Enforce MAX_REASONLENGTH.
		if (reason)
		{
			j = 0;
			while (reason[j] != '\0')
			{
				if ((j++) < MAX_REASONLENGTH)
					continue;
				reason[j] = '\0';
				break;
			}
		}

		if (I_SetUnbanTime)
			I_SetUnbanTime(unbanTime);

		if (I_SetBanUsername)
			I_SetBanUsername(username);

		if (I_SetBanReason)
			I_SetBanReason(reason);
	}

	if (malformed)
	{
		CONS_Alert(CONS_WARNING, "One or more lines of ban.txt are malformed. The game can correct for this, but some data may be lost.\n");
	}

	fclose(f);
}

#undef BANFORMAT

static void Command_ReloadBan(void)  //recheck ban.txt
{
	D_LoadBan(true);
}

static void Command_connect(void)
{

	// By default, clear the saved address that we'd save after succesfully joining just to be sure:
	joinedIP[0] = '\0';

	if (COM_Argc() < 2 || *COM_Argv(1) == 0)
	{
		CONS_Printf(M_GetText(
			"Connect <serveraddress> (port): connect to a server\n"
			"Connect ANY: connect to the first lan server found\n"
			//"Connect SELF: connect to your own server.\n"
			));
		return;
	}

	if (Playing() || demo.title)
	{
		CONS_Printf(M_GetText("You cannot connect while in a game. End this game first.\n"));
		return;
	}

	// modified game check: no longer handled
	// we don't request a restart unless the filelist differs

	server = false;
/*
	if (!stricmp(COM_Argv(1), "self"))
	{
		servernode = 0;
		server = true;
		/// \bug should be but...
		//SV_SpawnServer();
	}
	else
*/
	{
		// used in menu to connect to a server in the list
		if (netgame && !stricmp(COM_Argv(1), "node"))
		{
			servernode = (SINT8)atoi(COM_Argv(2));
		}
		else if (netgame)
		{
			CONS_Printf(M_GetText("You cannot connect while in a game. End this game first.\n"));
			return;
		}
		else if (I_NetOpenSocket)
		{
			I_NetOpenSocket();
			netgame = true;
			multiplayer = true;

			if (!stricmp(COM_Argv(1), "any"))
				servernode = BROADCASTADDR;
			else if (I_NetMakeNodewPort)
			{
				if (COM_Argc() >= 3) // address AND port
					servernode = I_NetMakeNodewPort(COM_Argv(1), COM_Argv(2));
				else // address only, or address:port
					servernode = I_NetMakeNode(COM_Argv(1));

				// Last IPs joined:
				// Keep the address we typed in memory so that we can save it if we *succesfully* join the server
				strlcpy(joinedIP, COM_Argv(1), MAX_LOGIP);
			}
			else
			{
				CONS_Alert(CONS_ERROR, M_GetText("There is no server identification with this network driver\n"));
				D_CloseConnection();
				return;
			}
		}
		else
			CONS_Alert(CONS_ERROR, M_GetText("There is no network driver\n"));
	}

	if (splitscreen != cv_splitplayers.value-1)
	{
		splitscreen = cv_splitplayers.value-1;
		SplitScreen_OnChange();
	}

	// Menu restore state.
	restoreMenu = &PLAY_MP_OptSelectDef;
	S_ChangeMusicInternal("NETMD2", true);
	if (setup_numplayers == 0)
	{
		setup_numplayers = 1;
	}

	CL_ConnectToServer();
}

static void ResetNode(INT32 node);

//
// CL_ClearPlayer
//
// Clears the player data so that a future client can use this slot
//
void CL_ClearPlayer(INT32 playernum)
{
	int i;

	// Handle mobj_t pointers.
	if (G_GamestateUsesLevel() == true)
	{
		if (players[playernum].follower)
		{
			K_RemoveFollower(&players[playernum]);
		}

		if (players[playernum].mo)
		{
			P_RemoveMobj(players[playernum].mo);
			P_SetTarget(&players[playernum].mo, NULL);
		}

		P_SetTarget(&players[playernum].skybox.viewpoint, NULL);
		P_SetTarget(&players[playernum].skybox.centerpoint, NULL);
		P_SetTarget(&players[playernum].awayview.mobj, NULL);
		P_SetTarget(&players[playernum].followmobj, NULL);
		P_SetTarget(&players[playernum].hoverhyudoro, NULL);
		P_SetTarget(&players[playernum].stumbleIndicator, NULL);
		P_SetTarget(&players[playernum].sliptideZipIndicator, NULL);
	}

	// Handle parties.
	for (i = 0; i < MAXPLAYERS; ++i)
	{
		if (splitscreen_invitations[i] == playernum)
			splitscreen_invitations[i] = -1;
	}
	splitscreen_invitations[playernum] = -1;
	splitscreen_party_size[playernum] = 0;
	splitscreen_original_party_size[playernum] = 0;

	// Wipe the struct.
	memset(&players[playernum], 0, sizeof (player_t));

	// Handle values which should not be initialised to 0.
	players[playernum].followerskin = -1; // don't have a ghost follower
	players[playernum].fakeskin = players[playernum].lastfakeskin = MAXSKINS; // don't avoid eggman

	// Handle post-cleanup.
	RemoveAdminPlayer(playernum); // don't stay admin after you're gone
}

//
// CL_RemovePlayer
//
// Removes a player from the current game
//
void CL_RemovePlayer(INT32 playernum, kickreason_t reason)
{
	// Sanity check: exceptional cases (i.e. c-fails) can cause multiple
	// kick commands to be issued for the same player.
	if (!playeringame[playernum])
		return;

	if (server && !demo.playback && playernode[playernum] != UINT8_MAX && !players[playernum].bot)
	{
		INT32 node = playernode[playernum];
		//playerpernode[node] = 0; // It'd be better to remove them all at once, but ghosting happened, so continue to let CL_RemovePlayer do it one-by-one
		playerpernode[node]--;
		if (playerpernode[node] <= 0)
		{
			nodeingame[node] = false;
			Net_CloseConnection(node);
			ResetNode(node);
		}
	}

	K_CalculateBattleWanted();

	LUA_HookPlayerQuit(&players[playernum], reason); // Lua hook for player quitting

	G_RemovePartyMember(playernum);

	// Reset player data
	CL_ClearPlayer(playernum);

	// remove avatar of player
	playeringame[playernum] = false;
	demo_extradata[playernum] |= DXD_PLAYSTATE;
	playernode[playernum] = UINT8_MAX;
	while (!playeringame[doomcom->numslots-1] && doomcom->numslots > 1)
		doomcom->numslots--;

	// Reset the name
	sprintf(player_names[playernum], "Player %c", 'A' + playernum);

	player_name_changes[playernum] = 0;

	LUA_InvalidatePlayer(&players[playernum]);

	// don't look through someone's view who isn't there
	G_ResetViews();

	K_CheckBumpers();
	P_CheckRacers();
}

void CL_Reset(void)
{
	if (metalrecording)
		G_StopMetalRecording(false);
	if (metalplayback)
		G_StopMetalDemo();
	if (demo.recording)
		G_CheckDemoStatus();

	// reset client/server code
	DEBFILE(va("\n-=-=-=-=-=-=-= Client reset =-=-=-=-=-=-=-\n\n"));

	if (servernode > 0 && servernode < MAXNETNODES)
	{
		nodeingame[(UINT8)servernode] = false;
		Net_CloseConnection(servernode);
	}
	D_CloseConnection(); // netgame = false
	multiplayer = false;
	servernode = 0;
	server = true;
	doomcom->numnodes = 1;
	doomcom->numslots = 1;
	SV_StopServer();
	SV_ResetServer();

	// make sure we don't leave any fileneeded gunk over from a failed join
	fileneedednum = 0;
	memset(fileneeded, 0, sizeof(fileneeded));

	totalfilesrequestednum = 0;
	totalfilesrequestedsize = 0;
	firstconnectattempttime = 0;
	serverisfull = false;
	connectiontimeout = (tic_t)cv_nettimeout.value; //reset this temporary hack

	expectChallenge = false;

#ifdef HAVE_CURL
	curl_failedwebdownload = false;
	curl_transfers = 0;
	curl_running = false;
	http_source[0] = '\0';
#endif

	// D_StartTitle should get done now, but the calling function will handle it
}

static void Command_GetPlayerNum(void)
{
	INT32 i;

	for (i = 0; i < MAXPLAYERS; i++)
		if (playeringame[i])
		{
			if (serverplayer == i)
				CONS_Printf(M_GetText("num:%2d  node:%2d  %s\n"), i, playernode[i], player_names[i]);
			else
				CONS_Printf(M_GetText("\x82num:%2d  node:%2d  %s\n"), i, playernode[i], player_names[i]);
		}
}

SINT8 nametonum(const char *name)
{
	INT32 playernum, i;

	if (!strcmp(name, "0"))
		return 0;

	playernum = (SINT8)atoi(name);

	if (playernum < 0 || playernum >= MAXPLAYERS)
		return -1;

	if (playernum)
	{
		if (playeringame[playernum])
			return (SINT8)playernum;
		else
			return -1;
	}

	for (i = 0; i < MAXPLAYERS; i++)
		if (playeringame[i] && !stricmp(player_names[i], name))
			return (SINT8)i;

	CONS_Printf(M_GetText("There is no player named \"%s\"\n"), name);

	return -1;
}

/** Lists all players and their player numbers.
  *
  * \sa Command_GetPlayerNum
  */
static void Command_Nodes(void)
{
	INT32 i;
	size_t maxlen = 0;
	const char *address;

	for (i = 0; i < MAXPLAYERS; i++)
	{
		const size_t plen = strlen(player_names[i]);
		if (playeringame[i] && plen > maxlen)
			maxlen = plen;
	}

	for (i = 0; i < MAXPLAYERS; i++)
	{
		if (playeringame[i])
		{
			CONS_Printf("%.2u: %*s", i, (int)maxlen, player_names[i]);

			if (playernode[i] != UINT8_MAX)
			{
				CONS_Printf(" - node %.2d", playernode[i]);
				if (I_GetNodeAddress && (address = I_GetNodeAddress(playernode[i])) != NULL)
					CONS_Printf(" - %s", address);
			}

			CONS_Printf(" [RRID-%s] ", GetPrettyRRID(players[i].public_key, true));

			if (IsPlayerAdmin(i))
				CONS_Printf(M_GetText(" (verified admin)"));

			if (players[i].spectator)
				CONS_Printf(M_GetText(" (spectator)"));

			CONS_Printf("\n");
		}
	}
}

static void Command_Ban(void)
{
	if (COM_Argc() < 2)
	{
		CONS_Printf(M_GetText("ban <playername/playernum> <reason>: ban and kick a player\n"));
		return;
	}

	if (!netgame) // Don't kick Tails in splitscreen!
	{
		CONS_Printf(M_GetText("This only works in a netgame.\n"));
		return;
	}

	if (server || IsPlayerAdmin(consoleplayer))
	{
		UINT8 buf[3 + MAX_REASONLENGTH];
		UINT8 *p = buf;
		const SINT8 pn = nametonum(COM_Argv(1));

		if (pn == -1 || pn == 0)
			return;

		WRITEUINT8(p, pn);

		if (COM_Argc() == 2)
		{
			WRITEUINT8(p, KICK_MSG_BANNED);
			SendNetXCmd(XD_KICK, &buf, 2);
		}
		else
		{
			size_t i, j = COM_Argc();
			char message[MAX_REASONLENGTH];

			//Steal from the motd code so you don't have to put the reason in quotes.
			strlcpy(message, COM_Argv(2), sizeof message);
			for (i = 3; i < j; i++)
			{
				strlcat(message, " ", sizeof message);
				strlcat(message, COM_Argv(i), sizeof message);
			}

			WRITEUINT8(p, KICK_MSG_CUSTOM_BAN);
			WRITESTRINGN(p, message, MAX_REASONLENGTH);
			SendNetXCmd(XD_KICK, &buf, p - buf);
		}
	}
	else
		CONS_Printf(M_GetText("Only the server or a remote admin can use this.\n"));
}

static void Command_BanIP(void)
{
	size_t ac = COM_Argc();

	if (ac < 2)
	{
		CONS_Printf(M_GetText("banip <ip> [<reason>]: ban an ip address\n"));
		return;
	}

	if (server) // Only the server can use this, otherwise does nothing.
	{
		char *addressInput = Z_StrDup(COM_Argv(1));

		const char *address = NULL;
		const char *mask = NULL;

		const char *reason = NULL;

		address = strtok(addressInput, "/");
		mask = strtok(NULL, "");

		if (ac > 2)
		{
			reason = COM_Argv(2);
		}

		if (I_SetBanAddress && I_SetBanAddress(address, mask))
		{
			if (reason)
			{
				CONS_Printf(
					"Banned IP address %s%s for: %s\n",
					address,
					(mask && (strlen(mask) > 0)) ? va("/%s", mask) : "",
					reason
				);
			}
			else
			{
				CONS_Printf(
					"Banned IP address %s%s\n",
					address,
					(mask && (strlen(mask) > 0)) ? va("/%s", mask) : ""
				);
			}

			if (I_SetUnbanTime)
				I_SetUnbanTime(NO_BAN_TIME);

			if (I_SetBanUsername)
				I_SetBanUsername(NULL);

			if (I_SetBanReason)
				I_SetBanReason(reason);

			D_SaveBan();
		}
		else
		{
			return;
		}
	}
}

static void Command_Kick(void)
{
	if (COM_Argc() < 2)
	{
		CONS_Printf(M_GetText("kick <playername/playernum> <reason>: kick a player\n"));
		return;
	}

	if (!netgame) // Don't kick Tails in splitscreen!
	{
		CONS_Printf(M_GetText("This only works in a netgame.\n"));
		return;
	}

	if (server || IsPlayerAdmin(consoleplayer))
	{
		UINT8 buf[3 + MAX_REASONLENGTH];
		UINT8 *p = buf;
		const SINT8 pn = nametonum(COM_Argv(1));

		if (pn == -1 || pn == 0)
			return;

		// Special case if we are trying to kick a player who is downloading the game state:
		// trigger a timeout instead of kicking them, because a kick would only
		// take effect after they have finished downloading
		if (server && playernode[pn] != UINT8_MAX && sendingsavegame[playernode[pn]])
		{
			Net_ConnectionTimeout(playernode[pn]);
			return;
		}

		WRITESINT8(p, pn);

		if (COM_Argc() == 2)
		{
			WRITEUINT8(p, KICK_MSG_GO_AWAY);
			SendNetXCmd(XD_KICK, &buf, 2);
		}
		else
		{
			size_t i, j = COM_Argc();
			char message[MAX_REASONLENGTH];

			//Steal from the motd code so you don't have to put the reason in quotes.
			strlcpy(message, COM_Argv(2), sizeof message);
			for (i = 3; i < j; i++)
			{
				strlcat(message, " ", sizeof message);
				strlcat(message, COM_Argv(i), sizeof message);
			}

			WRITEUINT8(p, KICK_MSG_CUSTOM_KICK);
			WRITESTRINGN(p, message, MAX_REASONLENGTH);
			SendNetXCmd(XD_KICK, &buf, p - buf);
		}
	}
	else
		CONS_Printf(M_GetText("Only the server or a remote admin can use this.\n"));
}

static void Got_KickCmd(UINT8 **p, INT32 playernum)
{
	INT32 pnum, msg;
	char buf[3 + MAX_REASONLENGTH];
	char *reason = buf;
	kickreason_t kickreason = KR_KICK;
	UINT32 banMinutes = 0;

	pnum = READUINT8(*p);
	msg = READUINT8(*p);

	if (pnum == serverplayer && IsPlayerAdmin(playernum))
	{
		CONS_Printf(M_GetText("Server is being shut down remotely. Goodbye!\n"));

		if (server)
			COM_BufAddText("quit\n");

		return;
	}

	// Is playernum authorized to make this kick?
	if (playernum != serverplayer && !IsPlayerAdmin(playernum)
		/*&& !(playernode[playernum] != UINT8_MAX && playerpernode[playernode[playernum]] == 2
		&& nodetoplayer2[playernode[playernum]] == pnum)*/)
	{
		// We received a kick command from someone who isn't the
		// server or admin, and who isn't in splitscreen removing
		// player 2. Thus, it must be someone with a modified
		// binary, trying to kick someone but without having
		// authorization.

		// We deal with this by changing the kick reason to
		// "consistency failure" and kicking the offending user
		// instead.

		CONS_Alert(CONS_WARNING, M_GetText("Illegal kick command received from %s for player %d\n"), player_names[playernum], pnum);

		// In debug, print a longer message with more details.
		// TODO Callum: Should we translate this?
/*
		CONS_Debug(DBG_NETPLAY,
			"So, you must be asking, why is this an illegal kick?\n"
			"Well, let's take a look at the facts, shall we?\n"
			"\n"
			"playernum (this is the guy who did it), he's %d.\n"
			"pnum (the guy he's trying to kick) is %d.\n"
			"playernum's node is %d.\n"
			"That node has %d players.\n"
			"Player 2 on that node is %d.\n"
			"pnum's node is %d.\n"
			"That node has %d players.\n"
			"Player 2 on that node is %d.\n"
			"\n"
			"If you think this is a bug, please report it, including all of the details above.\n",
				playernum, pnum,
				playernode[playernum], playerpernode[playernode[playernum]],
				nodetoplayer2[playernode[playernum]],
				playernode[pnum], playerpernode[playernode[pnum]],
				nodetoplayer2[playernode[pnum]]);
*/
		pnum = playernum;
		msg = KICK_MSG_CON_FAIL;
	}

	if (msg == KICK_MSG_CUSTOM_BAN || msg == KICK_MSG_CUSTOM_KICK)
	{
		READSTRINGN(*p, reason, MAX_REASONLENGTH+1);
	}

	//CONS_Printf("\x82%s ", player_names[pnum]);

	// Save bans here. Used to be split between here and the actual command, depending on
	// whenever the server did it or a remote admin did it, but it's simply more convenient
	// to keep it all in one place.
	if (server)
	{
		if (msg == KICK_MSG_GO_AWAY || msg == KICK_MSG_CUSTOM_KICK)
		{
			// Kick as a temporary ban.
			banMinutes = cv_kicktime.value;
		}

		if (msg == KICK_MSG_BANNED || msg == KICK_MSG_CUSTOM_BAN || banMinutes)
		{
			if (I_Ban && !I_Ban(playernode[(INT32)pnum]))
			{
				CONS_Alert(CONS_WARNING, M_GetText("Ban failed. Invalid node?\n"));
			}
			else
			{
				if (I_SetBanUsername)
					I_SetBanUsername(player_names[pnum]);

				if (I_SetBanReason)
					I_SetBanReason(reason);

				if (I_SetUnbanTime)
				{
					if (banMinutes)
						I_SetUnbanTime(time(NULL) + (banMinutes * 60));
					else
						I_SetUnbanTime(NO_BAN_TIME);
				}

				D_SaveBan();
			}
		}
	}

	if (msg == KICK_MSG_PLAYER_QUIT)
		S_StartSound(NULL, sfx_leave); // intended leave
	else
		S_StartSound(NULL, sfx_syfail); // he he he

	switch (msg)
	{
		case KICK_MSG_GO_AWAY:
			HU_AddChatText(va("\x82*%s has been kicked (No reason given)", player_names[pnum]), false);
			kickreason = KR_KICK;
			break;
		case KICK_MSG_PING_HIGH:
			HU_AddChatText(va("\x82*%s left the game (Broke delay limit)", player_names[pnum]), false);
			kickreason = KR_PINGLIMIT;
			break;
		case KICK_MSG_CON_FAIL:
			HU_AddChatText(va("\x82*%s left the game (Synch failure)", player_names[pnum]), false);
			kickreason = KR_SYNCH;

			if (M_CheckParm("-consisdump")) // Helps debugging some problems
			{
				INT32 i;

				CONS_Printf(M_GetText("Player kicked is #%d, dumping consistency...\n"), pnum);

				for (i = 0; i < MAXPLAYERS; i++)
				{
					if (!playeringame[i])
						continue;
					CONS_Printf("-------------------------------------\n");
					CONS_Printf("Player %d: %s\n", i, player_names[i]);
					CONS_Printf("Skin: %d\n", players[i].skin);
					CONS_Printf("Color: %d\n", players[i].skincolor);
					CONS_Printf("Speed: %d\n",players[i].speed>>FRACBITS);
					if (players[i].mo)
					{
						if (!players[i].mo->skin)
							CONS_Printf("Mobj skin: NULL!\n");
						else
							CONS_Printf("Mobj skin: %s\n", ((skin_t *)players[i].mo->skin)->name);
						CONS_Printf("Position: %d, %d, %d\n", players[i].mo->x, players[i].mo->y, players[i].mo->z);
						if (!players[i].mo->state)
							CONS_Printf("State: S_NULL\n");
						else
							CONS_Printf("State: %d\n", (statenum_t)(players[i].mo->state-states));
					}
					else
						CONS_Printf("Mobj: NULL\n");
					CONS_Printf("-------------------------------------\n");
				}
			}
			break;
		case KICK_MSG_TIMEOUT:
			HU_AddChatText(va("\x82*%s left the game (Connection timeout)", player_names[pnum]), false);
			kickreason = KR_TIMEOUT;
			break;
		case KICK_MSG_SIGFAIL:
			HU_AddChatText(va("\x82*%s left the game (Invalid signature)", player_names[pnum]), false);
			kickreason = KR_TIMEOUT;
			break;
		case KICK_MSG_PLAYER_QUIT:
			if (netgame) // not splitscreen/bots
				HU_AddChatText(va("\x82*%s left the game", player_names[pnum]), false);
			kickreason = KR_LEAVE;
			break;
		case KICK_MSG_BANNED:
			HU_AddChatText(va("\x82*%s has been banned (No reason given)", player_names[pnum]), false);
			kickreason = KR_BAN;
			break;
		case KICK_MSG_CUSTOM_KICK:
			HU_AddChatText(va("\x82*%s has been kicked (%s)", player_names[pnum], reason), false);
			kickreason = KR_KICK;
			break;
		case KICK_MSG_CUSTOM_BAN:
			HU_AddChatText(va("\x82*%s has been banned (%s)", player_names[pnum], reason), false);
			kickreason = KR_BAN;
			break;
	}

	// SRB2Kart: kicks count as forfeit
	switch (kickreason)
	{
		case KR_KICK:
		case KR_BAN:
		case KR_LEAVE:
			// Intentional removals should be hit with a true forfeit.
			K_PlayerForfeit(pnum, true);
			break;
		default:
			// Otherwise, give remaining players the point compensation, but doesn't penalize who left.
			K_PlayerForfeit(pnum, false);
			break;
	}

	if (playernode[pnum] == playernode[consoleplayer])
	{
#ifdef DUMPCONSISTENCY
		if (msg == KICK_MSG_CON_FAIL) SV_SavedGame();
#endif
		LUA_HookBool(false, HOOK(GameQuit)); //Lua hooks handled differently now

		D_QuitNetGame();
		CL_Reset();
		D_ClearState();
		M_StartControlPanel();

		if (msg == KICK_MSG_CON_FAIL)
			M_StartMessage(M_GetText("Server closed connection\n(Synch failure)\nPress (B)\n"), NULL, MM_NOTHING);
		else if (msg == KICK_MSG_PING_HIGH)
			M_StartMessage(M_GetText("Server closed connection\n(Broke delay limit)\nPress (B)\n"), NULL, MM_NOTHING);
		else if (msg == KICK_MSG_BANNED)
			M_StartMessage(M_GetText("You have been banned by the server\n\nPress (B)\n"), NULL, MM_NOTHING);
		else if (msg == KICK_MSG_CUSTOM_KICK)
			M_StartMessage(va(M_GetText("You have been kicked\n(%s)\nPress (B)\n"), reason), NULL, MM_NOTHING);
		else if (msg == KICK_MSG_CUSTOM_BAN)
			M_StartMessage(va(M_GetText("You have been banned\n(%s)\nPress (B)\n"), reason), NULL, MM_NOTHING);
		else if (msg == KICK_MSG_SIGFAIL)
			M_StartMessage(M_GetText("Server closed connection\n(Invalid signature)\nPress (B)\n"), NULL, MM_NOTHING);
		else
			M_StartMessage(M_GetText("You have been kicked by the server\n\nPress (B)\n"), NULL, MM_NOTHING);
	}
	else if (server)
	{
		// Sal: Because kicks (and a lot of other commands) are player-based, we can't tell which player pnum is on the node from a glance.
		// When we want to remove everyone from a node, we have to get the kicked player's node, then remove everyone on that node manually so we don't miss any.
		// This avoids the bugs with older SRB2 version's online splitscreen kicks, specifically ghosting.
		// On top of this, it can't just be a CL_RemovePlayer call; it has to be a server-sided.
		// Clients don't bother setting any nodes for anything but THE server player (even ignoring the server's extra players!), so it'll often remove everyone because they all have node -1/255, insta-desync!
		// And yes. This is a netxcmd wrap for just CL_RemovePlayer! :V

#define removethisplayer(otherp) \
	if (otherp >= 0) \
	{ \
		buf[0] = (UINT8)otherp; \
		if (otherp != pnum) \
		{ \
			HU_AddChatText(va("\x82*%s left the game (Joined with %s)", player_names[otherp], player_names[pnum]), false); \
			buf[1] = KR_LEAVE; \
		} \
		else \
			buf[1] = (UINT8)kickreason; \
		SendNetXCmd(XD_REMOVEPLAYER, &buf, 2); \
		otherp = -1; \
	}
		removethisplayer(nodetoplayer[playernode[pnum]])
		removethisplayer(nodetoplayer2[playernode[pnum]])
		removethisplayer(nodetoplayer3[playernode[pnum]])
		removethisplayer(nodetoplayer4[playernode[pnum]])
#undef removethisplayer
	}
}

#ifdef HAVE_CURL
/** Add a login for HTTP downloads. If the
  * user/password is missing, remove it.
  *
  * \sa Command_list_http_logins
  */
static void Command_set_http_login (void)
{
	HTTP_login  *login;
	HTTP_login **prev_next;

	if (COM_Argc() < 2)
	{
		CONS_Printf(
				"set_http_login <URL> [user:password]: Set or remove a login to "
				"authenticate HTTP downloads.\n"
		);
		return;
	}

	login = CURLGetLogin(COM_Argv(1), &prev_next);

	if (COM_Argc() == 2)
	{
		if (login)
		{
			(*prev_next) = login->next;
			CONS_Printf("Login for '%s' removed.\n", login->url);
			Z_Free(login);
		}
	}
	else
	{
		if (login)
			Z_Free(login->auth);
		else
		{
			login = ZZ_Alloc(sizeof *login);
			login->url  = Z_StrDup(COM_Argv(1));
		}

		login->auth = Z_StrDup(COM_Argv(2));

		login->next = curl_logins;
		curl_logins = login;
	}
}

/** List logins for HTTP downloads.
  *
  * \sa Command_set_http_login
  */
static void Command_list_http_logins (void)
{
	HTTP_login *login;

	for (
			login = curl_logins;
			login;
			login = login->next
	){
		CONS_Printf(
				"'%s' -> '%s'\n",
				login->url,
				login->auth
		);
	}
}
#endif/*HAVE_CURL*/

static void Command_ResendGamestate(void)
{
	SINT8 playernum;

	if (COM_Argc() == 1)
	{
		CONS_Printf(M_GetText("resendgamestate <playername/playernum>: resend the game state to a player\n"));
		return;
	}
	else if (client)
	{
		CONS_Printf(M_GetText("Only the server can use this.\n"));
		return;
	}

	playernum = nametonum(COM_Argv(1));
	if (playernum == -1 || playernum == 0)
		return;

	// Send a PT_WILLRESENDGAMESTATE packet to the client so they know what's going on
	netbuffer->packettype = PT_WILLRESENDGAMESTATE;
	if (!HSendPacket(playernode[playernum], true, 0, 0))
	{
		CONS_Alert(CONS_ERROR, M_GetText("A problem occured, please try again.\n"));
		return;
	}
}

static CV_PossibleValue_t netticbuffer_cons_t[] = {{0, "MIN"}, {3, "MAX"}, {0, NULL}};
consvar_t cv_netticbuffer = CVAR_INIT ("netticbuffer", "1", CV_SAVE, netticbuffer_cons_t, NULL);

static void Joinable_OnChange(void);

consvar_t cv_allownewplayer = CVAR_INIT ("allowjoin", "On", CV_SAVE|CV_CALL, CV_OnOff, Joinable_OnChange);

#ifdef VANILLAJOINNEXTROUND
consvar_t cv_joinnextround = CVAR_INIT ("joinnextround", "Off", CV_NETVAR, CV_OnOff, NULL); /// \todo not done
#endif

static CV_PossibleValue_t maxplayers_cons_t[] = {{2, "MIN"}, {MAXPLAYERS, "MAX"}, {0, NULL}};
consvar_t cv_maxconnections = CVAR_INIT ("maxconnections", "16", CV_SAVE|CV_CALL, maxplayers_cons_t, Joinable_OnChange);

static CV_PossibleValue_t joindelay_cons_t[] = {{1, "MIN"}, {3600, "MAX"}, {0, "Off"}, {0, NULL}};
consvar_t cv_joindelay = CVAR_INIT ("joindelay", "10", CV_SAVE|CV_NETVAR, joindelay_cons_t, NULL);

// Here for dedicated servers
static CV_PossibleValue_t discordinvites_cons_t[] = {{0, "Admins Only"}, {1, "Everyone"}, {0, NULL}};
consvar_t cv_discordinvites = CVAR_INIT ("discordinvites", "Everyone", CV_SAVE|CV_CALL, discordinvites_cons_t, Joinable_OnChange);

static CV_PossibleValue_t resynchattempts_cons_t[] = {{1, "MIN"}, {20, "MAX"}, {0, "No"}, {0, NULL}};

consvar_t cv_resynchattempts = CVAR_INIT ("resynchattempts", "2", CV_SAVE|CV_NETVAR, resynchattempts_cons_t, NULL);
consvar_t cv_blamecfail = CVAR_INIT ("blamecfail", "Off", CV_SAVE|CV_NETVAR, CV_OnOff, NULL);

// max file size to send to a player (in kilobytes)
static CV_PossibleValue_t maxsend_cons_t[] = {{0, "MIN"}, {51200, "MAX"}, {0, NULL}};
consvar_t cv_maxsend = CVAR_INIT ("maxsend", "51200", CV_SAVE|CV_NETVAR, maxsend_cons_t, NULL);
consvar_t cv_noticedownload = CVAR_INIT ("noticedownload", "Off", CV_SAVE|CV_NETVAR, CV_OnOff, NULL);

// Speed of file downloading (in packets per tic)
static CV_PossibleValue_t downloadspeed_cons_t[] = {{1, "MIN"}, {300, "MAX"}, {0, NULL}};
consvar_t cv_downloadspeed = CVAR_INIT ("downloadspeed", "32", CV_SAVE|CV_NETVAR, downloadspeed_cons_t, NULL);

static void Got_AddPlayer(UINT8 **p, INT32 playernum);
static void Got_RemovePlayer(UINT8 **p, INT32 playernum);
static void Got_AddBot(UINT8 **p, INT32 playernum);

static void Joinable_OnChange(void)
{
	UINT8 buf[3];
	UINT8 *p = buf;
	UINT8 maxplayer;

	if (!server)
		return;

	maxplayer = (UINT8)(min((dedicated ? MAXPLAYERS-1 : MAXPLAYERS), cv_maxconnections.value));

	WRITEUINT8(p, maxplayer);
	WRITEUINT8(p, cv_allownewplayer.value);
	WRITEUINT8(p, cv_discordinvites.value);

	SendNetXCmd(XD_DISCORD, &buf, 3);
}

// called one time at init
void D_ClientServerInit(void)
{
	DEBFILE(va("- - -== Ring Racers v%d.%d "VERSIONSTRING" debugfile ==- - -\n",
		VERSION, SUBVERSION));

	COM_AddCommand("getplayernum", Command_GetPlayerNum);
	COM_AddCommand("kick", Command_Kick);
	COM_AddCommand("ban", Command_Ban);
	COM_AddCommand("banip", Command_BanIP);
	COM_AddCommand("clearbans", Command_ClearBans);
	COM_AddCommand("showbanlist", Command_ShowBan);
	COM_AddCommand("reloadbans", Command_ReloadBan);
	COM_AddCommand("connect", Command_connect);
	COM_AddCommand("nodes", Command_Nodes);
#ifdef HAVE_CURL
	COM_AddCommand("set_http_login", Command_set_http_login);
	COM_AddCommand("list_http_logins", Command_list_http_logins);
#endif
	COM_AddCommand("resendgamestate", Command_ResendGamestate);
#ifdef PACKETDROP
	COM_AddCommand("drop", Command_Drop);
	COM_AddCommand("droprate", Command_Droprate);
#endif
	COM_AddCommand("numnodes", Command_Numnodes);

	RegisterNetXCmd(XD_KICK, Got_KickCmd);
	RegisterNetXCmd(XD_ADDPLAYER, Got_AddPlayer);
	RegisterNetXCmd(XD_REMOVEPLAYER, Got_RemovePlayer);
	RegisterNetXCmd(XD_ADDBOT, Got_AddBot);
#ifdef DUMPCONSISTENCY
	CV_RegisterVar(&cv_dumpconsistency);
#endif
	D_LoadBan(false);

	gametic = 0;
	localgametic = 0;

	// do not send anything before the real begin
	SV_StopServer();
	SV_ResetServer();
	if (dedicated)
		SV_SpawnServer();
}

static void ResetNode(INT32 node)
{
	nodeingame[node] = false;
	nodewaiting[node] = 0;
	nodeneedsauth[node] = false;
	//CONS_Printf("2: node %d -> %d\n", node, nodeneedsauth[node]);

	nettics[node] = gametic;
	supposedtics[node] = gametic;

	nodetoplayer[node] = -1;
	nodetoplayer2[node] = -1;
	nodetoplayer3[node] = -1;
	nodetoplayer4[node] = -1;
	playerpernode[node] = 0;

	sendingsavegame[node] = false;
	resendingsavegame[node] = false;
	savegameresendcooldown[node] = 0;

	bannednode[node].banid = SIZE_MAX;
	bannednode[node].timeleft = NO_BAN_TIME;
}

void SV_ResetServer(void)
{
	INT32 i;

	// +1 because this command will be executed in com_executebuffer in
	// tryruntic so gametic will be incremented, anyway maketic > gametic
	// is not an issue

	maketic = gametic + 1;
	neededtic = maketic;
	tictoclear = maketic;

	joindelay = 0;

	for (i = 0; i < MAXNETNODES; i++)
		ResetNode(i);

	for (i = 0; i < MAXPLAYERS; i++)
	{
		LUA_InvalidatePlayer(&players[i]);
		sprintf(player_names[i], "Player %c", 'A' + i);
	}

	memset(playeringame, false, sizeof playeringame);
	memset(playernode, UINT8_MAX, sizeof playernode);

	ClearAdminPlayers();
	Schedule_Clear();
	Automate_Clear();
	K_ClearClientPowerLevels();

	memset(splitscreen_invitations, -1, sizeof splitscreen_invitations);
	memset(splitscreen_partied, 0, sizeof splitscreen_partied);
	memset(player_name_changes, 0, sizeof player_name_changes);

	mynode = 0;
	cl_packetmissed = false;
	cl_redownloadinggamestate = false;

	if (dedicated)
	{
		nodeingame[0] = true;
		serverplayer = 0;
	}
	else
		serverplayer = consoleplayer;

	if (server)
		servernode = 0;

	doomcom->numslots = 0;

	// clear server_context
	memset(server_context, '-', 8);

	CV_RevertNetVars();

	// Copy our unlocks to a place where net material can grab at/overwrite them safely.
	// (permits all unlocks in dedicated)
	for (i = 0; i < MAXUNLOCKABLES; i++)
		netUnlocked[i] = (dedicated || gamedata->unlocked[i]);

	expectChallenge = false;

	DEBFILE("\n-=-=-=-=-=-=-= Server Reset =-=-=-=-=-=-=-\n\n");
}

static inline void SV_GenContext(void)
{
	UINT8 i;
	// generate server_context, as exactly 8 bytes of randomly mixed A-Z and a-z
	// (hopefully M_Random is initialized!! if not this will be awfully silly!)
	for (i = 0; i < 8; i++)
	{
		const char a = M_RandomKey(26*2);
		if (a < 26) // uppercase
			server_context[i] = 'A'+a;
		else // lowercase
			server_context[i] = 'a'+(a-26);
	}
}

//
// D_QuitNetGame
// Called before quitting to leave a net game
// without hanging the other players
//
void D_QuitNetGame(void)
{
	if (!netgame || !netbuffer)
		return;

	DEBFILE("===========================================================================\n"
	        "                  Quitting Game, closing connection\n"
	        "===========================================================================\n");

	// abort send/receive of files
	CloseNetFile();
	RemoveAllLuaFileTransfers();
	waitingforluafiletransfer = false;
	waitingforluafilecommand = false;

	if (server)
	{
		INT32 i;

		netbuffer->packettype = PT_SERVERSHUTDOWN;
		for (i = 0; i < MAXNETNODES; i++)
			if (nodeingame[i])
				HSendPacket(i, true, 0, 0);
#ifdef MASTERSERVER
		if (serverrunning && netgame && cv_advertise.value) // see mserv.c Online()
			UnregisterServer();
#endif
	}
	else if (servernode > 0 && servernode < MAXNETNODES && nodeingame[(UINT8)servernode])
	{
		netbuffer->packettype = PT_CLIENTQUIT;
		HSendPacket(servernode, true, 0, 0);
	}

	D_CloseConnection();
	ClearAdminPlayers();
	Schedule_Clear();
	Automate_Clear();
	K_ClearClientPowerLevels();

	DEBFILE("===========================================================================\n"
	        "                         Log finish\n"
	        "===========================================================================\n");
#ifdef DEBUGFILE
	if (debugfile)
	{
		fclose(debugfile);
		debugfile = NULL;
	}
#endif
}

// Adds a node to the game (player will follow at map change or at savegame....)
static inline void SV_AddNode(INT32 node)
{
	nettics[node] = gametic;
	supposedtics[node] = gametic;
	// little hack because the server connects to itself and puts
	// nodeingame when connected not here
	if (node)
		nodeingame[node] = true;

	nodeneedsauth[node] = false;
	CONS_Printf("3: node %d -> %d\n", node, nodeneedsauth[node]);
}

// Xcmd XD_ADDPLAYER
static void Got_AddPlayer(UINT8 **p, INT32 playernum)
{
	INT16 node, newplayernum;
	UINT8 console;
	UINT8 splitscreenplayer = 0;
	UINT8 i;
	player_t *newplayer;

	if (playernum != serverplayer && !IsPlayerAdmin(playernum))
	{
		// protect against hacked/buggy client
		CONS_Alert(CONS_WARNING, M_GetText("Illegal add player command received from %s\n"), player_names[playernum]);
		if (server)
			SendKick(playernum, KICK_MSG_CON_FAIL);
		return;
	}

	node = READUINT8(*p);
	newplayernum = READUINT8(*p);

	CONS_Debug(DBG_NETPLAY, "addplayer: %d %d\n", node, newplayernum);

	{
		// Clear player before joining, lest some things get set incorrectly
		CL_ClearPlayer(newplayernum);

		playeringame[newplayernum] = true;
		G_AddPlayer(newplayernum);

		if (newplayernum+1 > doomcom->numslots)
			doomcom->numslots = (INT16)(newplayernum+1);
	}

	newplayer = &players[newplayernum];

	newplayer->jointime = 0;

	READSTRINGN(*p, player_names[newplayernum], MAXPLAYERNAME);
	READSTRINGN(*p, players[newplayernum].public_key, 32);

	console = READUINT8(*p);
	splitscreenplayer = READUINT8(*p);

	for (i = 0; i < MAXAVAILABILITY; i++)
	{
		newplayer->availabilities[i] = READUINT8(*p);
	}

	// the server is creating my player
	if (node == mynode)
	{
		playernode[newplayernum] = 0; // for information only

		if (splitscreenplayer)
		{
			displayplayers[splitscreenplayer] = newplayernum;
			g_localplayers[splitscreenplayer] = newplayernum;
			DEBFILE(va("spawning sister # %d\n", splitscreenplayer));
		}
		else
		{
			consoleplayer = newplayernum;
			for (i = 0; i < MAXSPLITSCREENPLAYERS; i++)
			{
				displayplayers[i] = newplayernum;
				g_localplayers[i] = newplayernum;
			}
			splitscreen_partied[newplayernum] = true;
			DEBFILE("spawning me\n");
		}

		P_ForceLocalAngle(newplayer, newplayer->angleturn);

		D_SendPlayerConfig(splitscreenplayer);
		addedtogame = true;
	}

	players[newplayernum].splitscreenindex = splitscreenplayer;
	players[newplayernum].bot = false;

	playerconsole[newplayernum] = console;
	splitscreen_original_party_size[console] =
		++splitscreen_party_size[console];
	splitscreen_original_party[console][splitscreenplayer] =
		splitscreen_party[console][splitscreenplayer] = newplayernum;

	if (netgame)
	{
		char joinmsg[256];

		strcpy(joinmsg, M_GetText("\x82*%s has joined the game (player %d)"));
		strcpy(joinmsg, va(joinmsg, player_names[newplayernum], newplayernum));

		if (node != mynode)
			S_StartSound(NULL, sfx_join);

		// Merge join notification + IP to avoid clogging console/chat
		if (server && cv_showjoinaddress.value && I_GetNodeAddress)
		{
			const char *address = I_GetNodeAddress(node);
			if (address)
				strcat(joinmsg, va(" (%s)", address));
		}

		HU_AddChatText(joinmsg, false);
	}

	if (server && multiplayer && motd[0] != '\0')
		COM_BufAddText(va("sayto %d %s\n", newplayernum, motd));

	LUA_HookInt(newplayernum, HOOK(PlayerJoin));

#ifdef HAVE_DISCORDRPC
	DRPC_UpdatePresence();
#endif
}

// Xcmd XD_REMOVEPLAYER
static void Got_RemovePlayer(UINT8 **p, INT32 playernum)
{
	SINT8 pnum, reason;

	if (playernum != serverplayer && !IsPlayerAdmin(playernum))
	{
		// protect against hacked/buggy client
		CONS_Alert(CONS_WARNING, M_GetText("Illegal remove player command received from %s\n"), player_names[playernum]);
		if (server)
		{
			SendKick(playernum, KICK_MSG_CON_FAIL);
		}
		return;
	}

	pnum = READUINT8(*p);
	reason = READUINT8(*p);

	CL_RemovePlayer(pnum, reason);

#ifdef HAVE_DISCORDRPC
	DRPC_UpdatePresence();
#endif
}

// Xcmd XD_ADDBOT
// Compacted version of XD_ADDPLAYER for simplicity
static void Got_AddBot(UINT8 **p, INT32 playernum)
{
	INT16 newplayernum;
	UINT8 skinnum = 0;
	UINT8 difficulty = DIFFICULTBOT;

	if (playernum != serverplayer && !IsPlayerAdmin(playernum))
	{
		// protect against hacked/buggy client
		CONS_Alert(CONS_WARNING, M_GetText("Illegal add player command received from %s\n"), player_names[playernum]);
		if (server)
		{
			SendKick(playernum, KICK_MSG_CON_FAIL);
		}
		return;
	}

	newplayernum = READUINT8(*p);
	skinnum = READUINT8(*p);
	difficulty = READUINT8(*p);

	CONS_Debug(DBG_NETPLAY, "addbot: %d\n", newplayernum);

	// Clear player before joining, lest some things get set incorrectly
	CL_ClearPlayer(newplayernum);

	playeringame[newplayernum] = true;
	G_AddPlayer(newplayernum);
	if (newplayernum+1 > doomcom->numslots)
		doomcom->numslots = (INT16)(newplayernum+1);

	playernode[newplayernum] = servernode;

	// this will permit unlocks
	memcpy(&players[newplayernum].availabilities, R_GetSkinAvailabilities(false, true), MAXAVAILABILITY*sizeof(UINT8));

	players[newplayernum].splitscreenindex = 0;
	players[newplayernum].bot = true;
	players[newplayernum].botvars.difficulty = difficulty;
	players[newplayernum].lives = 9;

	players[newplayernum].skincolor = skins[skinnum].prefcolor;
	sprintf(player_names[newplayernum], "%s", skins[skinnum].realname);
	SetPlayerSkinByNum(newplayernum, skinnum);

	if (netgame)
	{
		HU_AddChatText(va("\x82*Bot %d has been added to the game", newplayernum+1), false);
	}

	LUA_HookInt(newplayernum, HOOK(PlayerJoin));
}

static boolean SV_AddWaitingPlayers(SINT8 node, UINT8 *availabilities, 
const char *name, uint8_t *key, const char *name2, uint8_t *key2,
const char *name3, uint8_t *key3, const char *name4, uint8_t *key4)
{
	INT32 n, newplayernum, i;
	UINT8 buf[4 + MAXPLAYERNAME + 32 + MAXAVAILABILITY];
	UINT8 *buf_p = buf;
	boolean newplayer = false;

	{
		// splitscreen can allow 2+ players in one node
		for (; nodewaiting[node] > 0; nodewaiting[node]--)
		{
			newplayer = true;

			{
				UINT8 nobotoverwrite;

				// search for a free playernum
				// we can't solely use playeringame since it is not updated here
				for (newplayernum = dedicated ? 1 : 0; newplayernum < MAXPLAYERS; newplayernum++)
				{
					if (playeringame[newplayernum])
						continue;

					for (n = 0; n < MAXNETNODES; n++)
						if (nodetoplayer[n] == newplayernum
						|| nodetoplayer2[n] == newplayernum
						|| nodetoplayer3[n] == newplayernum
						|| nodetoplayer4[n] == newplayernum)
							break;

					if (n == MAXNETNODES)
						break;
				}

				nobotoverwrite = newplayernum;

				while (playeringame[nobotoverwrite]
				&& players[nobotoverwrite].bot
				&& nobotoverwrite < MAXPLAYERS)
				{
					// Overwrite bots if there are NO other slots available.
					nobotoverwrite++;
				}

				if (nobotoverwrite < MAXPLAYERS)
				{
					newplayernum = nobotoverwrite;
				}
			}

			// should never happen since we check the playernum
			// before accepting the join
			I_Assert(newplayernum < MAXPLAYERS);

			playernode[newplayernum] = (UINT8)node;

			// Reset the buffer to the start for multiple joiners
			buf_p = buf;

			WRITEUINT8(buf_p, (UINT8)node);
			WRITEUINT8(buf_p, newplayernum);

			if (playerpernode[node] < 1)
			{
				nodetoplayer[node] = newplayernum;
				WRITESTRINGN(buf_p, name, MAXPLAYERNAME);
				WRITESTRINGN(buf_p, key, 32);
			}
			else if (playerpernode[node] < 2)
			{
				nodetoplayer2[node] = newplayernum;
				WRITESTRINGN(buf_p, name2, MAXPLAYERNAME);
				WRITESTRINGN(buf_p, key2, 32);
			}
			else if (playerpernode[node] < 3)
			{
				nodetoplayer3[node] = newplayernum;
				WRITESTRINGN(buf_p, name3, MAXPLAYERNAME);
				WRITESTRINGN(buf_p, key3, 32);
			}
			else if (playerpernode[node] < 4)
			{
				nodetoplayer4[node] = newplayernum;
				WRITESTRINGN(buf_p, name4, MAXPLAYERNAME);
				WRITESTRINGN(buf_p, key4, 32);
			}

			WRITEUINT8(buf_p, nodetoplayer[node]); // consoleplayer
			WRITEUINT8(buf_p, playerpernode[node]); // splitscreen num

			for (i = 0; i < MAXAVAILABILITY; i++)
			{
				WRITEUINT8(buf_p, availabilities[i]);
			}

			playerpernode[node]++;

			SendNetXCmd(XD_ADDPLAYER, buf, buf_p - buf);
			DEBFILE(va("Server added player %d node %d\n", newplayernum, node));
		}
	}

	return newplayer;
}

void CL_AddSplitscreenPlayer(void)
{
	if (cl_mode == CL_CONNECTED)
		CL_SendJoin();
}

void CL_RemoveSplitscreenPlayer(UINT8 p)
{
	if (cl_mode != CL_CONNECTED)
		return;

	SendKick(p, KICK_MSG_PLAYER_QUIT);
}

static void GotOurIP(UINT32 address)
{
	const unsigned char * p = (const unsigned char *)&address;
	#ifdef DEVELOP
		CONS_Printf("Got IP of %u.%u.%u.%u\n", p[0], p[1], p[2], p[3]);
	#endif
	ourIP = address;
}

// is there a game running
boolean Playing(void)
{
	return (server && serverrunning) || (client && cl_mode == CL_CONNECTED);
}

boolean SV_SpawnServer(void)
{
#ifdef TESTERS
	/* Just don't let the testers play. Easy. */
	I_Error("What do you think you're doing?");
	return false;
#else
	boolean result = false;
	if (demo.playback)
		G_StopDemo(); // reset engine parameter
	if (metalplayback)
		G_StopMetalDemo();

	if (!serverrunning)
	{
		CON_LogMessage(M_GetText("Starting Server....\n"));
		serverrunning = true;
		SV_ResetServer();
		SV_GenContext();
		if (netgame && I_NetOpenSocket)
		{
			I_NetOpenSocket();
		}

		// non dedicated server just connect to itself
		if (!dedicated)
			CL_ConnectToServer();
		else doomcom->numslots = 1;
	}

	ourIP = 0;
	if (netgame && server)
		STUN_bind(GotOurIP);

	// strictly speaking, i'm not convinced the following is necessary
	// but I'm not confident enough to remove it entirely in case it breaks something
	{
		UINT8 *availabilitiesbuffer = R_GetSkinAvailabilities(false, false);
		SINT8 node = 0;
		for (; node < MAXNETNODES; node++)
			result |= SV_AddWaitingPlayers(node, availabilitiesbuffer, cv_playername[0].zstring, PR_GetLocalPlayerProfile(0)->public_key, cv_playername[1].zstring, PR_GetLocalPlayerProfile(1)->public_key,  
			cv_playername[2].zstring, PR_GetLocalPlayerProfile(2)->public_key, cv_playername[3].zstring, PR_GetLocalPlayerProfile(3)->public_key);
	}
	return result;
#endif
}

void SV_StopServer(void)
{
	tic_t i;

	if (gamestate == GS_INTERMISSION)
		Y_EndIntermission();
	if (gamestate == GS_VOTING)
		Y_EndVote();
	gamestate = wipegamestate = GS_NULL;

	for (i = 0; i < MAXSPLITSCREENPLAYERS; i++)
		localtextcmd[i][0] = 0;

	for (i = firstticstosend; i < firstticstosend + BACKUPTICS; i++)
		D_Clearticcmd(i);

	consoleplayer = 0;
	cl_mode = CL_ABORTED;
	maketic = gametic+1;
	neededtic = maketic;
	serverrunning = false;
}

// called at singleplayer start and stopdemo
void SV_StartSinglePlayerServer(INT32 dogametype, boolean donetgame)
{
	INT32 lastgametype = gametype;
	server = true;
	multiplayer = (modeattacking == ATTACKING_NONE);
	joinedIP[0] = '\0';	// Make sure to empty this so that we don't save garbage when we start our own game. (because yes we use this for netgames too....)

	netgame = false; // so setting timelimit works... (XD_NETVAR doesn't play nice with SV_StopServer)

	G_SetGametype(dogametype);
	if (gametype != lastgametype)
		D_GameTypeChanged(lastgametype);

	netgame = donetgame;

	// no more tic the game with this settings!
	SV_StopServer();
}

static void SV_SendRefuse(INT32 node, const char *reason)
{
	strcpy(netbuffer->u.serverrefuse.reason, reason);

	netbuffer->packettype = PT_SERVERREFUSE;
	HSendPacket(node, false, 0, strlen(netbuffer->u.serverrefuse.reason) + 1);
	Net_CloseConnection(node);
}

// used at txtcmds received to check packetsize bound
static size_t TotalTextCmdPerTic(tic_t tic)
{
	INT32 i;
	size_t total = 1; // num of textcmds in the tic (ntextcmd byte)

	for (i = 0; i < MAXPLAYERS; i++)
	{
		UINT8 *textcmd = D_GetExistingTextcmd(tic, i);
		if ((!i || playeringame[i]) && textcmd)
			total += 2 + textcmd[0]; // "+2" for size and playernum
	}

	return total;
}

static boolean IsSplitPlayerOnNodeGuest(int node, int split)
{
	char allZero[32];
	memset(allZero, 0, 32);

	if (split == 0)
		return (memcmp(players[nodetoplayer[node]].public_key, allZero, 32) == 0);
	else if (split == 1)
		return (memcmp(players[nodetoplayer2[node]].public_key, allZero, 32) == 0);
	else if (split == 2)
		return (memcmp(players[nodetoplayer3[node]].public_key, allZero, 32) == 0);
	else if (split == 3)
		return (memcmp(players[nodetoplayer4[node]].public_key, allZero, 32) == 0);
	else
		I_Error("IsSplitPlayerOnNodeGuest: Out of bounds");
	return false; // unreachable
}

static boolean IsPlayerGuest(int player)
{
	char allZero[32];
	memset(allZero, 0, 32);

	return (memcmp(players[player].public_key, allZero, 32) == 0);
}

/** Called when a PT_CLIENTJOIN packet is received
  *
  * \param node The packet sender
  *
  */
static void HandleConnect(SINT8 node)
{
	char names[MAXSPLITSCREENPLAYERS][MAXPLAYERNAME + 1];
	INT32 i;
	UINT8 availabilitiesbuffer[MAXAVAILABILITY];

	// Sal: Dedicated mode is INCREDIBLY hacked together.
	// If a server filled out, then it'd overwrite the host and turn everyone into weird husks.....
	// It's too much effort to legimately fix right now. Just prevent it from reaching that state.
	UINT8 maxplayers = min((dedicated ? MAXPLAYERS-1 : MAXPLAYERS), cv_maxconnections.value);
	UINT8 connectedplayers = 0;

	CONS_Printf(">>>> node %d (%s)\n", node, I_GetNodeAddress(node));

	for (i = dedicated ? 1 : 0; i < MAXPLAYERS; i++)
		if (playernode[i] != UINT8_MAX) // We use this to count players because it is affected by SV_AddWaitingPlayers when more than one client joins on the same tic, unlike playeringame and D_NumPlayers. UINT8_MAX denotes no node for that player
			connectedplayers++;

	if (bannednode && bannednode[node].banid != SIZE_MAX)
	{
		const char *reason = NULL;

		// Get the reason...
		if (!I_GetBanReason || (reason = I_GetBanReason(bannednode[node].banid)) == NULL)
			reason = "No reason given";

		if (bannednode[node].timeleft != NO_BAN_TIME)
		{
			 // these are fudged a little to allow it to sink in for impatient rejoiners
			int minutes = (bannednode[node].timeleft + 30) / 60;
			int hours = (minutes + 1) / 60;
			int days = (hours + 1) / 24;

			if (days)
			{
				SV_SendRefuse(node, va("K|%s\n(Time remaining: %d day%s)", reason, days, days > 1 ? "s" : ""));
			}
			else if (hours)
			{
				SV_SendRefuse(node, va("K|%s\n(Time remaining: %d hour%s)", reason, hours, hours > 1 ? "s" : ""));
			}
			else if (minutes)
			{
				SV_SendRefuse(node, va("K|%s\n(Time remaining: %d minute%s)", reason, minutes, minutes > 1 ? "s" : ""));
			}
			else
			{
				SV_SendRefuse(node, va("K|%s\n(Time remaining: <1 minute)", reason));
			}
		}
		else
		{
			SV_SendRefuse(node, va("B|%s", reason));
		}
	}
	else if (netbuffer->u.clientcfg._255 != 255 ||
			netbuffer->u.clientcfg.packetversion != PACKETVERSION)
	{
		SV_SendRefuse(node, "Incompatible packet formats.");
	}
	else if (strncmp(netbuffer->u.clientcfg.application, SRB2APPLICATION,
				sizeof netbuffer->u.clientcfg.application))
	{
		SV_SendRefuse(node, "Different Ring Racers modifications\nare not compatible.");
	}
	else if (netbuffer->u.clientcfg.version != VERSION
		|| netbuffer->u.clientcfg.subversion != SUBVERSION)
	{
		SV_SendRefuse(node, va(M_GetText("Different Ring Racers versions cannot\nplay a netgame!\n(server version %d.%d)"), VERSION, SUBVERSION));
	}
	else if (!cv_allownewplayer.value && node)
	{
		SV_SendRefuse(node, M_GetText("The server is not accepting\njoins for the moment."));
	}
	else if (connectedplayers >= maxplayers)
	{
		SV_SendRefuse(node, va(M_GetText("Maximum players reached: %d"), maxplayers));
	}
	else if (netgame && netbuffer->u.clientcfg.localplayers > MAXSPLITSCREENPLAYERS) // Hacked client?
	{
		SV_SendRefuse(node, M_GetText("Too many players from\nthis node."));
	}
	else if (netgame && connectedplayers + netbuffer->u.clientcfg.localplayers > maxplayers)
	{
		SV_SendRefuse(node, va(M_GetText("Number of local players\nwould exceed maximum: %d"), maxplayers));
	}
	else if (netgame && !netbuffer->u.clientcfg.localplayers) // Stealth join?
	{
		SV_SendRefuse(node, M_GetText("No players from\nthis node."));
	}
	else if (luafiletransfers)
	{
		SV_SendRefuse(node, M_GetText("The server is broadcasting a file\nrequested by a Lua script.\nPlease wait a bit and then\ntry rejoining."));
	}
	else if (netgame && joindelay > 2 * (tic_t)cv_joindelay.value * TICRATE)
	{
		SV_SendRefuse(node, va(M_GetText("Too many people are connecting.\nPlease wait %d seconds and then\ntry rejoining."),
			(joindelay - 2 * cv_joindelay.value * TICRATE) / TICRATE));
	}
	else
	{
		int sigcheck;
		boolean newnode = false;

		for (i = 0; i < netbuffer->u.clientcfg.localplayers - playerpernode[node]; i++)
		{
			strlcpy(names[i], netbuffer->u.clientcfg.names[i], MAXPLAYERNAME + 1);
			if (!EnsurePlayerNameIsGood(names[i], -1))
			{
				SV_SendRefuse(node, "Bad player name");
				return;
			}

			if (node == 0) // Hey, that's us. We're always allowed to do what we want.
			{
				memcpy(lastReceivedKey[node][i], PR_GetLocalPlayerProfile(i)->public_key, sizeof(lastReceivedKey[node][i]));
			}
			else // Remote player, gotta check their signature.
			{
				CONS_Printf("Adding remote. Doing sigcheck for node %d, ID %s\n", node, GetPrettyRRID(lastReceivedKey[node][i], true));
				
				if (IsSplitPlayerOnNodeGuest(node, i)) // We're a GUEST and the server throws out our keys anyway.
				{
					if (!cv_allowguests.value)
					{
						SV_SendRefuse(node, M_GetText("The server doesn't allow GUESTs.\nCreate a profile to join!"));
						return;	
					}

					sigcheck = 0; // Always succeeds. Yes, this is a success response. C R Y P T O
				}
				else
				{	
					sigcheck = crypto_eddsa_check(netbuffer->u.clientcfg.challengeResponse[i], lastReceivedKey[node][i], lastSentChallenge[node], 32);
				}

				if (netgame && sigcheck != 0)
				{
					SV_SendRefuse(node, M_GetText("Signature verification failed."));
					return;
				}
			}
		}

		memcpy(availabilitiesbuffer, netbuffer->u.clientcfg.availabilities, sizeof(availabilitiesbuffer));

		// client authorised to join
		nodewaiting[node] = (UINT8)(netbuffer->u.clientcfg.localplayers - playerpernode[node]);
		if (!nodeingame[node])
		{
			gamestate_t backupstate = gamestate;
			newnode = true;

			SV_AddNode(node);

#ifdef VANILLAJOINNEXTROUND
			if (cv_joinnextround.value && gameaction == ga_nothing)
				G_SetGamestate(GS_WAITINGPLAYERS);
#endif
			if (!SV_SendServerConfig(node))
			{
				G_SetGamestate(backupstate);
				/// \note Shouldn't SV_SendRefuse be called before ResetNode?
				SV_SendRefuse(node, M_GetText("Server couldn't send info, please try again"));
				ResetNode(node); // Yeah, lets try it!
				/// \todo fix this !!!
				return; // restart the while
			}
			//if (gamestate != GS_LEVEL) // GS_INTERMISSION, etc?
			//	SV_SendPlayerConfigs(node); // send bare minimum player info
			G_SetGamestate(backupstate);
			DEBFILE("new node joined\n");
		}
		if (nodewaiting[node])
		{
			if (node && newnode)
			{
				SV_SendSaveGame(node, false); // send a complete game state
				DEBFILE("send savegame\n");
			}

			SV_AddWaitingPlayers(node, availabilitiesbuffer, names[0], lastReceivedKey[node][0], names[1], lastReceivedKey[node][1],
				names[2], lastReceivedKey[node][2], names[3], lastReceivedKey[node][3]);
			joindelay += cv_joindelay.value * TICRATE;
			player_joining = true;
		}
	}
}

/** Called when a PT_SERVERSHUTDOWN packet is received
  *
  * \param node The packet sender (should be the server)
  *
  */
static void HandleShutdown(SINT8 node)
{
	(void)node;
	LUA_HookBool(false, HOOK(GameQuit));
	D_QuitNetGame();
	CL_Reset();
	D_ClearState();
	M_StartControlPanel();
	M_StartMessage(M_GetText("Server has shutdown\n\nPress (B)\n"), NULL, MM_NOTHING);
}

/** Called when a PT_NODETIMEOUT packet is received
  *
  * \param node The packet sender (should be the server)
  *
  */
static void HandleTimeout(SINT8 node)
{
	(void)node;
	LUA_HookBool(false, HOOK(GameQuit));
	D_QuitNetGame();
	CL_Reset();
	D_ClearState();
	M_StartControlPanel();
	M_StartMessage(M_GetText("Server Timeout\n\nPress (B)\n"), NULL, MM_NOTHING);
}

// Called when a signature check fails and we suspect the server is playing games.
void HandleSigfail(const char *string)
{
	if (server) // This situation is basically guaranteed to be nonsense.
	{
		CONS_Alert(CONS_ERROR, "Auth error! %s\n", string);
		return; // Keep the game running, you're probably testing.
	}

	LUA_HookBool(false, HOOK(GameQuit));
	D_QuitNetGame();
	CL_Reset();
	D_ClearState();
	M_StartControlPanel();
	M_StartMessage(va(M_GetText("Signature check failed.\n(%s)\nPress (B)\n"), string), NULL, MM_NOTHING);
}

/** Called when a PT_SERVERINFO packet is received
  *
  * \param node The packet sender
  * \note What happens if the packet comes from a client or something like that?
  *
  */
static void HandleServerInfo(SINT8 node)
{
	char servername[MAXSERVERNAME];
	// compute ping in ms
	const tic_t ticnow = I_GetTime();
	const tic_t ticthen = (tic_t)LONG(netbuffer->u.serverinfo.time);
	const tic_t ticdiff = (ticnow - ticthen)*1000/NEWTICRATE;
	netbuffer->u.serverinfo.time = (tic_t)LONG(ticdiff);
	netbuffer->u.serverinfo.servername[MAXSERVERNAME-1] = 0;
	netbuffer->u.serverinfo.application
		[sizeof netbuffer->u.serverinfo.application - 1] = '\0';
	netbuffer->u.serverinfo.gametypename
		[sizeof netbuffer->u.serverinfo.gametypename - 1] = '\0';
	memcpy(servername, netbuffer->u.serverinfo.servername, MAXSERVERNAME);
	CopyCaretColors(netbuffer->u.serverinfo.servername, servername, MAXSERVERNAME);

	SL_InsertServer(&netbuffer->u.serverinfo, node);
}

static void PT_WillResendGamestate(void)
{
	char tmpsave[256];

	if (server || cl_redownloadinggamestate)
		return;

	// Don't let the server pull a fast one with everyone's identity!
	// Save the public keys we see, so if the server tries to swap one, we'll know.
	int i;
	for (i = 0; i < MAXPLAYERS; i++)
	{
		memcpy(priorKeys[i], players[i].public_key, sizeof(priorKeys[i]));
	}

	// Send back a PT_CANRECEIVEGAMESTATE packet to the server
	// so they know they can start sending the game state
	netbuffer->packettype = PT_CANRECEIVEGAMESTATE;
	if (!HSendPacket(servernode, true, 0, 0))
		return;

	CONS_Printf(M_GetText("Reloading game state...\n"));

	sprintf(tmpsave, "%s" PATHSEP TMPSAVENAME, srb2home);

	// Don't get a corrupt savegame error because tmpsave already exists
	if (FIL_FileExists(tmpsave) && unlink(tmpsave) == -1)
		I_Error("Can't delete %s\n", tmpsave);

	CL_PrepareDownloadSaveGame(tmpsave);

	cl_redownloadinggamestate = true;
}

static void PT_CanReceiveGamestate(SINT8 node)
{
	if (client || sendingsavegame[node])
		return;

	CONS_Printf(M_GetText("Resending game state to %s...\n"), player_names[nodetoplayer[node]]);

	SV_SendSaveGame(node, true); // Resend a complete game state
	resendingsavegame[node] = true;
}

/** Handles a packet received from a node that isn't in game
  *
  * \param node The packet sender
  * \todo Choose a better name, as the packet can also come from the server apparently?
  * \sa HandlePacketFromPlayer
  * \sa GetPackets
  *
  */
static void HandlePacketFromAwayNode(SINT8 node)
{
	if (node != servernode)
		DEBFILE(va("Received packet from unknown host %d\n", node));

// macro for packets that should only be sent by the server
// if it is NOT from the server, bail out and close the connection!
#define SERVERONLY \
			if (node != servernode) \
			{ \
				Net_CloseConnection(node); \
				break; \
			}
	switch (netbuffer->packettype)
	{
		case PT_ASKINFOVIAMS:
#if 0
			if (server && serverrunning)
			{
				INT32 clientnode;
				if (ms_RoomId < 0) // ignore if we're not actually on the MS right now
				{
					Net_CloseConnection(node); // and yes, close connection
					return;
				}
				clientnode = I_NetMakeNode(netbuffer->u.msaskinfo.clientaddr);
				if (clientnode != -1)
				{
					SV_SendServerInfo(clientnode, (tic_t)LONG(netbuffer->u.msaskinfo.time));
					SV_SendPlayerInfo(clientnode); // Send extra info
					Net_CloseConnection(clientnode);
					// Don't close connection to MS...
				}
				else
					Net_CloseConnection(node); // ...unless the IP address is not valid
			}
			else
				Net_CloseConnection(node); // you're not supposed to get it, so ignore it
#else
			Net_CloseConnection(node);
#endif
			break;

		case PT_TELLFILESNEEDED:
			if (server && serverrunning)
			{
				UINT8 *p;
				INT32 firstfile = netbuffer->u.filesneedednum;

				netbuffer->packettype = PT_MOREFILESNEEDED;
				netbuffer->u.filesneededcfg.first = firstfile;
				netbuffer->u.filesneededcfg.more = 0;

				p = PutFileNeeded(firstfile);

				HSendPacket(node, false, 0, p - ((UINT8 *)&netbuffer->u));
			}
			else // Shouldn't get this if you aren't the server...?
				Net_CloseConnection(node);
			break;

		case PT_MOREFILESNEEDED:
			if (server && serverrunning)
			{ // But wait I thought I'm the server?
				Net_CloseConnection(node);
				break;
			}
			SERVERONLY
			if (cl_mode == CL_ASKFULLFILELIST && netbuffer->u.filesneededcfg.first == fileneedednum)
			{
				D_ParseFileneeded(netbuffer->u.filesneededcfg.num, netbuffer->u.filesneededcfg.files, netbuffer->u.filesneededcfg.first);
				if (!netbuffer->u.filesneededcfg.more)
					cl_lastcheckedfilecount = UINT16_MAX; // Got the whole file list
			}
			break;

		case PT_ASKINFO:
			if (server && serverrunning)
			{
				SV_SendServerInfo(node, (tic_t)LONG(netbuffer->u.askinfo.time));
				SV_SendPlayerInfo(node); // Send extra info
			}
			Net_CloseConnection(node);
			break;

		case PT_SERVERREFUSE: // Negative response of client join request
			if (server && serverrunning)
			{ // But wait I thought I'm the server?
				Net_CloseConnection(node);
				break;
			}
			SERVERONLY
			if (cl_mode == CL_WAITJOINRESPONSE)
			{
				// Save the reason so it can be displayed after quitting the netgame
				char *reason = strdup(netbuffer->u.serverrefuse.reason);
				if (!reason)
					I_Error("Out of memory!\n");

				if (strstr(reason, "Maximum players reached"))
				{
					serverisfull = true;
					//Special timeout for when refusing due to player cap. The client will wait 3 seconds between join requests when waiting for a slot, so we need this to be much longer
					//We set it back to the value of cv_nettimeout.value in CL_Reset
					connectiontimeout = NEWTICRATE*7;
					cl_mode = CL_ASKJOIN;
					free(reason);
					break;
				}

				D_QuitNetGame();
				CL_Reset();
				D_ClearState();
				M_StartControlPanel();

				if (reason[1] == '|')
				{
					M_StartMessage(va("You have been %sfrom the server\n\nReason:\n%s",
						(reason[0] == 'B') ? "banned\n" : "temporarily\nkicked ",
						reason+2), NULL, MM_NOTHING);
				}
				else
				{
					M_StartMessage(va(M_GetText("Server refuses connection\n\nReason:\n%s"),
						reason), NULL, MM_NOTHING);
				}

				free(reason);

				// Will be reset by caller. Signals refusal.
				cl_mode = CL_ABORTED;
			}
			break;

		case PT_SERVERCFG: // Positive response of client join request
		{
			if (server && serverrunning && node != servernode)
			{ // but wait I thought I'm the server?
				Net_CloseConnection(node);
				break;
			}
			SERVERONLY
			/// \note how would this happen? and is it doing the right thing if it does?
			if (!(cl_mode == CL_WAITJOINRESPONSE || cl_mode == CL_ASKJOIN))
				break;

			if (client)
			{
				maketic = gametic = neededtic = (tic_t)LONG(netbuffer->u.servercfg.gametic);

				G_SetGametype(netbuffer->u.servercfg.gametype);

				modifiedgame = netbuffer->u.servercfg.modifiedgame;
				memcpy(server_context, netbuffer->u.servercfg.server_context, 8);
			}

#ifdef HAVE_DISCORDRPC
			discordInfo.maxPlayers = netbuffer->u.servercfg.maxplayer;
			discordInfo.joinsAllowed = netbuffer->u.servercfg.allownewplayer;
			discordInfo.everyoneCanInvite = netbuffer->u.servercfg.discordinvites;
#endif

			nodeingame[(UINT8)servernode] = true;
			serverplayer = netbuffer->u.servercfg.serverplayer;
			doomcom->numslots = SHORT(netbuffer->u.servercfg.totalslotnum);
			mynode = netbuffer->u.servercfg.clientnode;
			if (serverplayer >= 0)
				playernode[(UINT8)serverplayer] = servernode;

			if (netgame)
				CONS_Printf(M_GetText("Join accepted, waiting for complete game state...\n"));
			DEBFILE(va("Server accept join gametic=%u mynode=%d\n", gametic, mynode));

			/// \note Wait. What if a Lua script uses some global custom variables synched with the NetVars hook?
			///       Shouldn't them be downloaded even at intermission time?
			///       Also, according to HandleConnect, the server will send the savegame even during intermission...
			/// Sryder 2018-07-05: If we don't want to send the player config another way we need to send the gamestate
			///                    At almost any gamestate there could be joiners... So just always send gamestate?
			cl_mode = ((server) ? CL_CONNECTED : CL_DOWNLOADSAVEGAME);
			break;
		}

		// Handled in d_netfil.c
		case PT_FILEFRAGMENT:
			if (server)
			{ // But wait I thought I'm the server?
				Net_CloseConnection(node);
				break;
			}
			SERVERONLY
			PT_FileFragment();
			break;

		case PT_FILEACK:
			if (server)
				PT_FileAck();
			break;

		case PT_FILERECEIVED:
			if (server)
				PT_FileReceived();
			break;

		case PT_REQUESTFILE:
			if (server)
			{
				if (!cv_downloading.value || !PT_RequestFile(node))
					Net_CloseConnection(node); // close connection if one of the requested files could not be sent, or you disabled downloading anyway
			}
			else
				Net_CloseConnection(node); // nope
			break;

		case PT_NODETIMEOUT:
		case PT_CLIENTQUIT:
			if (server)
			{
				Net_CloseConnection(node);
				nodeneedsauth[node] = false;
			}
			break;

		case PT_CLIENTCMD:
			break; // This is not an "unknown packet"

		case PT_SERVERTICS:
			// Do not remove my own server (we have just get a out of order packet)
			if (node == servernode)
				break;
			/* FALLTHRU */
		case PT_CLIENTKEY:
			if (server)
			{
				PT_ClientKey(node);

				// Client's not in the server yet, but we still need to lock up the node.
				// Otherwise, someone else could request a challenge on the same node and trash it.
				nodeneedsauth[node] = true;
				freezetimeout[node] = I_GetTime() + jointimeout;

				CONS_Printf("4: node %d -> %d\n", node, nodeneedsauth[node]);
				if (nodeneedsauth[node] == false)
				{
					freezetimeout[node] = I_GetTime() + jointimeout;
					nodeneedsauth[node] = true;
				}
			}
			break;
		case PT_SERVERCHALLENGE:
			if (server && serverrunning && node != servernode)
			{
				Net_CloseConnection(node);
				break;
			}
			if (cl_mode != CL_WAITCHALLENGE)
				break;
			memcpy(awaitingChallenge, netbuffer->u.serverchallenge.secret, sizeof(awaitingChallenge));
			cl_mode = CL_ASKJOIN;
			break;
		default:
			DEBFILE(va("unknown packet received (%d) from unknown host\n",netbuffer->packettype));
			Net_CloseConnection(node);
			break; // Ignore it

	}
#undef SERVERONLY
}

/** Checks ticcmd for "speed hacks"
  *
  * \param p Which player
  * \return True if player is hacking
  * \sa HandlePacketFromPlayer
  *
  */
static boolean CheckForSpeedHacks(UINT8 p)
{
	if (netcmds[maketic%BACKUPTICS][p].forwardmove > MAXPLMOVE || netcmds[maketic%BACKUPTICS][p].forwardmove < -MAXPLMOVE
		|| netcmds[maketic%BACKUPTICS][p].turning > KART_FULLTURN || netcmds[maketic%BACKUPTICS][p].turning < -KART_FULLTURN
		|| netcmds[maketic%BACKUPTICS][p].throwdir > KART_FULLTURN || netcmds[maketic%BACKUPTICS][p].throwdir < -KART_FULLTURN)
	{
		CONS_Alert(CONS_WARNING, M_GetText("Illegal movement value received from node %d\n"), playernode[p]);
		//D_Clearticcmd(k);

		SendKick(p, KICK_MSG_CON_FAIL);
		return true;
	}

	return false;
}

static char NodeToSplitPlayer(int node, int split)
{
	if (split == 0)
		return nodetoplayer[node];
	else if (split == 1)
		return nodetoplayer2[node];
	else if (split == 2)
		return nodetoplayer3[node];
	else if (split == 3)
		return nodetoplayer4[node];
	return -1;
} 

/** Handles a packet received from a node that is in game
  *
  * \param node The packet sender
  * \todo Choose a better name
  * \sa HandlePacketFromAwayNode
  * \sa GetPackets
  *
  */
static void HandlePacketFromPlayer(SINT8 node)
{
	INT32 netconsole;
	tic_t realend, realstart;
	UINT8 *pak, *txtpak, numtxtpak;
#ifndef NOMD5
	UINT8 finalmd5[16];/* Well, it's the cool thing to do? */
#endif

	txtpak = NULL;

	if (dedicated && node == 0)
		netconsole = 0;
	else
		netconsole = nodetoplayer[node];
#ifdef PARANOIA
	if (netconsole >= MAXPLAYERS)
		I_Error("bad table nodetoplayer: node %d player %d", doomcom->remotenode, netconsole);
#endif
	

#ifdef SIGNGAMETRAFFIC
	if (server)
	{

		int splitnodes;
		if (IsPacketSigned(netbuffer->packettype))
		{
			for (splitnodes = 0; splitnodes < MAXSPLITSCREENPLAYERS; splitnodes++)
			{
				int targetplayer = NodeToSplitPlayer(node, splitnodes);
				if (targetplayer == -1)
					continue;

				const void* message = &netbuffer->u;
				if (IsSplitPlayerOnNodeGuest(node, splitnodes) || demo.playback)
				{
					//CONS_Printf("Throwing out a guest signature from node %d player %d\n", node, splitnodes);
				}
				else
				{
					if (crypto_eddsa_check(netbuffer->signature[splitnodes], players[targetplayer].public_key, message, doomcom->datalength - BASEPACKETSIZE))
					{
						CONS_Alert(CONS_ERROR, "SIGFAIL! Packet type %d from node %d player %d\nkey %s size %d netconsole %d\n", 
							netbuffer->packettype, node, splitnodes,
							GetPrettyRRID(players[targetplayer].public_key, true), doomcom->datalength - BASEPACKETSIZE, netconsole);
						
						if (netconsole != -1) // NO IDEA.
							SendKick(netconsole, KICK_MSG_SIGFAIL);
						// Net_CloseConnection(node);
						// nodeingame[node] = false;
						return;
					}
				}
					
			}
		}
	}
#endif

	switch (netbuffer->packettype)
	{
// -------------------------------------------- SERVER RECEIVE ----------
		case PT_CLIENTCMD:
		case PT_CLIENT2CMD:
		case PT_CLIENT3CMD:
		case PT_CLIENT4CMD:
		case PT_CLIENTMIS:
		case PT_CLIENT2MIS:
		case PT_CLIENT3MIS:
		case PT_CLIENT4MIS:
		case PT_NODEKEEPALIVE:
		case PT_NODEKEEPALIVEMIS:
			if (client)
				break;

			// To save bytes, only the low byte of tic numbers are sent
			// Use ExpandTics to figure out what the rest of the bytes are

			realstart = ExpandTics(netbuffer->u.clientpak.client_tic, nettics[node]);
			realend = ExpandTics(netbuffer->u.clientpak.resendfrom, nettics[node]);

			if (netbuffer->packettype == PT_CLIENTMIS || netbuffer->packettype == PT_CLIENT2MIS
				|| netbuffer->packettype == PT_CLIENT3MIS || netbuffer->packettype == PT_CLIENT4MIS
				|| netbuffer->packettype == PT_NODEKEEPALIVEMIS
				|| supposedtics[node] < realend)
			{
				supposedtics[node] = realend;
			}
			// Discard out of order packet
			if (nettics[node] > realend)
			{
				DEBFILE(va("out of order ticcmd discarded nettics = %u\n", nettics[node]));
				break;
			}

			// Update the nettics
			nettics[node] = realend;

			// This should probably still timeout though, as the node should always have a player 1 number
			if (netconsole == -1)
				break;

			// As long as clients send valid ticcmds, the server can keep running, so reset the timeout
			/// \todo Use a separate cvar for that kind of timeout?
			freezetimeout[node] = I_GetTime() + connectiontimeout;

			// Don't do anything for packets of type NODEKEEPALIVE?
			// Sryder 2018/07/01: Update the freezetimeout still!
			if (netbuffer->packettype == PT_NODEKEEPALIVE
				|| netbuffer->packettype == PT_NODEKEEPALIVEMIS)
				break;

			// Copy ticcmd
			G_MoveTiccmd(&netcmds[maketic%BACKUPTICS][netconsole], &netbuffer->u.clientpak.cmd, 1);

			// Check ticcmd for "speed hacks"
			if (CheckForSpeedHacks((UINT8)netconsole))
				break;

			// Splitscreen cmd
			if (((netbuffer->packettype == PT_CLIENT2CMD || netbuffer->packettype == PT_CLIENT2MIS)
				|| (netbuffer->packettype == PT_CLIENT3CMD || netbuffer->packettype == PT_CLIENT3MIS)
				|| (netbuffer->packettype == PT_CLIENT4CMD || netbuffer->packettype == PT_CLIENT4MIS))
				&& (nodetoplayer2[node] >= 0))
			{
				G_MoveTiccmd(&netcmds[maketic%BACKUPTICS][(UINT8)nodetoplayer2[node]],
					&netbuffer->u.client2pak.cmd2, 1);

				if (CheckForSpeedHacks((UINT8)nodetoplayer2[node]))
					break;
			}

			if (((netbuffer->packettype == PT_CLIENT3CMD || netbuffer->packettype == PT_CLIENT3MIS)
				|| (netbuffer->packettype == PT_CLIENT4CMD || netbuffer->packettype == PT_CLIENT4MIS))
				&& (nodetoplayer3[node] >= 0))
			{
				G_MoveTiccmd(&netcmds[maketic%BACKUPTICS][(UINT8)nodetoplayer3[node]],
					&netbuffer->u.client3pak.cmd3, 1);

				if (CheckForSpeedHacks((UINT8)nodetoplayer3[node]))
					break;
			}

			if ((netbuffer->packettype == PT_CLIENT4CMD || netbuffer->packettype == PT_CLIENT4MIS)
				&& (nodetoplayer4[node] >= 0))
			{
				G_MoveTiccmd(&netcmds[maketic%BACKUPTICS][(UINT8)nodetoplayer4[node]],
					&netbuffer->u.client4pak.cmd4, 1);

				if (CheckForSpeedHacks((UINT8)nodetoplayer4[node]))
					break;
			}

			// Check player consistancy during the level
			if (realstart <= gametic && realstart + BACKUPTICS - 1 > gametic && gamestate == GS_LEVEL
				&& consistancy[realstart%BACKUPTICS] != SHORT(netbuffer->u.clientpak.consistancy)
				&& !resendingsavegame[node] && savegameresendcooldown[node] <= I_GetTime()
				&& !SV_ResendingSavegameToAnyone())
			{
				if (cv_resynchattempts.value)
				{
					// Tell the client we are about to resend them the gamestate
					netbuffer->packettype = PT_WILLRESENDGAMESTATE;
					HSendPacket(node, true, 0, 0);

					resendingsavegame[node] = true;

					if (cv_blamecfail.value)
						CONS_Printf(M_GetText("Synch failure for player %d (%s); expected %hd, got %hd\n"),
							netconsole+1, player_names[netconsole],
							consistancy[realstart%BACKUPTICS],
							SHORT(netbuffer->u.clientpak.consistancy));
					DEBFILE(va("Restoring player %d (synch failure) [%update] %d!=%d\n",
						netconsole, realstart, consistancy[realstart%BACKUPTICS],
						SHORT(netbuffer->u.clientpak.consistancy)));
					break;
				}
				else
				{
					SendKick(netconsole, KICK_MSG_CON_FAIL);
					DEBFILE(va("player %d kicked (synch failure) [%u] %d!=%d\n",
						netconsole, realstart, consistancy[realstart%BACKUPTICS],
						SHORT(netbuffer->u.clientpak.consistancy)));
					break;
				}
			}
			break;
		case PT_BASICKEEPALIVE:
			if (client)
				break;

			// This should probably still timeout though, as the node should always have a player 1 number
			if (netconsole == -1)
				break;

			// If a client sends this it should mean they are done receiving the savegame
			sendingsavegame[node] = false;

			// As long as clients send keep alives, the server can keep running, so reset the timeout
			/// \todo Use a separate cvar for that kind of timeout?
			freezetimeout[node] = I_GetTime() + connectiontimeout;
			break;
		case PT_TEXTCMD:
		case PT_TEXTCMD2:
		case PT_TEXTCMD3:
		case PT_TEXTCMD4:
			if (netbuffer->packettype == PT_TEXTCMD2) // splitscreen special
				netconsole = nodetoplayer2[node];
			else if (netbuffer->packettype == PT_TEXTCMD3)
				netconsole = nodetoplayer3[node];
			else if (netbuffer->packettype == PT_TEXTCMD4)
				netconsole = nodetoplayer4[node];

			if (client)
				break;

			if (netconsole < 0 || netconsole >= MAXPLAYERS)
				Net_UnAcknowledgePacket(node);
			else
			{
				size_t j;
				tic_t tic = maketic;
				UINT8 *textcmd;

				// ignore if the textcmd has a reported size of zero
				// this shouldn't be sent at all
				if (!netbuffer->u.textcmd[0])
				{
					DEBFILE(va("GetPacket: Textcmd with size 0 detected! (node %u, player %d)\n",
						node, netconsole));
					Net_UnAcknowledgePacket(node);
					break;
				}

				// ignore if the textcmd size var is actually larger than it should be
				// BASEPACKETSIZE + 1 (for size) + textcmd[0] should == datalength
				if (netbuffer->u.textcmd[0] > (size_t)doomcom->datalength-BASEPACKETSIZE-1)
				{
					DEBFILE(va("GetPacket: Bad Textcmd packet size! (expected %d, actual %s, node %u, player %d)\n",
					netbuffer->u.textcmd[0], sizeu1((size_t)doomcom->datalength-BASEPACKETSIZE-1),
						node, netconsole));
					Net_UnAcknowledgePacket(node);
					break;
				}

				// check if tic that we are making isn't too large else we cannot send it :(
				// doomcom->numslots+1 "+1" since doomcom->numslots can change within this time and sent time
				j = software_MAXPACKETLENGTH
					- (netbuffer->u.textcmd[0]+2+BASESERVERTICSSIZE
					+ (doomcom->numslots+1)*sizeof(ticcmd_t));

				// search a tic that have enougth space in the ticcmd
				while ((textcmd = D_GetExistingTextcmd(tic, netconsole)),
					(TotalTextCmdPerTic(tic) > j || netbuffer->u.textcmd[0] + (textcmd ? textcmd[0] : 0) > MAXTEXTCMD)
					&& tic < firstticstosend + BACKUPTICS)
					tic++;

				if (tic >= firstticstosend + BACKUPTICS)
				{
					DEBFILE(va("GetPacket: Textcmd too long (max %s, used %s, mak %d, "
						"tosend %u, node %u, player %d)\n", sizeu1(j), sizeu2(TotalTextCmdPerTic(maketic)),
						maketic, firstticstosend, node, netconsole));
					Net_UnAcknowledgePacket(node);
					break;
				}

				// Make sure we have a buffer
				if (!textcmd) textcmd = D_GetTextcmd(tic, netconsole);

				DEBFILE(va("textcmd put in tic %u at position %d (player %d) ftts %u mk %u\n",
					tic, textcmd[0]+1, netconsole, firstticstosend, maketic));

				M_Memcpy(&textcmd[textcmd[0]+1], netbuffer->u.textcmd+1, netbuffer->u.textcmd[0]);
				textcmd[0] += (UINT8)netbuffer->u.textcmd[0];
			}
			break;
		case PT_LOGIN:
			if (client)
				break;

#ifndef NOMD5
			if (doomcom->datalength < 16)/* ignore partial sends */
				break;

			if (!adminpasswordset)
			{
				CONS_Printf(M_GetText("Password from %s failed (no password set).\n"), player_names[netconsole]);
				break;
			}

			// Do the final pass to compare with the sent md5
			D_MD5PasswordPass(adminpassmd5, 16, va("PNUM%02d", netconsole), &finalmd5);

			if (!memcmp(netbuffer->u.md5sum, finalmd5, 16))
			{
				CONS_Printf(M_GetText("%s passed authentication.\n"), player_names[netconsole]);
				COM_BufInsertText(va("promote %d\n", netconsole)); // do this immediately
			}
			else
				CONS_Printf(M_GetText("Password from %s failed.\n"), player_names[netconsole]);
#endif
			break;
		case PT_NODETIMEOUT:
		case PT_CLIENTQUIT:
			if (client)
				break;

			// nodeingame will be put false in the execution of kick command
			// this allow to send some packets to the quitting client to have their ack back
			nodewaiting[node] = 0;
			if (netconsole != -1 && playeringame[netconsole])
			{
				UINT8 kickmsg;

				if (netbuffer->packettype == PT_NODETIMEOUT)
					kickmsg = KICK_MSG_TIMEOUT;
				else
					kickmsg = KICK_MSG_PLAYER_QUIT;

				SendKick(netconsole, kickmsg);

				/*
				nodetoplayer[node] = -1;

				if (nodetoplayer2[node] != -1 && nodetoplayer2[node] >= 0
					&& playeringame[(UINT8)nodetoplayer2[node]])
				{
					SendKick(nodetoplayer2[node], kickmsg);
					nodetoplayer2[node] = -1;
				}

				if (nodetoplayer3[node] != -1 && nodetoplayer3[node] >= 0
					&& playeringame[(UINT8)nodetoplayer3[node]])
				{
					SendKick(nodetoplayer3[node], kickmsg);
					nodetoplayer3[node] = -1;
				}

				if (nodetoplayer4[node] != -1 && nodetoplayer4[node] >= 0
					&& playeringame[(UINT8)nodetoplayer4[node]])
				{
					SendKick(nodetoplayer4[node], kickmsg);
					nodetoplayer4[node] = -1;
				}
				*/
			}
			Net_CloseConnection(node);
			nodeingame[node] = false;
			nodeneedsauth[node] = false;
			CONS_Printf("1: node %d -> %d\n", node, nodeneedsauth[node]);
			break;
		case PT_CANRECEIVEGAMESTATE:
			PT_CanReceiveGamestate(node);
			break;
		case PT_ASKLUAFILE:
			if (server && luafiletransfers && luafiletransfers->nodestatus[node] == LFTNS_ASKED)
				AddLuaFileToSendQueue(node, luafiletransfers->realfilename);
			break;
		case PT_HASLUAFILE:
			if (server && luafiletransfers && luafiletransfers->nodestatus[node] == LFTNS_SENDING)
				SV_HandleLuaFileSent(node);
			break;
		case PT_RECEIVEDGAMESTATE:
			sendingsavegame[node] = false;
			resendingsavegame[node] = false;
			savegameresendcooldown[node] = I_GetTime() + 5 * TICRATE;
			break;
// -------------------------------------------- CLIENT RECEIVE ----------
		case PT_SERVERTICS:
			// Only accept PT_SERVERTICS from the server.
			if (node != servernode)
			{
				CONS_Alert(CONS_WARNING, M_GetText("%s received from non-host %d\n"), "PT_SERVERTICS", node);
				if (server)
					SendKick(netconsole, KICK_MSG_CON_FAIL);
				break;
			}

			realstart = ExpandTics(netbuffer->u.serverpak.starttic, maketic);
			realend = realstart + netbuffer->u.serverpak.numtics;

			if (!txtpak)
				txtpak = (UINT8 *)&netbuffer->u.serverpak.cmds[netbuffer->u.serverpak.numslots
					* netbuffer->u.serverpak.numtics];

			if (realend > gametic + CLIENTBACKUPTICS)
				realend = gametic + CLIENTBACKUPTICS;
			cl_packetmissed = realstart > neededtic;

			if (realstart <= neededtic && realend > neededtic)
			{
				tic_t i, j;
				pak = (UINT8 *)&netbuffer->u.serverpak.cmds;

				for (i = realstart; i < realend; i++)
				{
					// clear first
					D_Clearticcmd(i);

					// copy the tics
					pak = G_ScpyTiccmd(netcmds[i%BACKUPTICS], pak,
						netbuffer->u.serverpak.numslots*sizeof (ticcmd_t));

					// copy the textcmds
					numtxtpak = *txtpak++;
					for (j = 0; j < numtxtpak; j++)
					{
						INT32 k = *txtpak++; // playernum
						const size_t txtsize = txtpak[0]+1;

						if (i >= gametic) // Don't copy old net commands
							M_Memcpy(D_GetTextcmd(i, k), txtpak, txtsize);
						txtpak += txtsize;
					}
				}

				neededtic = realend;
			}
			else
			{
				DEBFILE(va("frame not in bound: %u\n", neededtic));
				/*if (realend < neededtic - 2 * TICRATE || neededtic + 2 * TICRATE < realstart)
					I_Error("Received an out of order PT_SERVERTICS packet!\n"
							"Got tics %d-%d, needed tic %d\n\n"
							"Please report this crash on the Master Board,\n"
							"IRC or Discord so it can be fixed.\n", (INT32)realstart, (INT32)realend, (INT32)neededtic);*/
			}
			break;
		case PT_PING:
			// Only accept PT_PING from the server.
			if (node != servernode)
			{
				CONS_Alert(CONS_WARNING, M_GetText("%s received from non-host %d\n"), "PT_PING", node);
				if (server)
					SendKick(netconsole, KICK_MSG_CON_FAIL);
				break;
			}

			//Update client ping table from the server.
			if (client)
			{
				UINT8 i;
				for (i = 0; i < MAXPLAYERS; i++)
					if (playeringame[i])
						playerpingtable[i] = (tic_t)netbuffer->u.pingtable[i];

				servermaxping = (tic_t)netbuffer->u.pingtable[MAXPLAYERS];
			}

			break;
		case PT_SERVERCFG:
			break;
		case PT_FILEFRAGMENT:
			// Only accept PT_FILEFRAGMENT from the server.
			if (node != servernode)
			{
				CONS_Alert(CONS_WARNING, M_GetText("%s received from non-host %d\n"), "PT_FILEFRAGMENT", node);
				if (server)
					SendKick(netconsole, KICK_MSG_CON_FAIL);
				break;
			}
			if (client)
				PT_FileFragment();
			break;
		case PT_FILEACK:
			if (server)
				PT_FileAck();
			break;
		case PT_FILERECEIVED:
			if (server)
				PT_FileReceived();
			break;
		case PT_WILLRESENDGAMESTATE:
			PT_WillResendGamestate();
			break;
		case PT_SENDINGLUAFILE:
			if (client)
				CL_PrepareDownloadLuaFile();
			break;
		case PT_CHALLENGEALL: ; // -Wpedantic
			if (demo.playback || node != servernode) // SERVER should still respond to this to prove its own identity, just not from clients.
				break;

			int challengeplayers;

			memcpy(lastChallengeAll, netbuffer->u.challengeall.secret, sizeof(lastChallengeAll));

			shouldsign_t safe = ShouldSignChallenge(lastChallengeAll);
			if (safe != SIGN_OK)
			{
				if (safe == SIGN_BADIP)
					HandleSigfail("External server sent the wrong IP");
				else if (safe == SIGN_BADTIME)
					HandleSigfail("Bad timestamp - check your clocks");
				else
					HandleSigfail("Unknown auth error - contact a developer");
				break;
			}

			netbuffer->packettype = PT_RESPONSEALL;

			#ifdef DEVELOP
				if (cv_noresponse.value)
				{
					CV_AddValue(&cv_noresponse, -1);
					CONS_Alert(CONS_WARNING, "cv_noresponse enabled, not sending PT_RESPONSEALL\n");
					break;
				}
			#endif

			// Don't leak uninitialized memory.
			memset(&netbuffer->u.responseall, 0, sizeof(netbuffer->u.responseall));

			for (challengeplayers = 0; challengeplayers <= splitscreen; challengeplayers++)
			{
				uint8_t signature[64];
				profile_t *localProfile = PR_GetLocalPlayerProfile(challengeplayers);
				if (!PR_IsLocalPlayerGuest(challengeplayers)) // GUESTS don't have keys
				{
					CONS_Printf("signing %s pk %s\n", GetPrettyRRID(lastChallengeAll, true), GetPrettyRRID(localProfile->public_key, true));
					crypto_eddsa_sign(signature, localProfile->secret_key, lastChallengeAll, sizeof(lastChallengeAll));

					// If our keys are garbage (corrupted profile?), fail here instead of when the server boots us, so the player knows what's going on.
					if (crypto_eddsa_check(signature, localProfile->public_key, lastChallengeAll, sizeof(lastChallengeAll)) != 0)
						I_Error("Couldn't self-verify key associated with player %d, profile %d.\nProfile data may be corrupted.", challengeplayers, cv_lastprofile[challengeplayers].value);
				}

				#ifdef DEVELOP
					if (cv_badresponse.value)
					{
						CV_AddValue(&cv_badresponse, -1);
						CONS_Alert(CONS_WARNING, "cv_badresponse enabled, scrubbing signature from PT_RESPONSEALL\n");
						memset(signature, 0, 64);
					}
				#endif

				memcpy(netbuffer->u.responseall.signature[challengeplayers], signature, sizeof(signature));
			}

			HSendPacket(servernode, true, 0, sizeof(netbuffer->u.responseall));
			break;
		case PT_RESPONSEALL:
			if (demo.playback || client)
				break;

			int responseplayer;
			for (responseplayer = 0; responseplayer < MAXSPLITSCREENPLAYERS; responseplayer++)
			{
				int targetplayer = NodeToSplitPlayer(node, responseplayer);
				if (targetplayer == -1)
					continue;

				if (!IsPlayerGuest(targetplayer))
				{
					CONS_Printf("receiving %s pk %s\n", GetPrettyRRID(lastChallengeAll, true), GetPrettyRRID(players[targetplayer].public_key, true));
					if (crypto_eddsa_check(netbuffer->u.responseall.signature[responseplayer], players[targetplayer].public_key, lastChallengeAll, sizeof(lastChallengeAll)))
					{
						CONS_Alert(CONS_WARNING, "Invalid PT_RESPONSEALL from node %d player %d split %d\n", node, targetplayer, responseplayer);
						if (playernode[targetplayer] != 0) // NO IDEA.
							SendKick(targetplayer, KICK_MSG_SIGFAIL);
						break;
					}
					else 
					{
						memcpy(lastReceivedSignature[targetplayer], netbuffer->u.responseall.signature[responseplayer], sizeof(lastReceivedSignature[targetplayer]));
						CONS_Printf("Writing signature %s for node %d player %d split %d\n", GetPrettyRRID(lastReceivedSignature[targetplayer], true), node, targetplayer, responseplayer);
					}
				}
			}
			break;
		case PT_RESULTSALL: ; // -Wpedantic
			int resultsplayer;
			uint8_t allzero[64];
			memset(allzero, 0, sizeof(allzero));

			CONS_Printf("Got PT_RESULTSALL\n");

			if (demo.playback || server || node != servernode || !expectChallenge)
				break;

			CONS_Printf("Checking PT_RESULTSALL\n");

			for (resultsplayer = 0; resultsplayer < MAXPLAYERS; resultsplayer++)
			{
				if (!playeringame[resultsplayer])
				{
					CONS_Printf("Player %d isn't in the game, excluded from checkall\n", resultsplayer);
					continue;
				}
				else if (IsPlayerGuest(resultsplayer))
				{
					CONS_Printf("GUEST on node %d player %d split %d, not enforcing\n", playernode[resultsplayer], resultsplayer, players[resultsplayer].splitscreenindex);
					continue;
				}
				else if (memcmp(knownWhenChallenged[resultsplayer], allzero, sizeof(allzero)) == 0)
				{
					CONS_Printf("That motherfucker wasn't here for the challenge - node %d player %d split %d, not enforcing\n", playernode[resultsplayer], resultsplayer, players[resultsplayer].splitscreenindex);
					continue;
				}
				else if (memcmp(knownWhenChallenged[resultsplayer], players[resultsplayer].public_key, sizeof(knownWhenChallenged[resultsplayer])) != 0)
				{
					// A player left after the challenge process started, and someone else took their place.
					// That means they haven't received a challenge either.
					CONS_Printf("Has key %s but I remember key %s - node %d player %d split %d, not enforcing\n",
					GetPrettyRRID(knownWhenChallenged[resultsplayer], true), GetPrettyRRID(players[resultsplayer].public_key, true),
					playernode[resultsplayer], resultsplayer, players[resultsplayer].splitscreenindex);
					continue;
				}
				else
				{
					if (crypto_eddsa_check(netbuffer->u.resultsall.signature[resultsplayer],
						knownWhenChallenged[resultsplayer], lastChallengeAll, sizeof(lastChallengeAll)))
					{
						CONS_Alert(CONS_WARNING, "PT_RESULTSALL had invalid signature %s for node %d player %d split %d, something doesn't add up!\n",
							GetPrettyRRID(netbuffer->u.resultsall.signature[resultsplayer], true), playernode[resultsplayer], resultsplayer, players[resultsplayer].splitscreenindex);
						HandleSigfail("Server sent invalid client signature.");
						break;
					}
					else
					{
						CONS_Printf("Checkall client-pass for node %d player %d split %d\n", playernode[resultsplayer], resultsplayer, players[resultsplayer].splitscreenindex);
					}
				}
			}
			csprng(lastChallengeAll, sizeof(lastChallengeAll));
			expectChallenge = false;
			break;
		default:
			DEBFILE(va("UNKNOWN PACKET TYPE RECEIVED %d from host %d\n",
				netbuffer->packettype, node));
	} // end switch
}

/**	Handles all received packets, if any
  *
  * \todo Add details to this description (lol)
  *
  */
static void GetPackets(void)
{
	SINT8 node; // The packet sender

	player_joining = false;

	while (HGetPacket())
	{
		node = (SINT8)doomcom->remotenode;

		if (netbuffer->packettype == PT_CLIENTJOIN && server)
		{
			if (!levelloading) // Otherwise just ignore
			{
				HandleConnect(node);
			}
			continue;
		}
		if (node == servernode && client && cl_mode != CL_SEARCHING)
		{
			if (netbuffer->packettype == PT_SERVERSHUTDOWN)
			{
				HandleShutdown(node);
				continue;
			}
			if (netbuffer->packettype == PT_NODETIMEOUT)
			{
				HandleTimeout(node);
				continue;
			}
		}

		if (netbuffer->packettype == PT_SERVERINFO)
		{
			HandleServerInfo(node);
			continue;
		}

		if (netbuffer->packettype == PT_PLAYERINFO)
			continue; // We do nothing with PLAYERINFO, that's for the MS browser.

		// Packet received from someone already playing
		if (nodeingame[node])
			HandlePacketFromPlayer(node);
		// Packet received from someone not playing
		else
			HandlePacketFromAwayNode(node);
	}
}

//
// NetUpdate
// Builds ticcmds for console player,
// sends out a packet
//
// no more use random generator, because at very first tic isn't yet synchronized
// Note: It is called consistAncy on purpose.
//
static INT16 Consistancy(void)
{
	INT32 i;
	UINT32 ret = 0;
#ifdef MOBJCONSISTANCY
	thinker_t *th;
	mobj_t *mo;
#endif

	DEBFILE(va("TIC %u ", gametic));

	for (i = 0; i < MAXPLAYERS; i++)
	{
		if (!playeringame[i])
			ret ^= 0xCCCC;
		else if (!players[i].mo || gamestate != GS_LEVEL);
		else
		{
			ret += players[i].mo->x;
			ret -= players[i].mo->y;
			ret += players[i].itemtype;
			ret *= i+1;
		}
	}
	// I give up
	// Coop desynching enemies is painful
	if (gamestate == GS_LEVEL)
	{
		for (i = 0; i < PRNUMCLASS; i++)
		{
			if (i & 1)
			{
				ret -= P_GetRandSeed(i);
			}
			else
			{
				ret += P_GetRandSeed(i);
			}
		}
	}

#ifdef MOBJCONSISTANCY
	if (gamestate == GS_LEVEL)
	{
		for (th = thlist[THINK_MOBJ].next; th != &thlist[THINK_MOBJ]; th = th->next)
		{
			if (th->function.acp1 == (actionf_p1)P_RemoveThinkerDelayed)
				continue;

			mo = (mobj_t *)th;

			if (mo->flags & (MF_SPECIAL | MF_SOLID | MF_PUSHABLE | MF_BOSS | MF_MISSILE | MF_SPRING | MF_MONITOR | MF_FIRE | MF_ENEMY | MF_PAIN | MF_STICKY))
			{
				ret -= mo->type;
				ret += mo->x;
				ret -= mo->y;
				ret += mo->z;
				ret -= mo->momx;
				ret += mo->momy;
				ret -= mo->momz;
				ret += mo->angle;
				ret -= mo->flags;
				ret += mo->flags2;
				ret -= mo->eflags;
				if (mo->target)
				{
					ret += mo->target->type;
					ret -= mo->target->x;
					ret += mo->target->y;
					ret -= mo->target->z;
					ret += mo->target->momx;
					ret -= mo->target->momy;
					ret += mo->target->momz;
					ret -= mo->target->angle;
					ret += mo->target->flags;
					ret -= mo->target->flags2;
					ret += mo->target->eflags;
					ret -= mo->target->state - states;
					ret += mo->target->tics;
					ret -= mo->target->sprite;
					ret += mo->target->frame;
				}
				else
					ret ^= 0x3333;
				if (mo->tracer && mo->tracer->type != MT_OVERLAY)
				{
					ret += mo->tracer->type;
					ret -= mo->tracer->x;
					ret += mo->tracer->y;
					ret -= mo->tracer->z;
					ret += mo->tracer->momx;
					ret -= mo->tracer->momy;
					ret += mo->tracer->momz;
					ret -= mo->tracer->angle;
					ret += mo->tracer->flags;
					ret -= mo->tracer->flags2;
					ret += mo->tracer->eflags;
					ret -= mo->tracer->state - states;
					ret += mo->tracer->tics;
					ret -= mo->tracer->sprite;
					ret += mo->tracer->frame;
				}
				else
					ret ^= 0xAAAA;
				// SRB2Kart: We use hnext & hprev very extensively
				if (mo->hnext && mo->hnext->type != MT_OVERLAY)
				{
					ret += mo->hnext->type;
					ret -= mo->hnext->x;
					ret += mo->hnext->y;
					ret -= mo->hnext->z;
					ret += mo->hnext->momx;
					ret -= mo->hnext->momy;
					ret += mo->hnext->momz;
					ret -= mo->hnext->angle;
					ret += mo->hnext->flags;
					ret -= mo->hnext->flags2;
					ret += mo->hnext->eflags;
					ret -= mo->hnext->state - states;
					ret += mo->hnext->tics;
					ret -= mo->hnext->sprite;
					ret += mo->hnext->frame;
				}
				else
					ret ^= 0x5555;
				if (mo->hprev && mo->hprev->type != MT_OVERLAY)
				{
					ret += mo->hprev->type;
					ret -= mo->hprev->x;
					ret += mo->hprev->y;
					ret -= mo->hprev->z;
					ret += mo->hprev->momx;
					ret -= mo->hprev->momy;
					ret += mo->hprev->momz;
					ret -= mo->hprev->angle;
					ret += mo->hprev->flags;
					ret -= mo->hprev->flags2;
					ret += mo->hprev->eflags;
					ret -= mo->hprev->state - states;
					ret += mo->hprev->tics;
					ret -= mo->hprev->sprite;
					ret += mo->hprev->frame;
				}
				else
					ret ^= 0xCCCC;
				ret -= mo->state - states;
				ret += mo->tics;
				ret -= mo->sprite;
				ret += mo->frame;
			}
		}
	}
#endif

	DEBFILE(va("Consistancy = %u\n", (ret & 0xFFFF)));

	return (INT16)(ret & 0xFFFF);
}

// confusing, but this DOESN'T send PT_NODEKEEPALIVE, it sends PT_BASICKEEPALIVE
// used during wipes to tell the server that a node is still connected
static void CL_SendClientKeepAlive(void)
{
	netbuffer->packettype = PT_BASICKEEPALIVE;

	HSendPacket(servernode, false, 0, 0);
}

static void SV_SendServerKeepAlive(void)
{
	INT32 n;

	for (n = 1; n < MAXNETNODES; n++)
	{
		if (nodeingame[n])
		{
			netbuffer->packettype = PT_BASICKEEPALIVE;
			HSendPacket(n, false, 0, 0);
		}
	}
}

// send the client packet to the server
static void CL_SendClientCmd(void)
{
	size_t packetsize = 0;
	boolean mis = false;

	netbuffer->packettype = PT_CLIENTCMD;

	if (cl_packetmissed)
	{
		netbuffer->packettype = PT_CLIENTMIS;
		mis = true;
	}

	netbuffer->u.clientpak.resendfrom = (UINT8)(neededtic & UINT8_MAX);
	netbuffer->u.clientpak.client_tic = (UINT8)(gametic & UINT8_MAX);

	if (gamestate == GS_WAITINGPLAYERS)
	{
		// Send PT_NODEKEEPALIVE packet
		netbuffer->packettype = (mis ? PT_NODEKEEPALIVEMIS : PT_NODEKEEPALIVE);
		packetsize = sizeof (clientcmd_pak) - sizeof (ticcmd_t) - sizeof (INT16);
		HSendPacket(servernode, false, 0, packetsize);
	}
	else if (gamestate != GS_NULL && (addedtogame || dedicated))
	{
		UINT8 lagDelay = 0;

		if (lowest_lag > 0)
		{
			// Gentlemens' ping.
			lagDelay = min(lowest_lag, MAXGENTLEMENDELAY);
		}

		packetsize = sizeof (clientcmd_pak);
		G_MoveTiccmd(&netbuffer->u.clientpak.cmd, &localcmds[0][lagDelay], 1);
		netbuffer->u.clientpak.consistancy = SHORT(consistancy[gametic % BACKUPTICS]);

		if (splitscreen) // Send a special packet with 2 cmd for splitscreen
		{
			netbuffer->packettype = (mis ? PT_CLIENT2MIS : PT_CLIENT2CMD);
			packetsize = sizeof (client2cmd_pak);
			G_MoveTiccmd(&netbuffer->u.client2pak.cmd2, &localcmds[1][lagDelay], 1);

			if (splitscreen > 1)
			{
				netbuffer->packettype = (mis ? PT_CLIENT3MIS : PT_CLIENT3CMD);
				packetsize = sizeof (client3cmd_pak);
				G_MoveTiccmd(&netbuffer->u.client3pak.cmd3, &localcmds[2][lagDelay], 1);

				if (splitscreen > 2)
				{
					netbuffer->packettype = (mis ? PT_CLIENT4MIS : PT_CLIENT4CMD);
					packetsize = sizeof (client4cmd_pak);
					G_MoveTiccmd(&netbuffer->u.client4pak.cmd4, &localcmds[3][lagDelay], 1);
				}
			}
		}

		HSendPacket(servernode, false, 0, packetsize);
	}

	if (cl_mode == CL_CONNECTED || dedicated)
	{
		UINT8 i;
		// Send extra data if needed
		for (i = 0; i < MAXSPLITSCREENPLAYERS; i++)
		{
			if (localtextcmd[i][0])
			{
				switch (i)
				{
					case 3:
						netbuffer->packettype = PT_TEXTCMD4;
						break;
					case 2:
						netbuffer->packettype = PT_TEXTCMD3;
						break;
					case 1:
						netbuffer->packettype = PT_TEXTCMD2;
						break;
					default:
						netbuffer->packettype = PT_TEXTCMD;
						break;
				}

				M_Memcpy(netbuffer->u.textcmd, localtextcmd[i], localtextcmd[i][0]+1);
				// All extra data have been sent
				if (HSendPacket(servernode, true, 0, localtextcmd[i][0]+1)) // Send can fail...
					localtextcmd[i][0] = 0;
			}
		}
	}
}

// send the server packet
// send tic from firstticstosend to maketic-1
static void SV_SendTics(void)
{
	tic_t realfirsttic, lasttictosend, i;
	UINT32 n;
	INT32 j;
	size_t packsize;
	UINT8 *bufpos;
	UINT8 *ntextcmd;

	// send to all client but not to me
	// for each node create a packet with x tics and send it
	// x is computed using supposedtics[n], max packet size and maketic
	for (n = 1; n < MAXNETNODES; n++)
		if (nodeingame[n])
		{
			// assert supposedtics[n]>=nettics[n]
			realfirsttic = supposedtics[n];

			lasttictosend = nettics[n] + CLIENTBACKUPTICS;
			if (lasttictosend > maketic)
				lasttictosend = maketic;

			if (realfirsttic >= lasttictosend)
			{
				// well we have sent all tics we will so use extrabandwidth
				// to resent packet that are supposed lost (this is necessary since lost
				// packet detection work when we have received packet with firsttic > neededtic
				// (getpacket servertics case)
				DEBFILE(va("Nothing to send node %u mak=%u sup=%u net=%u \n",
					n, lasttictosend, supposedtics[n], nettics[n]));
				realfirsttic = nettics[n];
				if (realfirsttic >= lasttictosend || (I_GetTime() + n)&3)
					// all tic are ok
					continue;
				DEBFILE(va("Sent %d anyway\n", realfirsttic));
			}
			if (realfirsttic < firstticstosend)
				realfirsttic = firstticstosend;

			// compute the length of the packet and cut it if too large
			packsize = BASESERVERTICSSIZE;
			for (i = realfirsttic; i < lasttictosend; i++)
			{
				packsize += sizeof (ticcmd_t) * doomcom->numslots;
				packsize += TotalTextCmdPerTic(i);

				if (packsize > software_MAXPACKETLENGTH)
				{
					DEBFILE(va("packet too large (%s) at tic %d (should be from %d to %d)\n",
						sizeu1(packsize), i, realfirsttic, lasttictosend));
					lasttictosend = i;

					// too bad: too much player have send extradata and there is too
					//          much data in one tic.
					// To avoid it put the data on the next tic. (see getpacket
					// textcmd case) but when numplayer changes the computation can be different
					if (lasttictosend == realfirsttic)
					{
						if (packsize > MAXPACKETLENGTH)
							I_Error("Too many players: can't send %s data for %d players to node %d\n"
							        "Well sorry nobody is perfect....\n",
							        sizeu1(packsize), doomcom->numslots, n);
						else
						{
							lasttictosend++; // send it anyway!
							DEBFILE("sending it anyway\n");
						}
					}
					break;
				}
			}

			// Send the tics
			netbuffer->packettype = PT_SERVERTICS;
			netbuffer->u.serverpak.starttic = (UINT8)realfirsttic;
			netbuffer->u.serverpak.numtics = (UINT8)(lasttictosend - realfirsttic);
			netbuffer->u.serverpak.numslots = (UINT8)SHORT(doomcom->numslots);
			bufpos = (UINT8 *)&netbuffer->u.serverpak.cmds;

			for (i = realfirsttic; i < lasttictosend; i++)
			{
				bufpos = G_DcpyTiccmd(bufpos, netcmds[i%BACKUPTICS], doomcom->numslots * sizeof (ticcmd_t));
			}

			// add textcmds
			for (i = realfirsttic; i < lasttictosend; i++)
			{
				ntextcmd = bufpos++;
				*ntextcmd = 0;
				for (j = 0; j < MAXPLAYERS; j++)
				{
					UINT8 *textcmd = D_GetExistingTextcmd(i, j);
					INT32 size = textcmd ? textcmd[0] : 0;

					if ((!j || playeringame[j]) && size)
					{
						(*ntextcmd)++;
						WRITEUINT8(bufpos, j);
						M_Memcpy(bufpos, textcmd, size + 1);
						bufpos += size + 1;
					}
				}
			}
			packsize = bufpos - (UINT8 *)&(netbuffer->u);

			HSendPacket(n, false, 0, packsize);
			// when tic are too large, only one tic is sent so don't go backward!
			if (lasttictosend-doomcom->extratics > realfirsttic)
				supposedtics[n] = lasttictosend-doomcom->extratics;
			else
				supposedtics[n] = lasttictosend;
			if (supposedtics[n] < nettics[n]) supposedtics[n] = nettics[n];
		}
	// node 0 is me!
	supposedtics[0] = maketic;
}

//
// TryRunTics
//
static void CreateNewLocalCMD(UINT8 p, INT32 realtics)
{
	INT32 i;

	for (i = MAXGENTLEMENDELAY-1; i > 0; i--)
	{
		G_MoveTiccmd(&localcmds[p][i], &localcmds[p][i-1], 1);
	}

	G_BuildTiccmd(&localcmds[p][0], realtics, p+1);
	localcmds[p][0].flags |= TICCMD_RECEIVED;
}

static void Local_Maketic(INT32 realtics)
{
	INT32 i;

	I_OsPolling(); // I_Getevent
	D_ProcessEvents(); // menu responder, cons responder,
	                   // game responder calls HU_Responder, AM_Responder,
	                   // and G_MapEventsToControls

	if (!dedicated) rendergametic = gametic;

	// translate inputs (keyboard/mouse/joystick) into game controls
	for (i = 0; i <= splitscreen; i++)
	{
		CreateNewLocalCMD(i, realtics);
	}
}

// create missed tic
static void SV_Maketic(void)
{
	INT32 i;

	PS_ResetBotInfo();

	for (i = 0; i < MAXPLAYERS; i++)
	{
		if (!playeringame[i])
			continue;

		if (K_PlayerUsesBotMovement(&players[i]))
		{
			const precise_t t = I_GetPreciseTime();

			K_BuildBotTiccmd(&players[i], &netcmds[maketic%BACKUPTICS][i]);

			ps_bots[i].isBot = true;
			ps_bots[i].total = I_GetPreciseTime() - t;
			ps_botticcmd_time += ps_bots[i].total;
			continue;
		}

		// We didn't receive this tic
		if ((netcmds[maketic % BACKUPTICS][i].flags & TICCMD_RECEIVED) == 0)
		{
			ticcmd_t *    ticcmd = &netcmds[(maketic    ) % BACKUPTICS][i];
			ticcmd_t *prevticcmd = &netcmds[(maketic - 1) % BACKUPTICS][i];

			{
				DEBFILE(va("MISS tic%4d for player %d\n", maketic, i));
				// Copy the input from the previous tic
				*ticcmd = *prevticcmd;
				ticcmd->flags &= ~TICCMD_RECEIVED;
			}
		}
	}

	// all tic are now proceed make the next
	maketic++;
}

boolean TryRunTics(tic_t realtics)
{
	boolean ticking;

	// the machine has lagged but it is not so bad
	if (realtics > TICRATE/7) // FIXME: consistency failure!!
	{
		if (server)
			realtics = 1;
		else
			realtics = TICRATE/7;
	}

	if (singletics)
		realtics = 1;

	if (realtics >= 1)
	{
		COM_BufTicker();
		if (mapchangepending)
			D_MapChange(-1, 0, encoremode, false, 2, false, fromlevelselect); // finish the map change
	}

	NetUpdate();

	if (demo.playback)
	{
		neededtic = gametic + realtics;
		// start a game after a demo
		maketic += realtics;
		firstticstosend = maketic;
		tictoclear = firstticstosend;
	}

	GetPackets();

#ifdef DEBUGFILE
	if (debugfile && (realtics || neededtic > gametic))
	{
		//SoM: 3/30/2000: Need long INT32 in the format string for args 4 & 5.
		//Shut up stupid warning!
		fprintf(debugfile, "------------ Tryruntic: REAL:%d NEED:%d GAME:%d LOAD: %d\n",
			realtics, neededtic, gametic, debugload);
		debugload = 100000;
	}
#endif

	ticking = neededtic > gametic;

	if (ticking)
	{
		if (realtics)
			hu_stopped = false;
	}

	if (player_joining)
	{
		if (realtics)
			hu_stopped = true;
		return false;
	}

	if (ticking)
	{
		{
			// run the count * tics
			while (neededtic > gametic)
			{
				DEBFILE(va("============ Running tic %d (local %d)\n", gametic, localgametic));

				ps_tictime = I_GetPreciseTime();

				G_Ticker((gametic % NEWTICRATERATIO) == 0);
				if (Playing() && netgame && (gametic % TICRATE == 0))
				{
					Schedule_Run();

					if (cv_livestudioaudience.value)
					{
						LiveStudioAudience();
					}
				}

				ExtraDataTicker();

				gametic++;
				consistancy[gametic%BACKUPTICS] = Consistancy();

				ps_tictime = I_GetPreciseTime() - ps_tictime;

				// Leave a certain amount of tics present in the net buffer as long as we've ran at least one tic this frame.
				if (client && gamestate == GS_LEVEL && leveltime > 3 && neededtic <= gametic + cv_netticbuffer.value)
					break;
			}
		}
	}
	else
	{
		if (realtics)
			hu_stopped = true;
	}

	return ticking;
}


/*	Ping Update except better:
	We call this once per second and check for people's pings. If their ping happens to be too high, we increment some timer and kick them out.
	If they're not lagging, decrement the timer by 1. Of course, reset all of this if they leave.
*/

static INT32 pingtimeout[MAXPLAYERS];

static inline void PingUpdate(void)
{
	INT32 i;
	boolean pingkick[MAXPLAYERS];
	UINT8 nonlaggers = 0;
	memset(pingkick, 0, sizeof(pingkick));

	netbuffer->packettype = PT_PING;

	//check for ping limit breakage.
	if (cv_maxping.value)
	{
		for (i = 0; i < MAXPLAYERS; i++)
		{
			if (!playeringame[i] || P_IsMachineLocalPlayer(&players[i]))
			{
				pingtimeout[i] = 0;
				continue;
			}

			if ((cv_maxping.value)
				&& (realpingtable[i] / pingmeasurecount > (unsigned)cv_maxping.value))
			{
				if (players[i].jointime > 10 * TICRATE)
				{
					pingkick[i] = true;
				}
			}
			else
			{
				nonlaggers++;

				// you aren't lagging, but you aren't free yet. In case you'll keep spiking, we just make the timer go back down. (Very unstable net must still get kicked).
				if (pingtimeout[i] > 0)
					pingtimeout[i]--;
			}
		}

		//kick lagging players... unless everyone but the server's ping sucks.
		//in that case, it is probably the server's fault.
		if (nonlaggers > 0)
		{
			for (i = 0; i < MAXPLAYERS; i++)
			{
				if (!playeringame[i] || !pingkick[i])
					continue;

				// Don't kick on ping alone if we haven't reached our threshold yet.
				if (++pingtimeout[i] < cv_pingtimeout.value)
					continue;

				pingtimeout[i] = 0;
				SendKick(i, KICK_MSG_PING_HIGH);
			}
		}
	}

	//make the ping packet and clear server data for next one
	for (i = 0; i < MAXPLAYERS; i++)
	{
		//CONS_Printf("player %d - total pings: %d\n", i, realpingtable[i]);

		netbuffer->u.pingtable[i] = realpingtable[i] / pingmeasurecount;
		//server takes a snapshot of the real ping for display.
		//otherwise, pings fluctuate a lot and would be odd to look at.
		playerpingtable[i] = realpingtable[i] / pingmeasurecount;
		realpingtable[i] = 0; //Reset each as we go.
	}

	// send the server's maxping as last element of our ping table. This is useful to let us know when we're about to get kicked.
	netbuffer->u.pingtable[MAXPLAYERS] = cv_maxping.value;

	//send out our ping packets
	for (i = 0; i < MAXNETNODES; i++)
		if (nodeingame[i])
			HSendPacket(i, true, 0, sizeof(INT32) * (MAXPLAYERS+1));

	pingmeasurecount = 0; //Reset count
}

static tic_t gametime = 0;

static void UpdatePingTable(void)
{
	tic_t fastest;
	tic_t lag;

	INT32 i;

	if (server)
	{
		if (Playing() && !(gametime % 35))	// update once per second.
			PingUpdate();

		fastest = 0;

		// update node latency values so we can take an average later.
		for (i = 0; i < MAXPLAYERS; i++)
		{
			if (playeringame[i] && playernode[i] > 0)
			{
				if (! server_lagless && playernode[i] > 0 && !players[i].spectator)
				{
					lag = GetLag(playernode[i]);
					realpingtable[i] += lag;

					if (! fastest || lag < fastest)
						fastest = lag;
				}
				else
				{
					// TicsToMilliseconds can't handle pings over 1000ms lol
					realpingtable[i] += GetLag(playernode[i]);
				}
			}
		}

		// Don't gentleman below your mindelay
		if (fastest < (tic_t)cv_mindelay.value)
			fastest = (tic_t)cv_mindelay.value;

		pingmeasurecount++;

		if (server_lagless)
			lowest_lag = 0;
		else
		{
			lowest_lag = fastest;

			if (fastest)
				lag = fastest;
			else
				lag = GetLag(0);

			lag = ( realpingtable[0] + lag );

			switch (playerpernode[0])
			{
				case 4:
					realpingtable[nodetoplayer4[0]] = lag;
					/*FALLTHRU*/
				case 3:
					realpingtable[nodetoplayer3[0]] = lag;
					/*FALLTHRU*/
				case 2:
					realpingtable[nodetoplayer2[0]] = lag;
					/*FALLTHRU*/
				case 1:
					realpingtable[nodetoplayer[0]] = lag;
			}
		}
	}
	else // We're a client, handle mindelay on the way out.
	{
		if ((neededtic - gametic) < (tic_t)cv_mindelay.value)
			lowest_lag = cv_mindelay.value - (neededtic - gametic);
	}
}

// It's that time again! Send everyone a safe message to sign, so we can show off their signature and prove we're playing fair.
static void SendChallenges(void)
{
	int i;
	netbuffer->packettype = PT_CHALLENGEALL;

	#ifdef DEVELOP
		if (cv_nochallenge.value)
		{
			CV_AddValue(&cv_nochallenge, -1);
			CONS_Alert(CONS_WARNING, "cv_nochallenge enabled, not sending PT_CHALLENGEALL\n");
			return;
		}
	#endif

	memset(knownWhenChallenged, 0, sizeof(knownWhenChallenged));

	GenerateChallenge(netbuffer->u.challengeall.secret);
	memcpy(lastChallengeAll, netbuffer->u.challengeall.secret, sizeof(lastChallengeAll));

	memset(lastReceivedSignature, 0, sizeof(lastReceivedSignature));

	for (i = 0; i < MAXNETNODES; i++)
	{
		if (nodeingame[i])
		{
			CONS_Printf("challenge to node %d, player %d\n", i, nodetoplayer[i]);
			HSendPacket(i, true, 0, sizeof(challengeall_pak));
			memcpy(knownWhenChallenged[nodetoplayer[i]], players[nodetoplayer[i]].public_key, sizeof(knownWhenChallenged[nodetoplayer[i]]));
		}
	}
}

// Before we start sending out the results, we need to kick everyone who didn't respond.
// (If we try to do both at once, clients will still see players who failled in-game when the results arrive...)
static void KickUnverifiedPlayers(void)
{
	int i;
	uint8_t allZero[64];
	memset(allZero, 0, sizeof(allZero));

	for (i = 0; i < MAXPLAYERS; i++)
	{
		if (!playeringame[i])
			continue;
		if (memcmp(lastReceivedSignature[i], allZero, sizeof(allZero)) == 0) // We never got a response!
		{
			if (!IsPlayerGuest(i) && memcmp(knownWhenChallenged[i], players[i].public_key, sizeof(knownWhenChallenged[i]) == 0))
			{
				if (playernode[i] != servernode)
				{
					CONS_Printf("We never got a response from player %d, goodbye\n", i);
					SendKick(i, KICK_MSG_SIGFAIL);
				}	
			}
		}
	}	
}

// 
static void SendChallengeResults(void)
{
	int i;
	netbuffer->packettype = PT_RESULTSALL;

	#ifdef DEVELOP
		if (cv_noresults.value)
		{
			CV_AddValue(&cv_noresults, -1);
			CONS_Alert(CONS_WARNING, "cv_noresults enabled, not sending PT_RESULTSALL\n");
			return;
		}
	#endif

	uint8_t allZero[64];
	memset(allZero, 0, sizeof(allZero));

	memset(&netbuffer->u.resultsall, 0, sizeof(netbuffer->u.resultsall));

	for (i = 0; i < MAXPLAYERS; i++)
	{
		if (!playeringame[i])
			continue;

		// Don't try to transmit signatures for players who didn't get here in time to send one.
		// (Everyone who had their chance should have been kicked by KickUnverifiedPlayers by now.)
		if (memcmp(lastReceivedSignature[i], allZero, sizeof(allZero)) == 0)
			continue;

		CONS_Printf("Player %d passed with key %s sig %s, adding...\n", i, GetPrettyRRID(players[i].public_key, true), GetPrettyRRID(lastReceivedSignature[i], true));
		memcpy(netbuffer->u.resultsall.signature[i], lastReceivedSignature[i], sizeof(netbuffer->u.resultsall.signature[i]));
		#ifdef DEVELOP
			if (cv_badresults.value)
			{
				CV_AddValue(&cv_badresults, -1);
				CONS_Alert(CONS_WARNING, "cv_badresults enabled, scrubbing signature from PT_RESULTSALL\n");
				memset(netbuffer->u.resultsall.signature[i], 0, sizeof(netbuffer->u.resultsall.signature[i]));
			}
		#endif
	}

	for (i = 0; i < MAXNETNODES; i++)
	{
		if (nodeingame[i])
		{
			CONS_Printf("results to node %d, player %d\n", i, nodetoplayer[i]);
			HSendPacket(i, true, 0, sizeof(resultsall_pak));
		}
	}
}

// Who should we try to verify when results come in?
// Store a public key for every active slot, so if players shuffle during challenge leniency,
// we don't incorrectly try to verify someone who didn't even get a challenge, throw a tantrum, and bail.
static void CheckPresentPlayers(void)
{
	int i;
	memset(knownWhenChallenged, 0, sizeof(knownWhenChallenged));

	for (i = 0; i < MAXPLAYERS; i++)
	{
		if (!playeringame[i])
		{
			//CONS_Printf("Player %i isn't present for checkall\n", i);
			continue;
		}
		else if (IsPlayerGuest(i))
		{
			//CONS_Printf("Player %i is present for checkall, but is a guest\n", i);
			continue;
		}
		else
		{
			CONS_Printf("Player %d (node %d split %d) is present for checkall, make a note of their key %s...\n", i, playernode[i], players[i].splitscreenindex, 
				GetPrettyRRID(players[i].public_key, true));
			memcpy(knownWhenChallenged[i], players[i].public_key, sizeof(knownWhenChallenged[i]));
		}
	}
}

// Handle "client-to-client" auth challenge flow.
static void UpdateChallenges(void)
{
	if (!(Playing() && netgame))
		return;

	if (server)
	{
		if (leveltime == CHALLENGEALL_START)
			SendChallenges();

		if (leveltime == CHALLENGEALL_KICKUNRESPONSIVE)
			KickUnverifiedPlayers();

		if (leveltime == CHALLENGEALL_SENDRESULTS)
			SendChallengeResults();

	}
	else // client
	{
		if (leveltime <= CHALLENGEALL_START)
		expectChallenge = true;

		if (leveltime == CHALLENGEALL_START)
			CheckPresentPlayers();

		if (leveltime > CHALLENGEALL_CLIENTCUTOFF && expectChallenge)
			HandleSigfail("Didn't receive client signatures.");
	}
}

static void RenewHolePunch(void)
{
	static time_t past;

	const time_t now = time(NULL);

	if ((now - past) > 20)
	{
		I_NetRegisterHolePunch();
		past = now;
	}
}

// Handle timeouts to prevent definitive freezes from happenning
static void HandleNodeTimeouts(void)
{
	INT32 i;

	if (server)
	{
		for (i = 1; i < MAXNETNODES; i++)
			if ((nodeingame[i] || nodeneedsauth[i]) && freezetimeout[i] < I_GetTime())
				Net_ConnectionTimeout(i);

		// In case the cvar value was lowered
		if (joindelay)
			joindelay = min(joindelay - 1, 3 * (tic_t)cv_joindelay.value * TICRATE);
	}
}

// Keep the network alive while not advancing tics!
void NetKeepAlive(void)
{
	tic_t nowtime;
	INT32 realtics;

	nowtime = I_GetTime();
	realtics = nowtime - gametime;

	// return if there's no time passed since the last call
	if (realtics <= 0) // nothing new to update
		return;

	UpdatePingTable();

	UpdateChallenges();

	GetPackets();

#ifdef MASTERSERVER
	MasterClient_Ticker();
#endif

	if (netgame && serverrunning)
	{
		RenewHolePunch();
	}

	if (client)
	{
		// send keep alive
		CL_SendClientKeepAlive();
		// No need to check for resynch because we aren't running any tics
	}
	else
	{
		SV_SendServerKeepAlive();
	}

	// No else because no tics are being run and we can't resynch during this

	Net_AckTicker();
	HandleNodeTimeouts();
	FileSendTicker();
}

// If a tree falls in the forest but nobody is around to hear it, does it make a tic?
#define DEDICATEDIDLETIME (10*TICRATE)

void NetUpdate(void)
{
	static tic_t resptime = 0;
	tic_t nowtime;
	INT32 i;
	INT32 realtics;

	nowtime = I_GetTime();
	realtics = nowtime - gametime;

	if (realtics <= 0) // nothing new to update
		return;

	if (realtics > 5)
	{
		if (server)
			realtics = 1;
		else
			realtics = 5;
	}

#ifdef DEDICATEDIDLETIME
	if (server && dedicated && gamestate == GS_LEVEL)
	{
		static tic_t dedicatedidle = 0;

		for (i = 1; i < MAXNETNODES; ++i)
			if (nodeingame[i])
			{
				if (dedicatedidle == DEDICATEDIDLETIME)
				{
					CONS_Printf("DEDICATED: Awakening from idle (Node %d detected...)\n", i);
					dedicatedidle = 0;
				}
				break;
			}

		if (i == MAXNETNODES)
		{
			if (leveltime == 2)
			{
				// On next tick...
				dedicatedidle = DEDICATEDIDLETIME-1;
			}
			else if (dedicatedidle == DEDICATEDIDLETIME)
			{
				if (D_GetExistingTextcmd(gametic, 0) || D_GetExistingTextcmd(gametic+1, 0))
				{
					CONS_Printf("DEDICATED: Awakening from idle (Netxcmd detected...)\n");
					dedicatedidle = 0;
				}
				else
				{
					realtics = 0;
				}
			}
			else if ((dedicatedidle += realtics) >= DEDICATEDIDLETIME)
			{
				const char *idlereason = "at round start";
				if (leveltime > 3)
					idlereason = va("for %d seconds", dedicatedidle/TICRATE);

				CONS_Printf("DEDICATED: No nodes %s, idling...\n", idlereason);
				realtics = 0;
				dedicatedidle = DEDICATEDIDLETIME;
			}
		}
	}
#endif

	gametime = nowtime;

	UpdatePingTable();

	UpdateChallenges();

	if (client)
		maketic = neededtic;

	Local_Maketic(realtics); // make local tic, and call menu?

	if (server)
		CL_SendClientCmd(); // send it

	GetPackets(); // get packet from client or from server

	// client send the command after a receive of the server
	// the server send before because in single player is beter

#ifdef MASTERSERVER
	MasterClient_Ticker(); // Acking the Master Server
#endif

	if (netgame && serverrunning)
	{
		RenewHolePunch();
	}

	if (client)
	{
		// If the client just finished redownloading the game state, load it
		if (cl_redownloadinggamestate && fileneeded[0].status == FS_FOUND)
			CL_ReloadReceivedSavegame();

		CL_SendClientCmd(); // Send tic cmd
		hu_redownloadinggamestate = cl_redownloadinggamestate;
	}
	else
	{
		if (!demo.playback && realtics > 0)
		{
			INT32 counts;

			hu_redownloadinggamestate = false;

			// Don't erase tics not acknowledged
			counts = realtics;

			firstticstosend = gametic;
			for (i = 0; i < MAXNETNODES; i++)
			{
				if (!nodeingame[i])
					continue;
				if (nettics[i] < firstticstosend)
					firstticstosend = nettics[i];
				if (maketic + counts >= nettics[i] + (BACKUPTICS - TICRATE))
					Net_ConnectionTimeout(i);
			}

			if (maketic + counts >= firstticstosend + BACKUPTICS)
				counts = firstticstosend+BACKUPTICS-maketic-1;

			for (i = 0; i < counts; i++)
				SV_Maketic(); // Create missed tics and increment maketic

			for (; tictoclear < firstticstosend; tictoclear++) // Clear only when acknowledged
				D_Clearticcmd(tictoclear);                    // Clear the maketic the new tic

			SV_SendTics();

			neededtic = maketic; // The server is a client too
		}
	}

	Net_AckTicker();
	HandleNodeTimeouts();

	nowtime /= NEWTICRATERATIO;

	if (nowtime > resptime)
	{
		resptime = nowtime;
#ifdef HAVE_THREADS
		I_lock_mutex(&k_menu_mutex);
#endif
		M_Ticker();
#ifdef HAVE_THREADS
		I_unlock_mutex(k_menu_mutex);
#endif
		CON_Ticker();
	}

	FileSendTicker();
}

/** Returns the number of players playing.
  * \return Number of players. Can be zero if we're running a ::dedicated
  *         server.
  * \author Graue <graue@oceanbase.org>
  */
INT32 D_NumPlayers(void)
{
	INT32 num = 0, ix;

	for (ix = 0; ix < MAXPLAYERS; ix++)
	{
		if (playeringame[ix] && !players[ix].bot)
		{
			num++;
		}
	}

	return num;
}

/** Return whether a player is a real person (not a CPU) and not spectating.
  */
boolean D_IsPlayerHumanAndGaming (INT32 player_number)
{
	player_t * player = &players[player_number];
	return (
			playeringame[player_number] &&
			! player->spectator &&
			! player->bot
	);
}

tic_t GetLag(INT32 node)
{
	// If the client has caught up to the server -- say, during a wipe -- lag is meaningless.
	if (nettics[node] > gametic)
		return 0;
	return gametic - nettics[node];
}

#define REWIND_POINT_INTERVAL 4*TICRATE + 16
rewind_t *rewindhead;

void CL_ClearRewinds(void)
{
	rewind_t *head;
	while ((head = rewindhead))
	{
		rewindhead = rewindhead->next;
		free(head);
	}
}

rewind_t *CL_SaveRewindPoint(size_t demopos)
{
	savebuffer_t save = {0};
	rewind_t *rewind;

	if (rewindhead && rewindhead->leveltime + REWIND_POINT_INTERVAL > leveltime)
		return NULL;

	rewind = (rewind_t *)malloc(sizeof (rewind_t));
	if (!rewind)
		return NULL;

	P_SaveBufferFromExisting(&save, rewind->savebuffer, NETSAVEGAMESIZE);
	P_SaveNetGame(&save, false);

	rewind->leveltime = leveltime;
	rewind->next = rewindhead;
	rewind->demopos = demopos;
	rewindhead = rewind;

	return rewind;
}

rewind_t *CL_RewindToTime(tic_t time)
{
	savebuffer_t save = {0};
	rewind_t *rewind;

	while (rewindhead && rewindhead->leveltime > time)
	{
		rewind = rewindhead->next;
		free(rewindhead);
		rewindhead = rewind;
	}

	if (!rewindhead)
		return NULL;

	P_SaveBufferFromExisting(&save, rewindhead->savebuffer, NETSAVEGAMESIZE);
	P_LoadNetGame(&save, false);

	wipegamestate = gamestate; // No fading back in!
	timeinmap = leveltime;

	return rewindhead;
}

void D_MD5PasswordPass(const UINT8 *buffer, size_t len, const char *salt, void *dest)
{
#ifdef NOMD5
	(void)buffer;
	(void)len;
	(void)salt;
	memset(dest, 0, 16);
#else
	char tmpbuf[256];
	const size_t sl = strlen(salt);

	if (len > 256-sl)
		len = 256-sl;

	memcpy(tmpbuf, buffer, len);
	memmove(&tmpbuf[len], salt, sl);
	//strcpy(&tmpbuf[len], salt);
	len += strlen(salt);
	if (len < 256)
		memset(&tmpbuf[len],0,256-len);

	// Yes, we intentionally md5 the ENTIRE buffer regardless of size...
	md5_buffer(tmpbuf, 256, dest);
#endif
}
