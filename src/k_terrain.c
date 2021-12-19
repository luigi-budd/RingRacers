// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 1998-2021 by ZDoom + GZDoom teams, and contributors
// Copyright (C) 2021 by Sally "TehRealSalt" Cochenour
// Copyright (C) 2021 by Kart Krew
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  k_terrain.c
/// \brief Implementation of TERRAIN lump from GZDoom codebase for DRRR.

#include "k_terrain.h"

#include "dehacked.h" // get_number
#include "deh_soc.h" // get_mobjtype
#include "doomdata.h"
#include "doomdef.h"
#include "doomtype.h"
#include "fastcmp.h"
#include "m_fixed.h"
#include "m_random.h"
#include "p_local.h"
#include "p_mobj.h"
#include "r_textures.h"
#include "w_wad.h"
#include "z_zone.h"

#include "k_kart.h" // on the chopping block...

static t_splash_t *splashDefs = NULL;
static size_t numSplashDefs = 0;

static t_footstep_t *footstepDefs = NULL;
static size_t numFootstepDefs = 0;

static terrain_t *terrainDefs = NULL;
static size_t numTerrainDefs = 0;

static t_floor_t *terrainFloorDefs = NULL;
static size_t numTerrainFloorDefs = 0;

static size_t defaultTerrain = SIZE_MAX;
static size_t defaultOffroadFootstep = SIZE_MAX;

/*--------------------------------------------------
	size_t K_GetSplashHeapIndex(t_splash_t *splash)

		See header file for description.
--------------------------------------------------*/
size_t K_GetSplashHeapIndex(t_splash_t *splash)
{
	if (splash == NULL)
	{
		return SIZE_MAX;
	}

	return (splash - splashDefs);
}

/*--------------------------------------------------
	size_t K_GetNumSplashDefs(void)

		See header file for description.
--------------------------------------------------*/
size_t K_GetNumSplashDefs(void)
{
	return numSplashDefs;
}

/*--------------------------------------------------
	t_splash_t *K_GetSplashByIndex(size_t checkIndex)

		See header file for description.
--------------------------------------------------*/
t_splash_t *K_GetSplashByIndex(size_t checkIndex)
{
	if (checkIndex >= numSplashDefs)
	{
		return NULL;
	}

	return &splashDefs[checkIndex];
}

/*--------------------------------------------------
	t_splash_t *K_GetSplashByName(const char *checkName)

		See header file for description.
--------------------------------------------------*/
t_splash_t *K_GetSplashByName(const char *checkName)
{
	size_t i;

	if (numSplashDefs == 0)
	{
		return NULL;
	}

	for (i = 0; i < numSplashDefs; i++)
	{
		t_splash_t *s = &splashDefs[i];

		if (stricmp(checkName, s->name) == 0)
		{
			// Name matches.
			return s;
		}
	}

	return NULL;
}

/*--------------------------------------------------
	size_t K_GetFootstepHeapIndex(t_footstep_t *footstep)

		See header file for description.
--------------------------------------------------*/
size_t K_GetFootstepHeapIndex(t_footstep_t *footstep)
{
	if (footstep == NULL)
	{
		return SIZE_MAX;
	}

	return (footstep - footstepDefs);
}

/*--------------------------------------------------
	size_t K_GetNumFootstepDefs(void)

		See header file for description.
--------------------------------------------------*/
size_t K_GetNumFootstepDefs(void)
{
	return numFootstepDefs;
}

/*--------------------------------------------------
	t_footstep_t *K_GetFootstepByIndex(size_t checkIndex)

		See header file for description.
--------------------------------------------------*/
t_footstep_t *K_GetFootstepByIndex(size_t checkIndex)
{
	if (checkIndex >= numFootstepDefs)
	{
		return NULL;
	}

	return &footstepDefs[checkIndex];
}

/*--------------------------------------------------
	t_footstep_t *K_GetFootstepByName(const char *checkName)

		See header file for description.
--------------------------------------------------*/
t_footstep_t *K_GetFootstepByName(const char *checkName)
{
	size_t i;

	if (numFootstepDefs == 0)
	{
		return NULL;
	}

	for (i = 0; i < numFootstepDefs; i++)
	{
		t_footstep_t *fs = &footstepDefs[i];

		if (stricmp(checkName, fs->name) == 0)
		{
			// Name matches.
			return fs;
		}
	}

	return NULL;
}

/*--------------------------------------------------
	size_t K_GetTerrainHeapIndex(terrain_t *terrain)

		See header file for description.
--------------------------------------------------*/
size_t K_GetTerrainHeapIndex(terrain_t *terrain)
{
	if (terrain == NULL)
	{
		return SIZE_MAX;
	}

	return (terrain - terrainDefs);
}

/*--------------------------------------------------
	size_t K_GetNumTerrainDefs(void)

		See header file for description.
--------------------------------------------------*/
size_t K_GetNumTerrainDefs(void)
{
	return numTerrainDefs;
}

