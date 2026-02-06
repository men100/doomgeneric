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
//	Zone Memory Allocation. Neat.
//


#include "z_zone.h"
#include "i_system.h"
#include "doomtype.h"


//
// ZONE MEMORY ALLOCATION
//
// There is never any space between memblocks,
//  and there will never be two contiguous free memblocks.
// The rover can be left pointing at a non-empty block.
//
// It is of no value to free a cachable block,
//  because it will get overwritten automatically if needed.
// 
 
#define MEM_ALIGN sizeof(void *)
#define ZONEID	0x1d4a11

typedef struct memblock_s
{
    int			size;	// including the header and possibly tiny fragments
    void**		user;
    int			tag;	// PU_FREE if this is free
    int			id;	// should be ZONEID
    struct memblock_s*	next;
    struct memblock_s*	prev;
} memblock_t;


typedef struct
{
    // total bytes malloced, including header
    int		size;

    // start / end cap for linked list
    memblock_t	blocklist;
    
    memblock_t*	rover;
    
} memzone_t;



memzone_t*	mainzone;
memzone_t*	secondaryzone;

extern void I_GetSecondaryZone(byte **ptr, int *size);

//
// Z_ClearZone
//
void Z_ClearZone (memzone_t* zone)
{
    memblock_t*		block;

    // set the entire zone to one free block
    zone->blocklist.next =
	zone->blocklist.prev =
	block = (memblock_t *)( (byte *)zone + sizeof(memzone_t) );
    
    zone->blocklist.user = (void *)zone;
    zone->blocklist.tag = PU_STATIC;
    zone->rover = block;
	
    block->prev = block->next = &zone->blocklist;
    
    // a free block.
    block->tag = PU_FREE;

    block->size = zone->size - sizeof(memzone_t);
}



//
// Z_Init
//
void Z_Init (void)
{
    memblock_t*	block;
    int		size;

    // Initialize Main Zone (Main RAM)
    mainzone = (memzone_t *)I_ZoneBase (&size);
    mainzone->size = size;
    Z_ClearZone(mainzone);

    // Initialize Secondary Zone (GNSS RAM)
    byte* sec_ptr;
    int sec_size;
    I_GetSecondaryZone(&sec_ptr, &sec_size);
    
    secondaryzone = (memzone_t *)sec_ptr;
    secondaryzone->size = sec_size;
    Z_ClearZone(secondaryzone);
    
    printf("Z_Init: Main Zone (Main RAM) %p size %d, Secondary Zone (GNSS) %p size %d\n", 
           mainzone, mainzone->size, secondaryzone, secondaryzone->size);
}


//
// Z_Free
//
void Z_Free (void* ptr)
{
    memblock_t*		block;
    memblock_t*		other;
    memzone_t*		zone;

    block = (memblock_t *) ( (byte *)ptr - sizeof(memblock_t));

    if (block->id != ZONEID)
	I_Error ("Z_Free: freed a pointer without ZONEID");

    // ブロックがどちらの zone に属するかを判定
    if ((byte*)block >= (byte*)mainzone && (byte*)block < (byte*)mainzone + mainzone->size)
    {
        zone = mainzone;
    }
    else if ((byte*)block >= (byte*)secondaryzone && (byte*)block < (byte*)secondaryzone + secondaryzone->size)
    {
        zone = secondaryzone;
    }
    else
    {
        I_Error("Z_Free: Pointer %p is not in any zone!", ptr);
        return;
    }

    if (block->tag != PU_FREE && block->user != NULL)
    {
        // clear the user's mark
	    *block->user = 0;
    }

    // mark as free
    block->tag = PU_FREE;
    block->user = NULL;
    block->id = 0;

    other = block->prev;

    if (other->tag == PU_FREE)
    {
        // merge with previous free block
        other->size += block->size;
        other->next = block->next;
        other->next->prev = other;

        if (block == zone->rover)
            zone->rover = other;

        block = other;
    }

    other = block->next;
    if (other->tag == PU_FREE)
    {
        // merge the next free block onto the end
        block->size += other->size;
        block->next = other->next;
        block->next->prev = block;

        if (other == zone->rover)
            zone->rover = block;
    }
}



//
// Z_Malloc
// You can pass a NULL user if the tag is < PU_PURGELEVEL.
//
#define MINFRAGMENT		64


