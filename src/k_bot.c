// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 2007-2016 by John "JTE" Muniz.
// Copyright (C) 2011-2018 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  k_bot.c
/// \brief Basic bot handling

#include "doomdef.h"
#include "d_player.h"
#include "g_game.h"
#include "r_main.h"
#include "p_local.h"
#include "k_bot.h"
#include "lua_hook.h"
#include "byteptr.h"
#include "d_net.h" // nodetoplayer
#include "k_kart.h"
#include "z_zone.h"
#include "i_system.h"
#include "p_maputl.h"
#include "d_ticcmd.h"

void K_AddBots(SINT8 numbots)
{
	UINT8 newplayernum = 0;
	UINT8 difficulty = MAXBOTDIFFICULTY;

	if (dedicated)
		newplayernum = 1;

	while (numbots > 0)
	{
		UINT8 buf[3];
		UINT8 *buf_p = buf;

		numbots--;

		// search for a free playernum
		// we can't use playeringame since it is not updated here
		for (; newplayernum < MAXPLAYERS; newplayernum++)
		{
			UINT8 n;

			for (n = 0; n < MAXNETNODES; n++)
				if (nodetoplayer[n] == newplayernum
				|| nodetoplayer2[n] == newplayernum
				|| nodetoplayer3[n] == newplayernum
				|| nodetoplayer4[n] == newplayernum)
					break;

			if (n == MAXNETNODES)
				break;
		}

		WRITEUINT8(buf_p, newplayernum);

		// test skins
		if (numbots == 6)
		{
			difficulty = MAXBOTDIFFICULTY;
			WRITEUINT8(buf_p, 1);
		}
		else if (numbots == 5)
		{
			difficulty = MAXBOTDIFFICULTY;
			WRITEUINT8(buf_p, 0);
		}
		else if (numbots == 4)
		{
			difficulty = MAXBOTDIFFICULTY-1;
			WRITEUINT8(buf_p, 2);
		}
		else if (numbots == 3)
		{
			difficulty = MAXBOTDIFFICULTY-2;
			WRITEUINT8(buf_p, 3);
		}
		else if (numbots == 2)
		{
			difficulty = MAXBOTDIFFICULTY-4;
			WRITEUINT8(buf_p, 5);
		}
		else if (numbots == 1)
		{
			difficulty = MAXBOTDIFFICULTY-4;
			WRITEUINT8(buf_p, 9);
		}
		else
		{
			difficulty = MAXBOTDIFFICULTY-2;
			WRITEUINT8(buf_p, 10);
		}

		WRITEUINT8(buf_p, difficulty);

		SendNetXCmd(XD_ADDBOT, buf, buf_p - buf);

		if (difficulty > 0)
		{
			difficulty--;
		}

		DEBFILE(va("Server added bot %d\n", newplayernum));
		// use the next free slot (we can't put playeringame[newplayernum] = true here)
		newplayernum++;
	}
}

boolean K_PlayerUsesBotMovement(player_t *player)
{
	if (player->bot || player->exiting)
		return true;

	return false;
}

boolean K_BotCanTakeCut(player_t *player)
{
	if (!K_ApplyOffroad(player)
		|| player->kartstuff[k_itemtype] == KITEM_SNEAKER
		|| player->kartstuff[k_itemtype] == KITEM_INVINCIBILITY
		|| player->kartstuff[k_itemtype] == KITEM_HYUDORO)
		return true;

	return false;
}

fixed_t K_BotRubberband(player_t *player)
{
	fixed_t rubberband = FRACUNIT;
	player_t *firstplace = NULL;
	UINT8 i;

	if (player->exiting)
	{
		return FRACUNIT;
	}

	for (i = 0; i < MAXPLAYERS; i++)
	{
		if (!playeringame[i] || players[i].spectator || players[i].exiting)
		{
			continue;
		}

		/*if (players[i].bot)
		{
			continue;
		}*/

		if (firstplace == NULL || players[i].distancetofinish < firstplace->distancetofinish)
		{
			firstplace = &players[i];
		}
	}

	if (firstplace != NULL)
	{
		const UINT32 spacing = 4096;
		UINT32 easiness = (MAXBOTDIFFICULTY - player->botvars.difficulty);
		UINT32 wanteddist = firstplace->distancetofinish + (spacing * easiness);

		if (wanteddist < player->distancetofinish)
		{
			// Catch up to 1st!
			rubberband = FRACUNIT + ((3*player->botvars.difficulty/2) * (player->distancetofinish - wanteddist));
		}
	}

	if (rubberband > 2*FRACUNIT)
	{
		rubberband = 2*FRACUNIT;
	}
	else if (rubberband < FRACUNIT)
	{
		rubberband = FRACUNIT;
	}

	return rubberband;
}