/*--------------------------------------------------
	terrain_t *K_GetTerrainByIndex(size_t checkIndex)

		See header file for description.
--------------------------------------------------*/
terrain_t *K_GetTerrainByIndex(size_t checkIndex)
{
	if (checkIndex >= numTerrainDefs)
	{
		return NULL;
	}

	return &terrainDefs[checkIndex];
}

/*--------------------------------------------------
	terrain_t *K_GetTerrainByName(const char *checkName)

		See header file for description.
--------------------------------------------------*/
terrain_t *K_GetTerrainByName(const char *checkName)
{
	size_t i;

	if (numTerrainDefs == 0)
	{
		return NULL;
	}

	for (i = 0; i < numTerrainDefs; i++)
	{
		terrain_t *t = &terrainDefs[i];

		if (stricmp(checkName, t->name) == 0)
		{
			// Name matches.
			return t;
		}
	}

	return NULL;
}

/*--------------------------------------------------
	terrain_t *K_GetDefaultTerrain(void)

		See header file for description.
--------------------------------------------------*/
terrain_t *K_GetDefaultTerrain(void)
{
	return K_GetTerrainByIndex(defaultTerrain);
}

/*--------------------------------------------------
	terrain_t *K_GetTerrainForTextureName(const char *checkName)

		See header file for description.
--------------------------------------------------*/
terrain_t *K_GetTerrainForTextureName(const char *checkName)
{
	size_t i;

	if (numTerrainFloorDefs == 0)
	{
		return NULL;
	}

	for (i = 0; i < numTerrainFloorDefs; i++)
	{
		t_floor_t *f = &terrainFloorDefs[i];

		if (strncasecmp(checkName, f->textureName, 8) == 0)
		{
			return K_GetTerrainByIndex(f->terrainID);
		}
	}

	// This texture doesn't have a terrain directly applied to it,
	// so we fallback to the default terrain.
	return K_GetDefaultTerrain();
}

/*--------------------------------------------------
	terrain_t *K_GetTerrainForTextureNum(INT32 textureNum)

		See header file for description.
--------------------------------------------------*/
terrain_t *K_GetTerrainForTextureNum(INT32 textureNum)
{
	texture_t *tex = NULL;

	if (textureNum < 0 || textureNum >= numtextures)
	{
		return NULL;
	}

	tex = textures[textureNum];
	return K_GetTerrainForTextureName(tex->name);
}

/*--------------------------------------------------
	terrain_t *K_GetTerrainForFlatNum(INT32 flatID)

		See header file for description.
--------------------------------------------------*/
terrain_t *K_GetTerrainForFlatNum(INT32 flatID)
{
	levelflat_t *levelFlat = NULL;

	if (flatID < 0 || flatID >= (signed)numlevelflats)
	{
		// Clearly invalid floor...
		return NULL;
	}

	levelFlat = &levelflats[flatID];
	return K_GetTerrainForTextureName(levelFlat->name);
}

/*--------------------------------------------------
	void K_UpdateMobjTerrain(mobj_t *mo, INT32 flatID)

		See header file for description.
--------------------------------------------------*/
void K_UpdateMobjTerrain(mobj_t *mo, INT32 flatID)
{
	if (mo == NULL || P_MobjWasRemoved(mo) == true)
	{
		// Invalid object.
		return;
	}

	if (mo->flags & MF_NOCLIPHEIGHT)
	{
		// You can't collide with floors anyway!
		mo->terrain = NULL;
		return;
	}

	// Update the object's terrain pointer.
	mo->terrain = K_GetTerrainForFlatNum(flatID);
}