// 特定の zone から allocate を試行する helper 関数
void* Z_Malloc_Zone(memzone_t* zone, int size, int tag, void* user)
{
    int		extra;
    memblock_t*	start;
    memblock_t* rover;
    memblock_t* newblock;
    memblock_t*	base;

    // 呼び手 (Z_Malloc) で実施済
    // size = (size + MEM_ALIGN - 1) & ~(MEM_ALIGN - 1);
    // size += sizeof(memblock_t);

    base = zone->rover;
    
    if (base->prev->tag == PU_FREE)
        base = base->prev;

    rover = base;
    start = base->prev;

    do
    {
        if (rover == start)
        {
            // リストを最後までスキャンした
            return NULL;
        }

        if (rover->tag != PU_FREE)
        {
            if (rover->tag < PU_PURGELEVEL)
            {
                base = rover = rover->next;
            }
            else
            {
                base = base->prev;
                Z_Free ((byte *)rover+sizeof(memblock_t));
                base = base->next;
                rover = base->next;
            }
        }
        else
        {
            rover = rover->next;
        }

    } while (base->tag != PU_FREE || base->size < size);

    
    extra = base->size - size;
    
    if (extra >  MINFRAGMENT)
    {
        newblock = (memblock_t *) ((byte *)base + size );
        newblock->size = extra;

        newblock->tag = PU_FREE;
        newblock->user = NULL;
        newblock->prev = base;
        newblock->next = base->next;
        newblock->next->prev = newblock;

        base->next = newblock;
        base->size = size;
    }
    
    if (user)
    {
        base->user = user;
        *(void **)user = (void *) ((byte *)base + sizeof(memblock_t));
    }
    else
    {
        if (tag >= PU_PURGELEVEL)
            I_Error ("Z_Malloc: an owner is required for purgable blocks");

        base->user = NULL;
    }
    
    base->tag = tag;
    base->id = ZONEID;
    
    zone->rover = base->next;
    
    return (void *) ((byte *)base + sizeof(memblock_t));
}

void*
Z_Malloc
( int		size,
  int		tag,
  void*		user )
{
    void* result;
    int original_size = size;

    size = (size + MEM_ALIGN - 1) & ~(MEM_ALIGN - 1);
    size += sizeof(memblock_t);
    
    // Try Main Zone (Main RAM)
    result = Z_Malloc_Zone(mainzone, size, tag, user);
    
    if (result == NULL)
    {
        // Try Secondary Zone (GNSS RAM)
        // printf("Z_Malloc: Main zone full, trying secondary for %d bytes\n", original_size);
        result = Z_Malloc_Zone(secondaryzone, size, tag, user);
    }

    if (result == NULL)
    {
        if (tag >= PU_CACHE) {
            // キャッシュ可能なブロックなので、NULL を返しても救える可能性がある
            printf("Z_Malloc: failed on allocation of %i bytes (Both zones full)\n", original_size);
            return NULL;
        }
        
        // こちらは救えない
        I_Error ("Z_Malloc: failed on allocation of %i bytes", original_size);
    }

    return result;
}


//
// Z_FreeTags_Zone
//
void Z_FreeTags_Zone(memzone_t* zone, int lowtag, int hightag)
{
    memblock_t*	block;
    memblock_t*	next;
	
    for (block = zone->blocklist.next ;
	 block != &zone->blocklist ;
	 block = next)
    {
	// get link before freeing
	next = block->next;

	// free block?
	if (block->tag == PU_FREE)
	    continue;
	
	if (block->tag >= lowtag && block->tag <= hightag)
	    Z_Free ( (byte *)block+sizeof(memblock_t));
    }
}

void
Z_FreeTags
(
  int           lowtag,
  int           hightag )
{
    Z_FreeTags_Zone(mainzone, lowtag, hightag);
    Z_FreeTags_Zone(secondaryzone, lowtag, hightag);
}


//
// Z_DumpHeap_Zone
//
void Z_DumpHeap_Zone(memzone_t* zone, int lowtag, int hightag)
{
    memblock_t*	block;
	
    printf ("zone size: %i  location: %p\n",
	    zone->size,zone);
    
    printf ("tag range: %i to %i\n",
	    lowtag, hightag);
	
    for (block = zone->blocklist.next ; ; block = block->next)
    {
	if (block->tag >= lowtag && block->tag <= hightag)
	    printf ("block:%p    size:%7i    user:%p    tag:%3i\n",
		    block, block->size, block->user, block->tag);
		
	if (block->next == &zone->blocklist)
	{
	    // all blocks have been hit
	    break;
	}
	
	if ( (byte *)block + block->size != (byte *)block->next)
	    printf ("ERROR: block size does not touch the next block\n");

	if ( block->next->prev != block)
	    printf ("ERROR: next block doesn't have proper back link\n");

	if (block->tag == PU_FREE && block->next->tag == PU_FREE)
	    printf ("ERROR: two consecutive free blocks\n");
    }
}

