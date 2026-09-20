//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	WAD I/O functions.
//


#ifndef __W_WAD__
#define __W_WAD__

#include <stdio.h>

#include "doomtype.h"
#include "d_mode.h"

#include "w_file.h"


//
// TYPES
//

//
// WADFILE I/O related stuff.
//

typedef struct lumpinfo_s lumpinfo_t;

// Slimmed for the badge: the one WAD is memory-mapped, so a lump is never
// read into a zone cache and there is only one wad_file to point at. That
// removes two pointers, and the hash chain is a 16-bit index rather than a
// third, so an entry is 20 bytes instead of 32 -- 14 KB of DRAM across the
// table, which is what let the Wi-Fi driver and the arena share the zone.
struct lumpinfo_s
{
    char	name[8];
    int		position;
    int		size;

    // Used for hash table lookups: index + 1 of the next lump in the
    // chain, 0 at the end.
    unsigned short next;
};


extern lumpinfo_t *lumpinfo;
extern unsigned int numlumps;

wad_file_t *W_AddFile (char *filename);

int	W_CheckNumForName (char* name);
int	W_GetNumForName (char* name);

int	W_LumpLength (unsigned int lump);
void    W_ReadLump (unsigned int lump, void *dest);

void*	W_CacheLumpNum (int lump, int tag);
void*	W_CacheLumpName (char* name, int tag);

void    W_GenerateHashTable(void);

extern unsigned int W_LumpNameHash(const char *s);

void    W_ReleaseLumpNum(int lump);
void    W_ReleaseLumpName(char *name);

void W_CheckCorrectIWAD(GameMission_t mission);

#endif