/*--------------------------------------------------
	void K_ProcessTerrainEffect(mobj_t *mo)

		See header file for description.
--------------------------------------------------*/
void K_ProcessTerrainEffect(mobj_t *mo)
{
	player_t *player = NULL;
	terrain_t *terrain = NULL;

	if (mo == NULL || P_MobjWasRemoved(mo) == true)
	{
		// Invalid object.
		return;
	}

	if (mo->terrain == NULL)
	{
		// No terrain type.
		return;
	}

	terrain = mo->terrain;
	player = mo->player;

	if (player == NULL)
	{
		// maybe can support regualar mobjs later? :)
		return;
	}

	// Damage effects
	if (terrain->damageType > 0)
	{
		UINT8 dmg = (terrain->damageType & 0xFF);
		P_DamageMobj(mo, NULL, NULL, 1, dmg);
	}

	// Sneaker panel
	if (terrain->flags & TRF_SNEAKERPANEL)
	{
		if (player->floorboost == 0)
			player->floorboost = 3;
		else
			player->floorboost = 2;

		K_DoSneaker(player, 0);
	}

	// Trick panel
	if (terrain->trickPanel > 0 && !(mo->eflags & MFE_SPRUNG))
	{
		const fixed_t hscale = mapobjectscale + (mapobjectscale - mo->scale);
		const fixed_t minspeed = 24*hscale;
		fixed_t speed = FixedHypot(mo->momx, mo->momy);
		fixed_t upwards = 16 * FRACUNIT * terrain->trickPanel;

		player->trickpanel = 1;
		player->pflags |= PF_TRICKDELAY;
		K_DoPogoSpring(mo, upwards, 1);

		if (speed < minspeed)
		{
			speed = minspeed;
		}

		P_InstaThrust(mo, mo->angle, speed);
	}

	// Bumpy floor
	if (terrain->flags & TRF_STAIRJANK)
	{
		/* use a shorter sound if not two tics have passed
		 * since the last step */
		S_StartSound(mo, player->stairjank
				>= 16 ?  sfx_s23b : sfx_s268);

		if (player->stairjank == 0)
		{
			mobj_t *spark = P_SpawnMobjFromMobj(mo,
					0, 0, 0, MT_JANKSPARK);
			spark->fuse = 9;
			spark->cusval = K_StairJankFlip(ANGLE_90);
			P_SetTarget(&spark->target, mo);
		}

		player->stairjank = 17;
	}

	// (Offroad is handled elsewhere!)
}

/*--------------------------------------------------
	void K_SetDefaultFriction(mobj_t *mo)

		See header file for description.
--------------------------------------------------*/
void K_SetDefaultFriction(mobj_t *mo)
{
	boolean isPlayer = false;

	if (mo == NULL || P_MobjWasRemoved(mo) == true)
	{
		// Invalid object.
		return;
	}

	isPlayer = (mo->player != NULL);

	mo->friction = ORIG_FRICTION;

	if (isPlayer == true)
	{
		mo->movefactor = FRACUNIT;
	}

	if (mo->terrain != NULL)
	{
		fixed_t strength = mo->terrain->friction;

		fixed_t newFriction = INT32_MAX;
		fixed_t newMovefactor = INT32_MAX;

		if (strength > 0) // sludge
		{
			strength = strength * 2; // otherwise, the maximum sludginess value is +967...
		}

		// The following might seem odd. At the time of movement,
		// the move distance is multiplied by 'friction/0x10000', so a
		// higher friction value actually means 'less friction'.
		newFriction = ORIG_FRICTION - FixedMul(0x1EB8, strength) / 0x80; // ORIG_FRICTION is 0xE800

		if (newFriction > FRACUNIT)
		{
			newFriction = FRACUNIT;
		}

		if (newFriction < 0)
		{
			newFriction = 0;
		}

		mo->friction = newFriction;

		if (isPlayer == true)
		{
			newMovefactor = FixedDiv(ORIG_FRICTION, newFriction);

			if (newMovefactor < FRACUNIT)
			{
				newMovefactor = 19*newMovefactor - 18*FRACUNIT;
			}
			else
			{
				newMovefactor = FRACUNIT;
			}

			mo->movefactor = newMovefactor;
		}
	}
}

/*--------------------------------------------------
	static void K_SpawnSplashParticles(mobj_t *mo, t_splash_t *s, fixed_t impact)

		See header file for description.
--------------------------------------------------*/
static void K_SpawnSplashParticles(mobj_t *mo, t_splash_t *s, fixed_t impact)
{
	const UINT8 numParticles = s->numParticles;
	const angle_t particleSpread = ANGLE_MAX / numParticles;
	size_t i;

	for (i = 0; i < numParticles; i++)
	{
		mobj_t *dust = NULL;
		angle_t pushAngle = (particleSpread * i);
		fixed_t momH = INT32_MAX;
		fixed_t momV = INT32_MAX;

		if (numParticles == 1)
		{
			// Random angle.
			pushAngle = P_RandomRange(0, ANGLE_MAX);
		}

		dust = P_SpawnMobjFromMobj(
			mo,
			(12 * FINECOSINE(pushAngle >> ANGLETOFINESHIFT)),
			(12 * FINESINE(pushAngle >> ANGLETOFINESHIFT)),
			0, s->mobjType
		);

		P_SetTarget(&dust->target, mo);
		dust->angle = pushAngle;

		dust->destscale = FixedMul(mo->scale, s->scale);
		P_SetScale(dust, dust->destscale);

		dust->momx = mo->momx / 2;
		dust->momy = mo->momy / 2;
		dust->momz = 0;

		momH = FixedMul(impact, s->pushH);
		momV = FixedMul(impact, s->pushV);

		dust->momx += FixedMul(momH, FINECOSINE(pushAngle >> ANGLETOFINESHIFT));
		dust->momy += FixedMul(momH, FINESINE(pushAngle >> ANGLETOFINESHIFT));
		dust->momz += momV * P_MobjFlip(mo);

		if (s->color != SKINCOLOR_NONE)
		{
			dust->color = s->color;
		}

		if (s->sfx != sfx_None)
		{
			S_StartSound(mo, s->sfx);
		}
	}
}