static fixed_t K_DistanceOfLineFromPoint(fixed_t v1x, fixed_t v1y, fixed_t v2x, fixed_t v2y, fixed_t cx, fixed_t cy)
{
	fixed_t v1toc[2] = {cx - v1x, cy - v1y};
	fixed_t v1tov2[2] = {v2x - v1x, v2y - v1y};

	fixed_t mag = FixedMul(v1tov2[0], v1tov2[0]) + FixedMul(v1tov2[1], v1tov2[1]);
	fixed_t dot = FixedMul(v1toc[0], v1tov2[0]) + FixedMul(v1toc[1], v1tov2[1]);

	fixed_t t;
	fixed_t px, py;

	if (mag == 0)
	{
		return 0;
	}

	t = FixedDiv(dot, mag);

	px = v1x + FixedMul(v1tov2[0], t);
	py = v1y + FixedMul(v1tov2[1], t);

	return P_AproxDistance(cx - px, cy - py);
}

static botprediction_t *K_CreateBotPrediction(player_t *player)
{
	const INT32 distance = (player->speed / FRACUNIT) * futuresight;
	INT32 distanceleft = distance;
	botprediction_t *predict = Z_Calloc(sizeof(botprediction_t), PU_LEVEL, NULL);
	waypoint_t *wp = player->nextwaypoint;
	fixed_t smallestradius = INT32_MAX;
	size_t nwp;
	size_t i;

	if (distance <= 0)
	{
		predict->x = wp->mobj->x;
		predict->y = wp->mobj->y;
		predict->radius = wp->mobj->radius;
		return predict;
	}

	while (distanceleft > 0)
	{
		if (wp->mobj->radius < smallestradius)
		{
			smallestradius = wp->mobj->radius;
		}

		nwp = 0;

		if (wp->numnextwaypoints == 0)
		{
			distanceleft = 0;
			break;
		}

		if (wp->numnextwaypoints > 1)
		{
			fixed_t closest = INT32_MAX;
			fixed_t dist    = INT32_MAX;

			for (i = 0; i < wp->numnextwaypoints; i++)
			{
				if (K_GetWaypointIsShortcut(wp->nextwaypoints[i]) && !K_BotCanTakeCut(player))
				{
					continue;
				}

				dist = P_AproxDistance(
					player->mo->x - wp->nextwaypoints[i]->mobj->x,
					player->mo->y - wp->nextwaypoints[i]->mobj->y
				);

				if (dist < closest)
				{
					nwp = i;
					closest = dist;
				}
			}
		}

		if ((INT32)(wp->nextwaypointdistances[nwp]) > distanceleft)
		{
			break;
		}

		distanceleft -= wp->nextwaypointdistances[nwp];

		wp = wp->nextwaypoints[nwp];
	}

	predict->x = wp->mobj->x;
	predict->y = wp->mobj->y;
	predict->radius = smallestradius;

	if (distanceleft > 0)
	{
		angle_t a = R_PointToAngle2(wp->mobj->x, wp->mobj->y, wp->nextwaypoints[nwp]->mobj->x, wp->nextwaypoints[nwp]->mobj->y);

		predict->x += P_ReturnThrustX(NULL, a, distanceleft * FRACUNIT);
		predict->y += P_ReturnThrustY(NULL, a, distanceleft * FRACUNIT);
	}

	return predict;
}

mobj_t *botmo = NULL;
fixed_t distancetocheck = 0;
INT16 badsteerglobal = 0;
fixed_t predictx = 0, predicty = 0;

static void K_SteerFromObject(mobj_t *bot, mobj_t *thing, boolean towards, INT16 amount)
{
	angle_t destangle = R_PointToAngle2(bot->x, bot->y, thing->x, thing->y);
	angle_t angle;

	if (towards)
	{
		fixed_t dist = K_DistanceOfLineFromPoint(
			bot->x, bot->y,
			bot->x + FINECOSINE(bot->angle >> ANGLETOFINESHIFT), bot->y + FINESINE(bot->angle >> ANGLETOFINESHIFT),
			thing->x, thing->y
		);

		if (dist <= (bot->radius + thing->radius))
		{
			// Don't need to turn any harder!
			return;
		}

		amount = -amount;
	}

	angle = (bot->angle - destangle);

	if (angle < ANGLE_180)
	{
		badsteerglobal -= amount;
	}
	else
	{
		badsteerglobal += amount;
	}
}