void
Z_DumpHeap
(
  int           lowtag,
  int           hightag )
{
    Z_DumpHeap_Zone(mainzone, lowtag, hightag);
    Z_DumpHeap_Zone(secondaryzone, lowtag, hightag);
}


//
// Z_FileDumpHeap
//
void Z_FileDumpHeap (FILE* f)
{
    memblock_t*	block;
	
    fprintf (f,"zone size: %i  location: %p\n",mainzone->size,mainzone);
	
    for (block = mainzone->blocklist.next ; ; block = block->next)
    {
	fprintf (f,"block:%p    size:%7i    user:%p    tag:%3i\n",
		 block, block->size, block->user, block->tag);
		
	if (block->next == &mainzone->blocklist)
	{
	    // all blocks have been hit
	    break;
	}
	
	if ( (byte *)block + block->size != (byte *)block->next)
	    fprintf (f,"ERROR: block size does not touch the next block\n");

	if ( block->next->prev != block)
	    fprintf (f,"ERROR: next block doesn't have proper back link\n");

	if (block->tag == PU_FREE && block->next->tag == PU_FREE)
	    fprintf (f,"ERROR: two consecutive free blocks\n");
    }
}



//
// Z_CheckHeap_Zone
//
void Z_CheckHeap_Zone(memzone_t* zone)
{
    memblock_t*	block;
	
    for (block = zone->blocklist.next ; ; block = block->next)
    {
	if (block->next == &zone->blocklist)
	{
	    // all blocks have been hit
	    break;
	}
	
	if ( (byte *)block + block->size != (byte *)block->next)
	    I_Error ("Z_CheckHeap: block size does not touch the next block\n");

	if ( block->next->prev != block)
	    I_Error ("Z_CheckHeap: next block doesn't have proper back link\n");

	if (block->tag == PU_FREE && block->next->tag == PU_FREE)
	    I_Error ("Z_CheckHeap: two consecutive free blocks\n");
    }
}

void Z_CheckHeap (void)
{
    Z_CheckHeap_Zone(mainzone);
    Z_CheckHeap_Zone(secondaryzone);
}


//
// Z_ChangeTag
//
void Z_ChangeTag2(void *ptr, int tag, char *file, int line)
{
    memblock_t*	block;
	
    block = (memblock_t *) ((byte *)ptr - sizeof(memblock_t));

    if (block->id != ZONEID)
        I_Error("%s:%i: Z_ChangeTag: block without a ZONEID!",
                file, line);

    if (tag >= PU_PURGELEVEL && block->user == NULL)
        I_Error("%s:%i: Z_ChangeTag: an owner is required "
                "for purgable blocks", file, line);

    block->tag = tag;
}

void Z_ChangeUser(void *ptr, void **user)
{
    memblock_t*	block;

    block = (memblock_t *) ((byte *)ptr - sizeof(memblock_t));

    if (block->id != ZONEID)
    {
        I_Error("Z_ChangeUser: Tried to change user for invalid block!");
    }

    block->user = user;
    *user = ptr;
}



//
// Z_FreeMemory
//
int Z_FreeMemory_Zone(memzone_t* zone)
{
    memblock_t*		block;
    int			free;
	
    free = 0;
    
    for (block = zone->blocklist.next ;
         block != &zone->blocklist;
         block = block->next)
    {
        if (block->tag == PU_FREE || block->tag >= PU_PURGELEVEL)
            free += block->size;
    }

    return free;
}

int Z_FreeMemory (void)
{
    return Z_FreeMemory_Zone(mainzone) + Z_FreeMemory_Zone(secondaryzone);
}

void Z_GetFreeMemory(int* main_free, int* sec_free)
{
    *main_free = Z_FreeMemory_Zone(mainzone);
    *sec_free = Z_FreeMemory_Zone(secondaryzone);
}

unsigned int Z_ZoneSize(void)
{
    return mainzone->size + secondaryzone->size;
}