/*--------------------------------------------------
	void K_SpawnSplashForMobj(mobj_t *mo, fixed_t impact)

		See header file for description.
--------------------------------------------------*/
void K_SpawnSplashForMobj(mobj_t *mo, fixed_t impact)
{
	t_splash_t *s = NULL;

	if (mo == NULL || P_MobjWasRemoved(mo) == true)
	{
		// Invalid object.
		return;
	}

	if (mo->terrain == NULL || mo->terrain->splashID == SIZE_MAX)
	{
		// No impact for this terrain type.
		return;
	}
	else
	{
		s = K_GetSplashByIndex(mo->terrain->splashID);
	}

	if (s == NULL || s->mobjType == MT_NULL || s->numParticles == 0)
	{
		// No particles to spawn.
		return;
	}

	// Idea for later: if different spawning styles are desired,
	// we can put a switch case here!
	K_SpawnSplashParticles(mo, s, impact);
}

/*--------------------------------------------------
	static void K_SpawnFootstepParticle(mobj_t *mo, t_footstep_t *fs)

		See header file for description.
--------------------------------------------------*/
static void K_SpawnFootstepParticle(mobj_t *mo, t_footstep_t *fs)
{
	mobj_t *dust = NULL;
	angle_t pushAngle = ANGLE_MAX;
	angle_t tireAngle = ANGLE_MAX;
	fixed_t momentum = INT32_MAX;
	fixed_t momH = INT32_MAX;
	fixed_t momV = INT32_MAX;

	if (mo->player != NULL)
	{
		tireAngle = (mo->player->drawangle + ANGLE_180);
	}
	else
	{
		tireAngle = (mo->angle + ANGLE_180);
	}

	if ((leveltime / 2) & 1)
	{
		tireAngle -= ANGLE_45;
		tireAngle -= P_RandomRange(0, ANGLE_11hh);
	}
	else
	{
		tireAngle += ANGLE_45;
		tireAngle += P_RandomRange(0, ANGLE_11hh);
	}

	pushAngle = K_MomentumAngle(mo) + ANGLE_180;

	dust = P_SpawnMobjFromMobj(
		mo,
		(P_RandomRange(-2, 2) * FRACUNIT) + (24 * FINECOSINE(tireAngle >> ANGLETOFINESHIFT)),
		(P_RandomRange(-2, 2) * FRACUNIT) + (24 * FINESINE(tireAngle >> ANGLETOFINESHIFT)),
		0, fs->mobjType
	);

	P_SetTarget(&dust->target, mo);
	dust->angle = K_MomentumAngle(mo);

	dust->destscale = FixedMul(mo->scale, fs->scale);
	P_SetScale(dust, dust->destscale);

	dust->momx = mo->momx;
	dust->momy = mo->momy;
	dust->momz = P_GetMobjZMovement(mo) / 2;

	momentum = P_AproxDistance(mo->momx, mo->momy);
	momH = FixedMul(momentum, fs->pushH);
	momV = FixedMul(momentum, fs->pushV);

	dust->momx += FixedMul(momH, FINECOSINE(pushAngle >> ANGLETOFINESHIFT));
	dust->momy += FixedMul(momH, FINESINE(pushAngle >> ANGLETOFINESHIFT));
	dust->momz += (momV / 16) * P_MobjFlip(mo);

	if (fs->color != SKINCOLOR_NONE)
	{
		dust->color = fs->color;
	}

	if ((fs->sfx != sfx_None) && (fs->sfxFreq > 0) && (leveltime % fs->sfxFreq == 0))
	{
		S_StartSound(mo, fs->sfx);
	}
}

/*--------------------------------------------------
	void K_HandleFootstepParticles(mobj_t *mo)

		See header file for description.
--------------------------------------------------*/
void K_HandleFootstepParticles(mobj_t *mo)
{
	t_footstep_t *fs = NULL;

	if (mo == NULL || P_MobjWasRemoved(mo) == true)
	{
		// Invalid object.
		return;
	}

	if (mo->terrain == NULL || mo->terrain->footstepID == SIZE_MAX)
	{
		// If no terrain, check for offroad.
		// If we're in offroad, use the default particle.

		if (mo->player != NULL && mo->player->boostpower < FRACUNIT)
		{
			fs = K_GetFootstepByIndex(defaultOffroadFootstep);
		}
	}
	else
	{
		fs = K_GetFootstepByIndex(mo->terrain->footstepID);
	}

	if (fs == NULL || fs->mobjType == MT_NULL)
	{
		// No particles to spawn.
		return;
	}

	// Idea for later: if different spawning styles are desired,
	// we can put a switch case here!
	K_SpawnFootstepParticle(mo, fs);
}