static boolean K_BotSteerObjects(mobj_t *thing)
{
	INT16 anglediff;
	fixed_t dist;
	angle_t destangle, angle;
	INT16 attack = ((9 - botmo->player->kartspeed) * KART_FULLTURN) / 8; // Acceleration chars are more aggressive
	INT16 dodge = ((9 - botmo->player->kartweight) * KART_FULLTURN) / 8; // Handling chars dodge better

#define PlayerAttackSteer(botcond, thingcond) \
	if ((botcond) && !(thingcond)) \
	{ \
		K_SteerFromObject(botmo, thing, true, KART_FULLTURN + attack); \
	} \
	else if ((thingcond) && !(botcond)) \
	{ \
		K_SteerFromObject(botmo, thing, false, KART_FULLTURN + dodge); \
	}

	if (!botmo || P_MobjWasRemoved(botmo) || !botmo->player)
	{
		return false;
	}

	if (!thing->health)
	{
		return true;
	}

	if (botmo == thing)
	{
		return true;
	}

	dist = P_AproxDistance(P_AproxDistance(
		botmo->x - thing->x,
		botmo->y - thing->y),
		(botmo->z - thing->z) / 4
	);

	if (dist > distancetocheck)
	{
		return true;
	}

	if (!P_CheckSight(botmo, thing))
	{
		return true;
	}

	destangle = R_PointToAngle2(botmo->x, botmo->y, thing->x, thing->y);
	angle = (botmo->angle - destangle);

	if (angle < ANGLE_180)
	{
		anglediff = AngleFixed(angle)>>FRACBITS;
	}
	else 
	{
		anglediff = 360-(AngleFixed(angle)>>FRACBITS);
	}

	anglediff = abs(anglediff);

	switch (thing->type)
	{
		case MT_BANANA:
		case MT_BANANA_SHIELD:
		case MT_EGGMANITEM:
		case MT_EGGMANITEM_SHIELD:
		case MT_ORBINAUT:
		case MT_ORBINAUT_SHIELD:
		case MT_JAWZ:
		case MT_JAWZ_DUD:
		case MT_JAWZ_SHIELD:
		case MT_SSMINE:
		case MT_SSMINE_SHIELD:
		case MT_BALLHOG:
		case MT_SPB:
		case MT_BUBBLESHIELDTRAP:
			K_SteerFromObject(botmo, thing, false, KART_FULLTURN + dodge);
			break;
		case MT_RANDOMITEM:
			if (anglediff > 90)
			{
				break;
			}

			if (P_CanPickupItem(botmo->player, 1))
			{
				K_SteerFromObject(botmo, thing, true, 2*KART_FULLTURN);
			}
			break;
		case MT_FLOATINGITEM:
			if (anglediff > 90)
			{
				break;
			}

			if (P_CanPickupItem(botmo->player, 3))
			{
				K_SteerFromObject(botmo, thing, true, 2*KART_FULLTURN);
			}
			break;
		case MT_RING:
		case MT_FLINGRING:
			if (anglediff > 90)
			{
				break;
			}

			if ((RINGTOTAL(botmo->player) < 20 && !botmo->player->kartstuff[k_ringlock]
				&& P_CanPickupItem(botmo->player, 0))
				&& !thing->extravalue1
				&& (botmo->player->kartstuff[k_itemtype] != KITEM_THUNDERSHIELD))
			{
				K_SteerFromObject(botmo, thing, true, (RINGTOTAL(botmo->player) <= 0 ? 2*KART_FULLTURN : KART_FULLTURN));
			}
			break;
		case MT_PLAYER:
			if (thing->player
				&& !thing->player->kartstuff[k_hyudorotimer]
				&& !botmo->player->kartstuff[k_hyudorotimer])
			{
				// There REALLY ought to be a better way to handle this logic, right?!
				// Squishing
				PlayerAttackSteer(
					botmo->scale > thing->scale + (mapobjectscale/8),
					thing->scale > botmo->scale + (mapobjectscale/8)
				)
				// Invincibility
				else PlayerAttackSteer(
					botmo->player->kartstuff[k_invincibilitytimer],
					thing->player->kartstuff[k_invincibilitytimer]
				)
				// Thunder Shield
				else PlayerAttackSteer(
					botmo->player->kartstuff[k_itemtype] == KITEM_THUNDERSHIELD,
					thing->player->kartstuff[k_itemtype] == KITEM_THUNDERSHIELD
				)
				// Bubble Shield
				else PlayerAttackSteer(
					botmo->player->kartstuff[k_itemtype] == KITEM_BUBBLESHIELD,
					thing->player->kartstuff[k_itemtype] == KITEM_BUBBLESHIELD
				)
				// Flame Shield
				else PlayerAttackSteer(
					botmo->player->kartstuff[k_itemtype] == KITEM_FLAMESHIELD,
					thing->player->kartstuff[k_itemtype] == KITEM_FLAMESHIELD
				)
				// Has held item shield
				else PlayerAttackSteer(
					(botmo->player->kartstuff[k_itemheld] || botmo->player->kartstuff[k_eggmanheld]),
					(thing->player->kartstuff[k_itemheld] || thing->player->kartstuff[k_eggmanheld])
				)
				// Ring Sting
				else PlayerAttackSteer(
					thing->player->kartstuff[k_rings] <= 0,
					botmo->player->kartstuff[k_rings] <= 0
				)
				else
				{
					// After ALL of that, we can do standard bumping
					fixed_t ourweight = K_GetMobjWeight(botmo, thing);
					fixed_t theirweight = K_GetMobjWeight(thing, botmo);
					fixed_t weightdiff = 0;

					if (anglediff > 90)
					{
						weightdiff = theirweight - ourweight;
					}
					else
					{
						weightdiff = ourweight - theirweight;
					}

					if (weightdiff > mapobjectscale)
					{
						K_SteerFromObject(botmo, thing, true, KART_FULLTURN + attack);
					}
					else
					{
						K_SteerFromObject(botmo, thing, false, KART_FULLTURN + dodge);
					}
				}
			}
			break;
		default:
			if (thing->flags & (MF_SOLID|MF_ENEMY|MF_BOSS|MF_PAIN|MF_MISSILE|MF_FIRE))
			{
				K_SteerFromObject(botmo, thing, false, 2*KART_FULLTURN);
			}
			break;
	}

	return true;
}

