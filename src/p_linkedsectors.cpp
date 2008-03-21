/*
** p_linkedsectors.cpp
**
** Linked sector movement
**
**---------------------------------------------------------------------------
** Copyright 2008 Christoph Oelckers
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

#include "templates.h"
#include "p_local.h"
#include "p_lnspec.h"

enum
{
	// The different movement types.
	LINK_NONE=0,
	LINK_FLOOR=1,
	LINK_CEILING=2,
	LINK_BOTH=3,
	LINK_FLOORMIRRORFLAG=4,
	LINK_CEILINGMIRRORFLAG=8,

	LINK_FLOORMIRROR=5,
	LINK_CEILINGMIRROR=10,
	LINK_BOTHMIRROR=15,

	LINK_FLAGMASK = 15

};


//============================================================================
//
// Checks whether the other sector is linked to this one
// Used to ignore linked sectors in the FindNextLowest/Highest* 
// functions
//
// NOTE: After looking at Eternity's code I discovered that this check
// is not done for the FindNext* function but instead only for 
// FindLowestCeilingSurrounding where IMO it makes much less sense
// because the most frequent use of this feature is most likely lifts
// with a more detailed surface. Needs to be investigated!
//
//============================================================================

bool sector_t::IsLinked(sector_t *other, bool ceiling) const
{
	extsector_t::linked::plane &scrollplane = ceiling? e->Linked.Ceiling : e->Linked.Floor;
	int flag = ceiling? LINK_CEILING : LINK_FLOOR;

	for(unsigned i = 0; i < scrollplane.Sectors.Size(); i++)
	{
		if (other == scrollplane.Sectors[i].Sector && scrollplane.Sectors[i].Type & flag) return true;
	}
	return false;
}


//============================================================================
//
// Helper functions for P_MoveLinkedSectors
//
//============================================================================

static bool MoveCeiling(sector_t *sector, int crush, fixed_t move)
{
	sector->ceilingplane.ChangeHeight (move);
	sector->ceilingtexz += move;

	if (P_ChangeSector(sector, crush, move, 1)) return false;

	// Don't let the ceiling go below the floor
	if ((sector->ceilingplane.a | sector->ceilingplane.b |
		 sector->floorplane.a | sector->floorplane.b) == 0 &&
		 sector->floortexz > sector->ceilingtexz) return false;

	return true;
}

static bool MoveFloor(sector_t *sector, int crush, fixed_t move)
{
	sector->floorplane.ChangeHeight (move);
	sector->floortexz += move;

	if (P_ChangeSector(sector, crush, move, 0)) return false;

	// Don't let the floor go above the ceiling
	if ((sector->ceilingplane.a | sector->ceilingplane.b |
		 sector->floorplane.a | sector->floorplane.b) == 0 &&
		 sector->floortexz > sector->ceilingtexz) return false;

	return true;
}

//============================================================================
//
// P_MoveLinkedSectors
//
// Moves all floors/ceilings linked to the control sector
// Important: All sectors must complete their movement
// even if a previous one already failed.
//
//============================================================================

bool P_MoveLinkedSectors(sector_t *sector, int crush, fixed_t move, bool ceiling)
{
	extsector_t::linked::plane &scrollplane = ceiling? sector->e->Linked.Ceiling : sector->e->Linked.Floor;
	bool ok = true;

	for(unsigned i = 0; i < scrollplane.Sectors.Size(); i++)
	{
		switch(scrollplane.Sectors[i].Type)
		{
		case LINK_FLOOR:
			ok &= MoveFloor(scrollplane.Sectors[i].Sector, crush, move);
			break;

		case LINK_CEILING:
			ok &= MoveCeiling(scrollplane.Sectors[i].Sector, crush, move);
			break;

		case LINK_BOTH:
			if (move < 0)
			{
				ok &= MoveFloor(scrollplane.Sectors[i].Sector, crush, move);
				ok &= MoveCeiling(scrollplane.Sectors[i].Sector, crush, move);
			}
			else
			{
				ok &= MoveCeiling(scrollplane.Sectors[i].Sector, crush, move);
				ok &= MoveFloor(scrollplane.Sectors[i].Sector, crush, move);
			}
			break;

		case LINK_FLOORMIRROR:
			ok &= MoveFloor(scrollplane.Sectors[i].Sector, crush, -move);
			break;

		case LINK_CEILINGMIRROR:
			ok &= MoveCeiling(scrollplane.Sectors[i].Sector, crush, -move);
			break;

		case LINK_BOTHMIRROR:
			if (move > 0)
			{
				ok &= MoveFloor(scrollplane.Sectors[i].Sector, crush, -move);
				ok &= MoveCeiling(scrollplane.Sectors[i].Sector, crush, -move);
			}
			else
			{
				ok &= MoveCeiling(scrollplane.Sectors[i].Sector, crush, -move);
				ok &= MoveFloor(scrollplane.Sectors[i].Sector, crush, -move);
			}
			break;

		case LINK_FLOOR+LINK_CEILINGMIRROR:
			ok &= MoveFloor(scrollplane.Sectors[i].Sector, crush, move);
			ok &= MoveCeiling(scrollplane.Sectors[i].Sector, crush, -move);
			break;

		case LINK_CEILING+LINK_FLOORMIRROR:
			ok &= MoveFloor(scrollplane.Sectors[i].Sector, crush, -move);
			ok &= MoveCeiling(scrollplane.Sectors[i].Sector, crush, move);
			break;

		default:
			// all other types are invalid and have to be elimintated in the attachment stage
			break;
		}
	}
	return ok;
}

//============================================================================
//
// P_StartLinkedSectorInterpolations
//
// Starts interpolators for every sector plane is being changed by moving
// this sector
//
//============================================================================

void P_StartLinkedSectorInterpolations(sector_t *sector, bool ceiling)
{
	extsector_t::linked::plane &scrollplane = ceiling? sector->e->Linked.Ceiling : sector->e->Linked.Floor;

	for(unsigned i = 0; i < scrollplane.Sectors.Size(); i++)
	{
		if (scrollplane.Sectors[i].Type & LINK_FLOOR)
		{
			setinterpolation(INTERP_SectorFloor, scrollplane.Sectors[i].Sector, false);
		}
		if (scrollplane.Sectors[i].Type & LINK_CEILING)
		{
			setinterpolation(INTERP_SectorCeiling, scrollplane.Sectors[i].Sector, false);
		}
	}
}

//============================================================================
//
// P_StopLinkedSectorInterpolations
//
// Stops interpolators for every sector plane is being changed by moving
// this sector
//
//============================================================================

void P_StopLinkedSectorInterpolations(sector_t *sector, bool ceiling)
{
	extsector_t::linked::plane &scrollplane = ceiling? sector->e->Linked.Ceiling : sector->e->Linked.Floor;

	for(unsigned i = 0; i < scrollplane.Sectors.Size(); i++)
	{
		if (scrollplane.Sectors[i].Type & LINK_FLOOR)
		{
			stopinterpolation(INTERP_SectorFloor, scrollplane.Sectors[i].Sector, false);
		}
		if (scrollplane.Sectors[i].Type & LINK_CEILING)
		{
			stopinterpolation(INTERP_SectorCeiling, scrollplane.Sectors[i].Sector, false);
		}
	}
}



//============================================================================
//
// AddSingleSector
//
// Adds a single sector to a scroll plane. Checks for invalid
// flag combinations if the sector is already added and removes it if necessary.
//
//============================================================================

static void AddSingleSector(extsector_t::linked::plane &scrollplane, sector_t *sector, int movetype)
{
	// First we have to check the list if the sector is already in it
	// If so the move type may have to be adjusted or the link to be removed

	for(unsigned i = 0; i < scrollplane.Sectors.Size(); i++)
	{
		if (scrollplane.Sectors[i].Sector == sector)
		{
			int oldtype = scrollplane.Sectors[i].Type;

			if ((oldtype & (LINK_FLOOR|LINK_FLOORMIRROR)) && 
				(movetype & (LINK_FLOORMIRROR|LINK_FLOOR)))
			{
				// Invalid combination for floor.
				movetype &= ~(LINK_FLOOR + LINK_FLOORMIRROR);
			}

			if ((oldtype & (LINK_CEILING|LINK_CEILINGMIRROR)) && 
				(movetype == LINK_CEILINGMIRROR || movetype == LINK_CEILING))
			{
				// Invalid combination for CEILING.
				movetype &= ~(LINK_CEILING + LINK_CEILINGMIRROR);
			}

			scrollplane.Sectors[i].Type |= movetype;
			return;
		}
	}

	// The sector hasn't been attached yet so do it now

	FLinkedSector newlink = {sector, movetype};
	scrollplane.Sectors.Push(newlink);
}

//============================================================================
//
// RemoveTaggedSectors
//
// Remove all sectors with the given tag from the link list
//
//============================================================================

static void RemoveTaggedSectors(extsector_t::linked::plane &scrollplane, int tag)
{
	for(int i = scrollplane.Sectors.Size()-1; i>=0; i--)
	{
		if (scrollplane.Sectors[i].Sector->tag == tag)
		{
			scrollplane.Sectors.Delete(i);
		}
	}
}



//============================================================================
//
// P_AddSectorLink
//
// Links sector planes to a control sector based on the sector's tag
//
//============================================================================

bool P_AddSectorLinks(sector_t *control, int tag, INTBOOL ceiling, int movetype)
{
	int param = movetype;

	// can't be done if the control sector is moving.
	if ((ceiling && control->ceilingdata) || (!ceiling && control->floordata)) return false;

	// Make sure we have only valid combinations
	movetype &= LINK_FLAGMASK;
	if ((movetype & LINK_FLOORMIRROR) == LINK_FLOORMIRRORFLAG) movetype &= ~LINK_FLOORMIRRORFLAG;
	if ((movetype & LINK_CEILINGMIRROR) == LINK_CEILINGMIRRORFLAG) movetype &= ~LINK_CEILINGMIRRORFLAG;

	// Don't remove any sector if the parameter is invalid.
	// Addition may still be performed based on the given value.
	if (param != 0 && movetype == 0) return false;

	extsector_t::linked::plane &scrollplane = ceiling? control->e->Linked.Ceiling : control->e->Linked.Floor;

	if (movetype > 0)
	{
		for(int sec = -1; (sec = P_FindSectorFromTag(tag, sec)) >= 0; )
		{
			// Don't attach to self!
			if (control != &sectors[sec])
			{
				AddSingleSector(scrollplane, &sectors[sec], movetype);
			}
		}
	}
	else
	{
		RemoveTaggedSectors(scrollplane, tag);
	}
	return true;
}

//============================================================================
//
// P_AddSectorLinksByID
//
// Links sector planes to a control sector based on a control linedef
// touching the sectors. This is the method Eternity uses and is here
// mostly so that the Eternity line types can be emulated
//
//============================================================================

void P_AddSectorLinksByID(sector_t *control, int id, INTBOOL ceiling)
{
	extsector_t::linked::plane &scrollplane = ceiling? control->e->Linked.Ceiling : control->e->Linked.Floor;

	for(int line = -1; (line = P_FindLineFromID(id, line)) >= 0; )
	{
		line_t *ld = &lines[line];

		if (ld->special == Static_Init && ld->args[1] == Init_SectorLink)
		{
			int movetype = ld->args[3];

			// Make sure we have only valid combinations
			movetype &= LINK_FLAGMASK;
			if ((movetype & LINK_FLOORMIRROR) == LINK_FLOORMIRRORFLAG) movetype &= ~LINK_FLOORMIRRORFLAG;
			if ((movetype & LINK_CEILINGMIRROR) == LINK_CEILINGMIRRORFLAG) movetype &= ~LINK_CEILINGMIRRORFLAG;

			if (movetype != 0 && ld->frontsector != NULL && ld->frontsector != control)
			{
				AddSingleSector(scrollplane, ld->frontsector, movetype);
			}
		}
	}
}