/*--------------------------------------------------
	static void K_FlagBoolean(UINT32 *inputFlags, UINT32 newFlag, char *val)

		Sets a flag to true or false depending on
		the string input.

	Input Arguments:-
		inputFlags - Pointer to flags value to modify.
		newFlag - The flag(s) to set / unset.
		val - The string input from the file.

	Return:-
		None
--------------------------------------------------*/
static void K_FlagBoolean(UINT32 *inputFlags, UINT32 newFlag, char *val)
{
	if (stricmp(val, "true") == 0)
	{
		*inputFlags |= newFlag;
	}
	else if (stricmp(val, "false") == 0)
	{
		*inputFlags &= ~newFlag;
	}
}

/*--------------------------------------------------
	static void K_SplashDefaults(t_splash_t *splash)

		Sets the defaults for a new Splash block.

	Input Arguments:-
		splash - Terrain Splash structure to default.

	Return:-
		None
--------------------------------------------------*/
static void K_SplashDefaults(t_splash_t *splash)
{
	splash->mobjType = MT_NULL;
	splash->sfx = sfx_None;
	splash->scale = FRACUNIT;
	splash->color = SKINCOLOR_NONE;

	splash->pushH = FRACUNIT/4;
	splash->pushV = FRACUNIT/64;
	splash->spread = 2;
	splash->cone = ANGLE_11hh;

	splash->numParticles = 8;
}

/*--------------------------------------------------
	static void K_NewSplashDefs(void)

		Increases the size of splashDefs by 1, and
		sets the new struct's values to their defaults.

	Input Arguments:-
		None

	Return:-
		None
--------------------------------------------------*/
static void K_NewSplashDefs(void)
{
	numSplashDefs++;
	splashDefs = (t_splash_t *)Z_Realloc(splashDefs, sizeof(t_splash_t) * (numSplashDefs + 1), PU_STATIC, NULL);
	K_SplashDefaults( &splashDefs[numSplashDefs - 1] );
}

/*--------------------------------------------------
	static void K_ParseSplashParameter(size_t i, char *param, char *val)

		Parser function for Splash blocks.

	Input Arguments:-
		i - Struct ID
		param - Parameter string
		val - Value string

	Return:-
		None
--------------------------------------------------*/
static void K_ParseSplashParameter(size_t i, char *param, char *val)
{
	t_splash_t *splash = &splashDefs[i];

	if (stricmp(param, "mobjType") == 0)
	{
		splash->mobjType = get_number(val) + 1;
	}
	else if (stricmp(param, "sfx") == 0)
	{
		splash->sfx = get_number(val);
	}
	else if (stricmp(param, "scale") == 0)
	{
		splash->scale = FLOAT_TO_FIXED(atof(val));
	}
	else if (stricmp(param, "color") == 0)
	{
		splash->color = get_number(val);
	}
}

/*--------------------------------------------------
	static void K_FootstepDefaults(t_footstep_t *footstep)

		Sets the defaults for a new Footstep block.

	Input Arguments:-
		footstep - Terrain Footstep structure to default.

	Return:-
		None
--------------------------------------------------*/
static void K_FootstepDefaults(t_footstep_t *footstep)
{
	footstep->mobjType = MT_NULL;
	footstep->sfx = sfx_None;
	footstep->scale = FRACUNIT;
	footstep->color = SKINCOLOR_NONE;

	footstep->pushH = FRACUNIT/2;
	footstep->pushV = FRACUNIT/32;
	footstep->spread = 2;
	footstep->cone = ANGLE_11hh;

	footstep->sfxFreq = 6;
}

/*--------------------------------------------------
	static void K_NewFootstepDefs(void)

		Increases the size of footstepDefs by 1, and
		sets the new struct's values to their defaults.

	Input Arguments:-
		None

	Return:-
		None
--------------------------------------------------*/
static void K_NewFootstepDefs(void)
{
	numFootstepDefs++;
	footstepDefs = (t_footstep_t *)Z_Realloc(footstepDefs, sizeof(t_footstep_t) * (numFootstepDefs + 1), PU_STATIC, NULL);
	K_FootstepDefaults( &footstepDefs[numFootstepDefs - 1] );
}

/*--------------------------------------------------
	static void K_ParseFootstepParameter(size_t i, char *param, char *val)

		Parser function for Footstep blocks.

	Input Arguments:-
		i - Struct ID
		param - Parameter string
		val - Value string

	Return:-
		None
--------------------------------------------------*/
static void K_ParseFootstepParameter(size_t i, char *param, char *val)
{
	t_footstep_t *footstep = &footstepDefs[i];

	if (stricmp(param, "mobjType") == 0)
	{
		footstep->mobjType = get_number(val) + 1;
	}
	else if (stricmp(param, "sfx") == 0)
	{
		footstep->sfx = get_number(val);
	}
	else if (stricmp(param, "scale") == 0)
	{
		footstep->scale = FLOAT_TO_FIXED(atof(val));
	}
	else if (stricmp(param, "color") == 0)
	{
		footstep->color = get_number(val);
	}
}