static INT16 K_BotFindObjects(player_t *player)
{
	INT32 xl, xh, yl, yh, bx, by;

	badsteerglobal = 0;

	botmo = player->mo;
	distancetocheck = (player->mo->radius * 32) + (player->speed * 4);

	xl = (unsigned)(botmo->x - distancetocheck - bmaporgx)>>MAPBLOCKSHIFT;
	xh = (unsigned)(botmo->x + distancetocheck - bmaporgx)>>MAPBLOCKSHIFT;
	yl = (unsigned)(botmo->y - distancetocheck - bmaporgy)>>MAPBLOCKSHIFT;
	yh = (unsigned)(botmo->y + distancetocheck - bmaporgy)>>MAPBLOCKSHIFT;

	BMBOUNDFIX(xl, xh, yl, yh);

	for (bx = xl; bx <= xh; bx++)
	{
		for (by = yl; by <= yh; by++)
		{
			P_BlockThingsIterator(bx, by, K_BotSteerObjects);
		}
	}

	return badsteerglobal;
}

static boolean K_BotUseItemNearPlayer(player_t *player, ticcmd_t *cmd, fixed_t radius)
{
	UINT8 i;

	if (player->pflags & PF_ATTACKDOWN)
	{
		return false;
	}

	for (i = 0; i < MAXPLAYERS; i++)
	{
		player_t *target = NULL;
		fixed_t dist = INT32_MAX;

		if (!playeringame[i])
		{
			continue;
		}

		target = &players[i];

		if (target->mo == NULL || P_MobjWasRemoved(target->mo)
			|| player == target || target->spectator
			|| target->powers[pw_flashing])
		{
			continue;
		}

		dist = P_AproxDistance(P_AproxDistance(
			player->mo->x - target->mo->x,
			player->mo->y - target->mo->y),
			(player->mo->z - target->mo->z) / 4
		);

		if (dist <= radius)
		{
			cmd->buttons |= BT_ATTACK;
			return true;
		}
	}

	return false;
}

static boolean K_PlayerNearSpot(player_t *player, fixed_t x, fixed_t y, fixed_t radius)
{
	UINT8 i;

	for (i = 0; i < MAXPLAYERS; i++)
	{
		player_t *target = NULL;
		fixed_t dist = INT32_MAX;

		if (!playeringame[i])
		{
			continue;
		}

		target = &players[i];

		if (target->mo == NULL || P_MobjWasRemoved(target->mo)
			|| player == target || target->spectator
			|| target->powers[pw_flashing])
		{
			continue;
		}

		dist = P_AproxDistance(
			x - target->mo->x,
			y - target->mo->y
		);

		if (dist <= radius)
		{
			return true;
		}
	}

	return false;
}

static boolean K_BotRevealsBanana(player_t *player, INT16 turnamt, boolean mine)
{
	UINT8 i;

	// Only get out bananas if you have a target

	if (abs(turnamt) >= KART_FULLTURN/2)
	{
		return false;
	}
	else
	{
		UINT32 airtime = FixedDiv((30 * player->mo->scale) + player->mo->momz, gravity);
		fixed_t throwspeed = FixedMul(82 * mapobjectscale, K_GetKartGameSpeedScalar(gamespeed));
		fixed_t estx = player->mo->x + P_ReturnThrustX(NULL, player->mo->angle, (throwspeed + player->speed) * airtime);
		fixed_t esty = player->mo->y + P_ReturnThrustY(NULL, player->mo->angle, (throwspeed + player->speed) * airtime);

		if (K_PlayerNearSpot(player, estx, esty, player->mo->radius * 16))
		{
			return true;
		}

		if (mine)
		{
			airtime = FixedDiv((40 * player->mo->scale) + player->mo->momz, gravity);
			throwspeed = FixedMul(82 * mapobjectscale, K_GetKartGameSpeedScalar(gamespeed)) * 2;
			estx = player->mo->x + P_ReturnThrustX(NULL, player->mo->angle, (throwspeed + player->speed) * airtime);
			esty = player->mo->y + P_ReturnThrustY(NULL, player->mo->angle, (throwspeed + player->speed) * airtime);

			if (K_PlayerNearSpot(player, estx, esty, player->mo->radius * 16))
			{
				return true;
			}
		}
	}

	for (i = 0; i < MAXPLAYERS; i++)
	{
		player_t *target = NULL;
		fixed_t dist = INT32_MAX;

		if (!playeringame[i])
		{
			continue;
		}

		target = &players[i];

		if (target->mo == NULL || P_MobjWasRemoved(target->mo)
			|| player == target || target->spectator
			|| target->powers[pw_flashing]
			|| !P_CheckSight(player->mo, target->mo))
		{
			continue;
		}

		dist = P_AproxDistance(P_AproxDistance(
			player->mo->x - target->mo->x,
			player->mo->y - target->mo->y),
			(player->mo->z - target->mo->z) / 4
		);

		if (dist <= (player->mo->radius * 16))
		{
			angle_t a = player->mo->angle - R_PointToAngle2(player->mo->x, player->mo->y, target->mo->x, target->mo->y);
			INT16 ad = 0;
			const INT16 cone = 10;

			if (a < ANGLE_180)
			{
				ad = AngleFixed(a)>>FRACBITS;
			}
			else 
			{
				ad = 360-(AngleFixed(a)>>FRACBITS);
			}

			ad = abs(ad);

			if (ad >= 180-cone)
			{
				return true;
			}
		}
	}

	return false;
}