/*--------------------------------------------------
	static void K_TerrainDefaults(terrain_t *terrain)

		Sets the defaults for a new Terrain block.

	Input Arguments:-
		terrain - Terrain structure to default.

	Return:-
		None
--------------------------------------------------*/
static void K_TerrainDefaults(terrain_t *terrain)
{
	terrain->splashID = SIZE_MAX;
	terrain->footstepID = SIZE_MAX;

	terrain->friction = FRACUNIT;
	terrain->offroad = 0;
	terrain->damageType = -1;
	terrain->trickPanel = 0;
	terrain->flags = 0;
}

/*--------------------------------------------------
	static void K_NewTerrainDefs(void)

		Increases the size of terrainDefs by 1, and
		sets the new struct's values to their defaults.

	Input Arguments:-
		None

	Return:-
		None
--------------------------------------------------*/
static void K_NewTerrainDefs(void)
{
	numTerrainDefs++;
	terrainDefs = (terrain_t *)Z_Realloc(terrainDefs, sizeof(terrain_t) * (numTerrainDefs + 1), PU_STATIC, NULL);
	K_TerrainDefaults( &terrainDefs[numTerrainDefs - 1] );
}

/*--------------------------------------------------
	static void K_ParseTerrainParameter(UINT32 i, char *param, char *val)

		Parser function for Terrain blocks.

	Input Arguments:-
		i - Struct ID
		param - Parameter string
		val - Value string

	Return:-
		None
--------------------------------------------------*/
static void K_ParseTerrainParameter(UINT32 i, char *param, char *val)
{
	terrain_t *terrain = &terrainDefs[i];

	if (stricmp(param, "splash") == 0)
	{
		t_splash_t *splash = K_GetSplashByName(val);
		terrain->splashID = K_GetSplashHeapIndex(splash);
	}
	else if (stricmp(param, "footstep") == 0)
	{
		t_footstep_t *footstep = K_GetFootstepByName(val);
		terrain->footstepID = K_GetFootstepHeapIndex(footstep);
	}
	else if (stricmp(param, "friction") == 0)
	{
		terrain->friction = FLOAT_TO_FIXED(atof(val));
	}
	else if (stricmp(param, "offroad") == 0)
	{
		terrain->offroad = (UINT8)get_number(val); // offroad strength enum?
	}
	else if (stricmp(param, "damageType") == 0)
	{
		terrain->damageType = (INT16)get_number(val);
	}
	else if (stricmp(param, "trickPanel") == 0)
	{
		terrain->trickPanel = (UINT8)get_number(val); // trick panel strength enum?
	}
	else if (stricmp(param, "liquid") == 0)
	{
		K_FlagBoolean(&terrain->flags, TRF_LIQUID, val);
	}
	else if (stricmp(param, "sneakerPanel") == 0)
	{
		K_FlagBoolean(&terrain->flags, TRF_SNEAKERPANEL, val);
	}
	else if (stricmp(param, "bumpy") == 0 || stricmp(param, "stairJank") == 0)
	{
		K_FlagBoolean(&terrain->flags, TRF_STAIRJANK, val);
	}
	else if (stricmp(param, "tripwire") == 0)
	{
		K_FlagBoolean(&terrain->flags, TRF_TRIPWIRE, val);
	}
}

/*--------------------------------------------------
	static void K_NewTerrainFloorDefs(void)

		Increases the size of numTerrainFloorDefs by 1.

	Input Arguments:-
		None

	Return:-
		None
--------------------------------------------------*/
static void K_NewTerrainFloorDefs(void)
{
	numTerrainFloorDefs++;
	terrainFloorDefs = (t_floor_t *)Z_Realloc(terrainFloorDefs, sizeof(t_floor_t) * (numTerrainFloorDefs + 1), PU_STATIC, NULL);
}

/*--------------------------------------------------
	static boolean K_DoTERRAINLumpParse(size_t num, void (*parser)(UINT32, char *, char *))

		Runs another parser function for the TERRAIN
		lump, handling the nitty-gritty parts of the
		token handling.

	Input Arguments:-
		num - Struct ID to modify. Which one it will modify depends on the parser function.
		parser - The parser function. Takes three inputs: Struct ID, Parameter String, and Value String.

	Return:-
		false if any errors occured, otherwise true.
--------------------------------------------------*/
static boolean K_DoTERRAINLumpParse(size_t num, void (*parser)(UINT32, char *, char *))
{
	char *param, *val;

	param = M_GetToken(NULL);

	if (!fastcmp(param, "{"))
	{
		Z_Free(param);
		CONS_Alert(CONS_WARNING, "Invalid TERRAIN data capsule!\n");
		return false;
	}

	Z_Free(param);

	while (true)
	{
		param = M_GetToken(NULL);

		if (fastcmp(param, "}"))
		{
			Z_Free(param);
			break;
		}

		val = M_GetToken(NULL);
		parser(num, param, val);

		Z_Free(param);
		Z_Free(val);
	}

	return true;
}