void K_BuildBotTiccmd(player_t *player, ticcmd_t *cmd)
{
	botprediction_t *predict = NULL;
	INT32 turnamt = 0;

	// Can't build a ticcmd if we aren't spawned...
	if (!player->mo)
		return;

	// Remove any existing controls
	memset(cmd, 0, sizeof(ticcmd_t));
	cmd->angleturn = (player->mo->angle >> 16) | TICCMD_RECEIVED;

	if (player->playerstate == PST_DEAD)
	{
		cmd->buttons |= BT_ACCELERATE;
		return;
	}

#ifdef HAVE_BLUA
	// Complete override of all ticcmd functionality
	if (LUAh_BotTiccmd(player, cmd))
		return;
#endif

	// Start boost handler
	if (leveltime <= starttime)
	{
		if (leveltime >= starttime-35)
			cmd->buttons |= BT_ACCELERATE;
		return;
	}

	if (player->nextwaypoint != NULL && player->nextwaypoint->mobj != NULL && !P_MobjWasRemoved(player->nextwaypoint->mobj))
	{
		SINT8 turnsign = 0;
		angle_t destangle, moveangle, angle;
		INT16 anglediff;

		predict = K_CreateBotPrediction(player);

		destangle = R_PointToAngle2(player->mo->x, player->mo->y, predict->x, predict->y);
		moveangle = player->mo->angle;

		angle = (moveangle - destangle);

		if (angle < ANGLE_180)
		{
			turnsign = -1; // Turn right
			anglediff = AngleFixed(angle)>>FRACBITS;
		}
		else 
		{
			turnsign = 1; // Turn left
			anglediff = 360-(AngleFixed(angle)>>FRACBITS);
		}

		anglediff = abs(anglediff);
		turnamt = KART_FULLTURN * turnsign;

		if (anglediff > 90)
		{
			// Wrong way!
			cmd->forwardmove = -25;
			cmd->buttons |= BT_BRAKE;
		}
		else
		{
			const fixed_t realrad = predict->radius - player->mo->radius;

			INT16 objectsteer = 0;

			fixed_t rad = realrad;
			fixed_t dirdist = K_DistanceOfLineFromPoint(
				player->mo->x, player->mo->y,
				player->mo->x + FINECOSINE(moveangle >> ANGLETOFINESHIFT), player->mo->y + FINESINE(moveangle >> ANGLETOFINESHIFT),
				predict->x, predict->y
			);

			if (anglediff > 0)
			{
				// Become more precise based on how hard you need to turn
				// This makes predictions into turns a little nicer
				// Facing 90 degrees away from the predicted point gives you a 1/3 radius
				rad = FixedMul(rad, ((135 - anglediff) * FRACUNIT) / 135);
			}

			if (rad > realrad)
			{
				rad = realrad;
			}
			else if (rad < 2*player->mo->radius)
			{
				rad = 2*player->mo->radius;
			}

			cmd->buttons |= BT_ACCELERATE;

			// Full speed ahead!
			cmd->forwardmove = 50;

			if (anglediff > 60)
			{
				// Actually, don't go too fast...
				cmd->forwardmove /= 2;
				cmd->buttons |= BT_BRAKE;
			}
			else if (anglediff <= 23 || dirdist <= realrad)
			{
				objectsteer = K_BotFindObjects(player);
			}

			if (dirdist <= rad)
			{
				fixed_t speedmul = FixedMul(player->speed, K_GetKartSpeed(player, false));
				fixed_t speedrad = rad/4;

				if (speedmul > FRACUNIT)
				{
					speedmul = FRACUNIT;
				}

				// Increase radius with speed
				// At low speed, the CPU will try to be more accurate
				// At high speed, they're more likely to lawnmower
				speedrad += FixedMul(speedmul, (3*rad/4) - speedrad);

				if (speedrad < 2*player->mo->radius)
				{
					speedrad = 2*player->mo->radius;
				}

				if (dirdist <= speedrad)
				{
					// Don't turn at all
					turnamt = 0;
				}
				else
				{
					// Make minor adjustments
					turnamt /= 4;
				}
			}

			if (objectsteer != 0)
			{
				turnamt += objectsteer;
			}
		}
	}

	if (player->kartstuff[k_userings] == 1)
	{
		if (!player->exiting)
		{
			INT32 saferingsval = 16 - K_GetKartRingPower(player);

			if (player->speed < K_GetKartSpeed(player, false)/2 // Being slowed down too much
				|| player->kartstuff[k_speedboost] > (FRACUNIT/5)) // Have another type of boost (tethering)
			{
				saferingsval -= 5;
			}

			if (player->kartstuff[k_rings] > saferingsval)
			{
				cmd->buttons |= BT_ATTACK;
			}
		}
	}
	else
	{
		if (player->botvars.itemdelay)
		{
			player->botvars.itemdelay--;
			player->botvars.itemconfirm = 0;
		}
		else if (player->kartstuff[k_stealingtimer] == 0 && player->kartstuff[k_stolentimer] == 0)
		{
			if (player->kartstuff[k_eggmanexplode])
			{
				K_BotUseItemNearPlayer(player, cmd, 128*player->mo->scale);
			}
			else if (player->kartstuff[k_rocketsneakertimer] > 0)
			{
				if (player->botvars.itemconfirm > TICRATE)
				{
					if (!player->kartstuff[k_sneakertimer] && !(player->pflags & PF_ATTACKDOWN))
					{
						cmd->buttons |= BT_ATTACK;
						player->botvars.itemconfirm = 0;
					}
				}
				else
				{
					player->botvars.itemconfirm++;
				}
			}
			else
			{
				switch (player->kartstuff[k_itemtype])
				{
					case KITEM_INVINCIBILITY:
					case KITEM_SPB:
					case KITEM_GROW:
					case KITEM_SHRINK:
					case KITEM_HYUDORO:
					case KITEM_SUPERRING:
						if (!(player->pflags & PF_ATTACKDOWN))
						{
							cmd->buttons |= BT_ATTACK;
							player->botvars.itemconfirm = 0;
						}
						break;
					case KITEM_SNEAKER:
						if ((player->kartstuff[k_offroad] && K_ApplyOffroad(player)) // Stuck in offroad, use it NOW
							|| K_GetWaypointIsShortcut(player->nextwaypoint) == true // Going toward a shortcut!
							|| player->speed < K_GetKartSpeed(player, false)/2 // Being slowed down too much
							|| player->kartstuff[k_speedboost] > (FRACUNIT/8) // Have another type of boost (tethering)
							|| player->botvars.itemconfirm > 4*TICRATE) // Held onto it for too long
						{
							if (!player->kartstuff[k_sneakertimer] && !(player->pflags & PF_ATTACKDOWN))
							{
								cmd->buttons |= BT_ATTACK;
								player->botvars.itemconfirm = 2*TICRATE;
							}
						}
						else
						{
							player->botvars.itemconfirm++;
						}
						break;
					case KITEM_ROCKETSNEAKER:
						if (player->kartstuff[k_rocketsneakertimer] <= 0 && !(player->pflags & PF_ATTACKDOWN))
						{
							cmd->buttons |= BT_ATTACK;
							player->botvars.itemconfirm = 0;
						}
						break;
					case KITEM_BANANA:
						if (!player->kartstuff[k_itemheld])
						{
							if ((K_BotRevealsBanana(player, turnamt, false) || (player->botvars.itemconfirm++ > 5*TICRATE))
								&& !(player->pflags & PF_ATTACKDOWN))
							{
								cmd->buttons |= BT_ATTACK;
								player->botvars.itemconfirm = 0;
							}
						}
						else
						{
							SINT8 throwdir = -1;
							UINT8 i;

							player->botvars.itemconfirm++;

							if (abs(turnamt) >= KART_FULLTURN/2)
							{
								player->botvars.itemconfirm += 2;
							}
							else
							{
								const UINT32 airtime = FixedDiv((30 * player->mo->scale) + player->mo->momz, gravity);
								const fixed_t throwspeed = FixedMul(82 * mapobjectscale, K_GetKartGameSpeedScalar(gamespeed));
								const fixed_t estx = player->mo->x + P_ReturnThrustX(NULL, player->mo->angle, (throwspeed + player->speed) * airtime);
								const fixed_t esty = player->mo->y + P_ReturnThrustY(NULL, player->mo->angle, (throwspeed + player->speed) * airtime);

								if (K_PlayerNearSpot(player, estx, esty, player->mo->radius * 16))
								{
									player->botvars.itemconfirm += 8;
									throwdir = 1;
								}
							}

							for (i = 0; i < MAXPLAYERS; i++)
							{
								player_t *target = NULL;
								fixed_t dist = INT32_MAX;

								if (!playeringame[i])
								{
									continue;
								}

								target = &players[i];

								if (target->mo == NULL || P_MobjWasRemoved(target->mo)
									|| player == target || target->spectator
									|| target->powers[pw_flashing]
									|| !P_CheckSight(player->mo, target->mo))
								{
									continue;
								}

								dist = P_AproxDistance(P_AproxDistance(
									player->mo->x - target->mo->x,
									player->mo->y - target->mo->y),
									(player->mo->z - target->mo->z) / 4
								);

								if (dist <= (player->mo->radius * 16))
								{
									angle_t a = player->mo->angle - R_PointToAngle2(player->mo->x, player->mo->y, target->mo->x, target->mo->y);
									INT16 ad = 0;
									const INT16 cone = 10;

									if (a < ANGLE_180)
									{
										ad = AngleFixed(a)>>FRACBITS;
									}
									else 
									{
										ad = 360-(AngleFixed(a)>>FRACBITS);
									}

									ad = abs(ad);

									if (ad >= 180-cone)
									{
										player->botvars.itemconfirm += 4;
										throwdir = -1;
									}
								}
							}

							if ((player->botvars.itemconfirm > 2*TICRATE || player->kartstuff[k_bananadrag] >= TICRATE)
								&& !(player->pflags & PF_ATTACKDOWN))
							{
								if (throwdir == 1)
								{
									cmd->buttons |= BT_FORWARD;
								}
								else if (throwdir == -1)
								{
									cmd->buttons |= BT_BACKWARD;
								}

								cmd->buttons |= BT_ATTACK;
								player->botvars.itemconfirm = 0;
							}
						}
						break;
					case KITEM_ORBINAUT:
						if (!player->kartstuff[k_itemheld] && !(player->pflags & PF_ATTACKDOWN))
						{
							cmd->buttons |= BT_ATTACK;
							player->botvars.itemconfirm = 0;
						}
						else if (player->kartstuff[k_position] != 1) // Hold onto orbiting items when in 1st :)
						/* FALL-THRU */
					case KITEM_BALLHOG:
						{
							const fixed_t topspeed = K_GetKartSpeed(player, false);
							fixed_t radius = (player->mo->radius * 32);
							SINT8 throwdir = -1;
							UINT8 i;

							if (player->speed > topspeed)
							{
								radius = FixedMul(radius, FixedDiv(player->speed, topspeed));
							}

							for (i = 0; i < MAXPLAYERS; i++)
							{
								player_t *target = NULL;
								fixed_t dist = INT32_MAX;

								if (!playeringame[i])
								{
									continue;
								}

								target = &players[i];

								if (target->mo == NULL || P_MobjWasRemoved(target->mo)
									|| player == target || target->spectator
									|| target->powers[pw_flashing]
									|| !P_CheckSight(player->mo, target->mo))
								{
									continue;
								}

								dist = P_AproxDistance(P_AproxDistance(
									player->mo->x - target->mo->x,
									player->mo->y - target->mo->y),
									(player->mo->z - target->mo->z) / 4
								);

								if (dist <= radius)
								{
									angle_t a = player->mo->angle - R_PointToAngle2(player->mo->x, player->mo->y, target->mo->x, target->mo->y);
									INT16 ad = 0;
									const INT16 cone = 10;

									if (a < ANGLE_180)
									{
										ad = AngleFixed(a)>>FRACBITS;
									}
									else 
									{
										ad = 360-(AngleFixed(a)>>FRACBITS);
									}

									ad = abs(ad);

									if (ad <= cone)
									{
										player->botvars.itemconfirm += 8;
										throwdir = 1;
									}
									else if (ad >= 180-cone)
									{
										player->botvars.itemconfirm += 4;
									}
								}
							}

							if ((player->botvars.itemconfirm > 5*TICRATE)
								&& !(player->pflags & PF_ATTACKDOWN))
							{
								if (throwdir == 1)
								{
									cmd->buttons |= BT_FORWARD;
								}
								else if (throwdir == -1)
								{
									cmd->buttons |= BT_BACKWARD;
								}

								cmd->buttons |= BT_ATTACK;
								player->botvars.itemconfirm = 0;
							}
						}
						break;
					case KITEM_JAWZ:
						if (!player->kartstuff[k_itemheld] && !(player->pflags & PF_ATTACKDOWN))
						{
							cmd->buttons |= BT_ATTACK;
							player->botvars.itemconfirm = 0;
						}
						else if (player->kartstuff[k_position] != 1) // Hold onto orbiting items when in 1st :)
						{
							SINT8 throwdir = 1;
							UINT8 i;

							player->botvars.itemconfirm++;

							for (i = 0; i < MAXPLAYERS; i++)
							{
								player_t *target = NULL;
								fixed_t dist = INT32_MAX;

								if (!playeringame[i])
								{
									continue;
								}

								target = &players[i];

								if (target->mo == NULL || P_MobjWasRemoved(target->mo)
									|| player == target || target->spectator
									|| target->powers[pw_flashing]
									|| !P_CheckSight(player->mo, target->mo))
								{
									continue;
								}

								dist = P_AproxDistance(P_AproxDistance(
									player->mo->x - target->mo->x,
									player->mo->y - target->mo->y),
									(player->mo->z - target->mo->z) / 4
								);

								if (dist <= (player->mo->radius * 32))
								{
									angle_t a = player->mo->angle - R_PointToAngle2(player->mo->x, player->mo->y, target->mo->x, target->mo->y);
									INT16 ad = 0;
									const INT16 cone = 10;

									if (a < ANGLE_180)
									{
										ad = AngleFixed(a)>>FRACBITS;
									}
									else 
									{
										ad = 360-(AngleFixed(a)>>FRACBITS);
									}

									ad = abs(ad);

									if (ad >= 180-cone)
									{
										player->botvars.itemconfirm += 4;
										throwdir = -1;
									}
								}
							}

							if (player->kartstuff[k_lastjawztarget] != -1)
							{
								player->botvars.itemconfirm += 8;
								throwdir = 1;
							}

							if ((player->botvars.itemconfirm > 5*TICRATE)
								&& !(player->pflags & PF_ATTACKDOWN))
							{
								if (throwdir == 1)
								{
									cmd->buttons |= BT_FORWARD;
								}
								else if (throwdir == -1)
								{
									cmd->buttons |= BT_BACKWARD;
								}

								cmd->buttons |= BT_ATTACK;
								player->botvars.itemconfirm = 0;
							}
						}
						break;
					case KITEM_MINE:
						if (!player->kartstuff[k_itemheld])
						{
							if ((K_BotRevealsBanana(player, turnamt, true) || (player->botvars.itemconfirm++ > 5*TICRATE))
								&& !(player->pflags & PF_ATTACKDOWN))
							{
								cmd->buttons |= BT_ATTACK;
								player->botvars.itemconfirm = 0;
							}
						}
						else
						{
							SINT8 throwdir = 0;
							UINT8 i;

							player->botvars.itemconfirm++;

							for (i = 0; i < MAXPLAYERS; i++)
							{
								player_t *target = NULL;
								fixed_t dist = INT32_MAX;

								if (!playeringame[i])
								{
									continue;
								}

								target = &players[i];

								if (target->mo == NULL || P_MobjWasRemoved(target->mo)
									|| player == target || target->spectator
									|| target->powers[pw_flashing]
									|| !P_CheckSight(player->mo, target->mo))
								{
									continue;
								}

								dist = P_AproxDistance(P_AproxDistance(
									player->mo->x - target->mo->x,
									player->mo->y - target->mo->y),
									(player->mo->z - target->mo->z) / 4
								);

								if (dist <= (player->mo->radius * 16))
								{
									angle_t a = player->mo->angle - R_PointToAngle2(player->mo->x, player->mo->y, target->mo->x, target->mo->y);
									INT16 ad = 0;
									const INT16 cone = 10;

									if (a < ANGLE_180)
									{
										ad = AngleFixed(a)>>FRACBITS;
									}
									else 
									{
										ad = 360-(AngleFixed(a)>>FRACBITS);
									}

									ad = abs(ad);

									if (ad >= 180-cone)
									{
										player->botvars.itemconfirm += 4;
										throwdir = -1;
									}
								}
							}

							if (abs(turnamt) >= KART_FULLTURN/2)
							{
								player->botvars.itemconfirm += 2;
								throwdir = -1;
							}
							else
							{
								UINT32 airtime = FixedDiv((30 * player->mo->scale) + player->mo->momz, gravity);
								fixed_t throwspeed = FixedMul(82 * mapobjectscale, K_GetKartGameSpeedScalar(gamespeed));
								fixed_t estx = player->mo->x + P_ReturnThrustX(NULL, player->mo->angle, (throwspeed + player->speed) * airtime);
								fixed_t esty = player->mo->y + P_ReturnThrustY(NULL, player->mo->angle, (throwspeed + player->speed) * airtime);

								if (K_PlayerNearSpot(player, estx, esty, player->mo->radius * 16))
								{
									player->botvars.itemconfirm += 8;
									throwdir = 0;
								}

								airtime = FixedDiv((40 * player->mo->scale) + player->mo->momz, gravity);
								throwspeed = FixedMul(82 * mapobjectscale, K_GetKartGameSpeedScalar(gamespeed)) * 2;
								estx = player->mo->x + P_ReturnThrustX(NULL, player->mo->angle, (throwspeed + player->speed) * airtime);
								esty = player->mo->y + P_ReturnThrustY(NULL, player->mo->angle, (throwspeed + player->speed) * airtime);

								if (K_PlayerNearSpot(player, estx, esty, player->mo->radius * 16))
								{
									player->botvars.itemconfirm += 4;
									throwdir = 1;
								}
							}

							if (((player->botvars.itemconfirm > 2*TICRATE)
								|| (player->kartstuff[k_bananadrag] >= TICRATE))
								&& !(player->pflags & PF_ATTACKDOWN))
							{
								if (throwdir == 1)
								{
									cmd->buttons |= BT_FORWARD;
								}
								else if (throwdir == -1)
								{
									cmd->buttons |= BT_BACKWARD;
								}

								cmd->buttons |= BT_ATTACK;
								player->botvars.itemconfirm = 0;
							}
						}
						break;
					case KITEM_THUNDERSHIELD:
						if (!K_BotUseItemNearPlayer(player, cmd, 192*player->mo->scale))
						{
							if (player->botvars.itemconfirm > 10*TICRATE && !(player->pflags & PF_ATTACKDOWN))
							{
								cmd->buttons |= BT_ATTACK;
								player->botvars.itemconfirm = 0;
							}
							else
							{
								player->botvars.itemconfirm++;
							}
						}
						break;
					default:
						player->botvars.itemconfirm = 0;
						break;
				}
			}
		}
	}

	if (turnamt != 0)
	{
		const INT16 minturn = KART_FULLTURN/2;

		if (turnamt > KART_FULLTURN)
		{
			turnamt = KART_FULLTURN;
		}
		else if (turnamt < -KART_FULLTURN)
		{
			turnamt = -KART_FULLTURN;
		}

		if ((turnamt > minturn && player->botvars.lastturn >= 0)
			|| (turnamt < -minturn && player->botvars.lastturn <= 0))
		{
			if (turnamt > 0)
			{
				player->botvars.lastturn = 1;
			}
			else if (turnamt < 0)
			{
				player->botvars.lastturn = -1;
			}

			cmd->driftturn = turnamt;
			cmd->angleturn += K_GetKartTurnValue(player, turnamt);
		}
		else
		{
			// Can reset turn dir
			player->botvars.lastturn = 0;
		}
	}

	if (predict != NULL)
	{
		Z_Free(predict);
	}
}