/*--------------------------------------------------
	static boolean K_TERRAINLumpParser(UINT8 *data, size_t size)

		Parses inputted lump data as a TERRAIN lump.

	Input Arguments:-
		data - Pointer to lump data.
		size - The length of the lump data.

	Return:-
		false if any errors occured, otherwise true.
--------------------------------------------------*/
static boolean K_TERRAINLumpParser(UINT8 *data, size_t size)
{
	char *tkn = M_GetToken((char *)data);
	size_t pos = 0;
	size_t i;

	while (tkn && (pos = M_GetTokenPos()) < size)
	{
		boolean valid = true;

		// Avoid anything inside bracketed stuff, only look for external keywords.
		if (fastcmp(tkn, "{") || fastcmp(tkn, "}"))
		{
			CONS_Alert(CONS_ERROR, "Rogue bracket detected in TERRAIN lump.\n");
			valid = false;
		}
		// Check for valid fields.
		else if (stricmp(tkn, "splash") == 0)
		{
			Z_Free(tkn);
			tkn = M_GetToken(NULL);
			pos = M_GetTokenPos();

			if (tkn && pos < size)
			{
				t_splash_t *s = NULL;

				for (i = 0; i < numSplashDefs; i++)
				{
					s = &splashDefs[i];

					if (stricmp(tkn, s->name) == 0)
					{
						break;
					}
				}

				if (i == numSplashDefs)
				{
					K_NewSplashDefs();
					s = &splashDefs[i];

					strncpy(s->name, tkn, TERRAIN_NAME_LEN);
					CONS_Printf("Created new Splash type '%s'\n", s->name);
				}

				valid = K_DoTERRAINLumpParse(i, K_ParseSplashParameter);
			}
			else
			{
				CONS_Alert(CONS_ERROR, "No Splash type name.\n");
				valid = false;
			}
		}
		else if (stricmp(tkn, "footstep") == 0)
		{
			Z_Free(tkn);
			tkn = M_GetToken(NULL);
			pos = M_GetTokenPos();

			if (tkn && pos < size)
			{
				t_footstep_t *fs = NULL;

				for (i = 0; i < numFootstepDefs; i++)
				{
					fs = &footstepDefs[i];

					if (stricmp(tkn, fs->name) == 0)
					{
						break;
					}
				}

				if (i == numFootstepDefs)
				{
					K_NewFootstepDefs();
					fs = &footstepDefs[i];

					strncpy(fs->name, tkn, TERRAIN_NAME_LEN);
					CONS_Printf("Created new Footstep type '%s'\n", fs->name);
				}

				valid = K_DoTERRAINLumpParse(i, K_ParseFootstepParameter);
			}
			else
			{
				CONS_Alert(CONS_ERROR, "No Footstep type name.\n");
				valid = false;
			}
		}
		else if (stricmp(tkn, "terrain") == 0)
		{
			Z_Free(tkn);
			tkn = M_GetToken(NULL);
			pos = M_GetTokenPos();

			if (tkn && pos < size)
			{
				terrain_t *t = NULL;

				for (i = 0; i < numTerrainDefs; i++)
				{
					t = &terrainDefs[i];

					if (stricmp(tkn, t->name) == 0)
					{
						break;
					}
				}

				if (i == numTerrainDefs)
				{
					K_NewTerrainDefs();
					t = &terrainDefs[i];

					strncpy(t->name, tkn, TERRAIN_NAME_LEN);
					CONS_Printf("Created new Terrain type '%s'\n", t->name);
				}

				valid = K_DoTERRAINLumpParse(i, K_ParseTerrainParameter);
			}
			else
			{
				CONS_Alert(CONS_ERROR, "No Terrain type name.\n");
				valid = false;
			}
		}
		else if (stricmp(tkn, "floor") == 0 || stricmp(tkn, "texture") == 0)
		{
			Z_Free(tkn);
			tkn = M_GetToken(NULL);
			pos = M_GetTokenPos();

			if (tkn && pos < size)
			{
				if (stricmp(tkn, "optional") == 0)
				{
					// "optional" is ZDoom syntax
					// We don't use it, but we can ignore it.
					Z_Free(tkn);
					tkn = M_GetToken(NULL);
					pos = M_GetTokenPos();
				}

				if (tkn && pos < size)
				{
					t_floor_t *f = NULL;

					for (i = 0; i < numTerrainFloorDefs; i++)
					{
						f = &terrainFloorDefs[i];

						if (stricmp(tkn, f->textureName) == 0)
						{
							break;
						}
					}

					if (i == numTerrainFloorDefs)
					{
						K_NewTerrainFloorDefs();
						f = &terrainFloorDefs[i];

						strncpy(f->textureName, tkn, 9);
					}

					Z_Free(tkn);
					tkn = M_GetToken(NULL);
					pos = M_GetTokenPos();

					if (tkn && pos < size)
					{
						terrain_t *t = K_GetTerrainByName(tkn);

						if (t == NULL)
						{
							CONS_Alert(CONS_ERROR, "Invalid Terrain type '%s'.\n", tkn);
							valid = false;
						}
						else
						{
							f->terrainID = K_GetTerrainHeapIndex(t);
							CONS_Printf("Texture '%s' set to Terrain '%s'\n", f->textureName, tkn);
						}
					}
					else
					{
						CONS_Alert(CONS_ERROR, "No terrain for floor definition.\n");
						valid = false;
					}
				}
				else
				{
					CONS_Alert(CONS_ERROR, "No texture for floor definition.\n");
					valid = false;
				}
			}
			else
			{
				CONS_Alert(CONS_ERROR, "No texture for floor definition.\n");
				valid = false;
			}
		}
		else if (stricmp(tkn, "defaultTerrain") == 0)
		{
			Z_Free(tkn);
			tkn = M_GetToken(NULL);
			pos = M_GetTokenPos();

			if (tkn && pos < size)
			{
				terrain_t *t = NULL;

				for (i = 0; i < numTerrainDefs; i++)
				{
					t = &terrainDefs[i];

					if (stricmp(tkn, t->name) == 0)
					{
						break;
					}
				}

				if (i == numTerrainDefs)
				{
					CONS_Alert(CONS_ERROR, "Invalid DefaultTerrain type.\n");
					valid = false;
				}
				else
				{
					defaultTerrain = i;
					CONS_Printf("DefaultTerrain set to '%s'\n", tkn);
				}
			}
			else
			{
				CONS_Alert(CONS_ERROR, "No DefaultTerrain type.\n");
				valid = false;
			}
		}
		else if (stricmp(tkn, "defaultOffroadFootstep") == 0)
		{
			Z_Free(tkn);
			tkn = M_GetToken(NULL);
			pos = M_GetTokenPos();

			if (tkn && pos < size)
			{
				t_footstep_t *fs = NULL;

				for (i = 0; i < numFootstepDefs; i++)
				{
					fs = &footstepDefs[i];

					if (stricmp(tkn, fs->name) == 0)
					{
						break;
					}
				}

				if (i == numFootstepDefs)
				{
					CONS_Alert(CONS_ERROR, "Invalid DefaultOffroadFootstep type.\n");
					valid = false;
				}
				else
				{
					defaultOffroadFootstep = i;
					CONS_Printf("DefaultOffroadFootstep set to '%s'\n", tkn);
				}
			}
			else
			{
				CONS_Alert(CONS_ERROR, "No DefaultOffroadFootstep type.\n");
				valid = false;
			}
		}
		else
		{
			CONS_Alert(CONS_ERROR, "Unknown field '%s' found in TERRAIN lump.\n", tkn);
			valid = false;
		}

		Z_Free(tkn);

		if (valid == false)
		{
			return false;
		}

		tkn = M_GetToken(NULL);
	}

	Z_Free(tkn);
	return true;
}

/*--------------------------------------------------
	void K_InitTerrain(UINT16 wadNum)

		See header file for description.
--------------------------------------------------*/
void K_InitTerrain(UINT16 wadNum)
{
	UINT16 lumpNum;
	lumpinfo_t *lump_p = wadfiles[wadNum]->lumpinfo;

	// Iterate through all lumps and compare the name individually.
	// In PK3 files, you can potentially have multiple TERRAIN differentiated by
	// their file extension.
	for (lumpNum = 0; lumpNum < wadfiles[wadNum]->numlumps; lumpNum++, lump_p++)
	{
		UINT8 *data;

		if (memcmp(lump_p->name, "TERRAIN", 8) != 0)
		{
			continue;
		}

		data = (UINT8 *)W_CacheLumpNumPwad(wadNum, lumpNum, PU_STATIC);

		// If that didn't exist, we have nothing to do here.
		if (data == NULL)
		{
			continue;
		}
		else
		{
			size_t size = W_LumpLengthPwad(wadNum, lumpNum);

			size_t nameLength = strlen(wadfiles[wadNum]->filename) + 1 + strlen(lump_p->fullname); // length of file name, '|', and lump name
			char *name = malloc(nameLength + 1);

			sprintf(name, "%s|%s", wadfiles[wadNum]->filename, lump_p->fullname);
			name[nameLength] = '\0';

			size = W_LumpLengthPwad(wadNum, lumpNum);

			CONS_Printf(M_GetText("Loading TERRAIN from %s\n"), name);
			K_TERRAINLumpParser(data, size);

			free(name);
		}
	}
}
