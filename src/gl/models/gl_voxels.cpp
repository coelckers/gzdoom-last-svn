/*
** gl_voxels.cpp
**
** Voxel management
**
**---------------------------------------------------------------------------
** Copyright 2010 Christoph Oelckers
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
** 4. When not used as part of GZDoom or a GZDoom derivative, this code will be
**    covered by the terms of the GNU Lesser General Public License as published
**    by the Free Software Foundation; either version 2.1 of the License, or (at
**    your option) any later version.
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

#include "gl/system/gl_system.h"
#include "w_wad.h"
#include "cmdlib.h"
#include "sc_man.h"
#include "m_crc32.h"
#include "c_console.h"
#include "g_game.h"
#include "doomstat.h"
#include "g_level.h"
#include "textures/bitmap.h"
//#include "gl/gl_intern.h"

#include "gl/renderer/gl_renderer.h"
#include "gl/scene/gl_drawinfo.h"
#include "gl/models/gl_models.h"
#include "gl/textures/gl_material.h"
#include "gl/utility/gl_geometric.h"
#include "gl/utility/gl_convert.h"


//===========================================================================
//
// Creates a 16x16 texture from the palette so that we can
// use the existing palette manipulation code to render the voxel
// Otherwise all shaders had to be duplicated and the non-shader code
// would be a lot less efficient.
//
//===========================================================================

class FVoxelTexture : public FTexture
{
public:

	FVoxelTexture(FVoxel *voxel);
	~FVoxelTexture();
	const BYTE *GetColumn (unsigned int column, const Span **spans_out);
	const BYTE *GetPixels ();
	void Unload ();

	int CopyTrueColorPixels(FBitmap *bmp, int x, int y, int rotate, FCopyInfo *inf);
	bool UseBasePalette() { return false; }

protected:
	FVoxel *SourceVox;

};

//===========================================================================
//
// 
//
//===========================================================================

FVoxelTexture::FVoxelTexture(FVoxel *vox)
{
	SourceVox = vox;
	Width = 16;
	Height = 16;
	WidthBits = 4;
	HeightBits = 4;
	WidthMask = 15;
	gl_info.bNoFilter = true;
}

//===========================================================================
//
// 
//
//===========================================================================

FVoxelTexture::~FVoxelTexture()
{
}

const BYTE *FVoxelTexture::GetColumn (unsigned int column, const Span **spans_out)
{
	// not needed
	return NULL;
}

const BYTE *FVoxelTexture::GetPixels ()
{
	// not needed
	return NULL;
}

void FVoxelTexture::Unload ()
{
}

//===========================================================================
//
// FVoxelTexture::CopyTrueColorPixels
//
// This creates a dummy 16x16 paletted bitmap and converts that using the
// voxel palette
//
//===========================================================================

int FVoxelTexture::CopyTrueColorPixels(FBitmap *bmp, int x, int y, int rotate, FCopyInfo *inf)
{
	PalEntry pe[256];
	BYTE bitmap[256];
	BYTE *pp = SourceVox->Palette;

	for(int i=0;i<256;i++, pp+=3)
	{
		bitmap[i] = (BYTE)i;
		pe[i].r = (pp[0] << 2) | (pp[0] >> 4);
		pe[i].g = (pp[1] << 2) | (pp[1] >> 4);
		pe[i].b = (pp[2] << 2) | (pp[2] >> 4);
		pe[i].a = 255;
    }
    
	bmp->CopyPixelData(x, y, bitmap, Width, Height, 1, 16, rotate, pe, inf);
	return 0;
}	


//===========================================================================
//
// 
//
//===========================================================================

FVoxelModel::FVoxelModel(FVoxel *voxel, bool owned)
{
	mVoxel = voxel;
	mOwningVoxel = owned;
	mVBO = NULL;
	mPalette = new FVoxelTexture(voxel);
	Initialize();
}

//===========================================================================
//
// 
//
//===========================================================================

FVoxelModel::~FVoxelModel()
{
	CleanGLData();
	delete mPalette;
	if (mOwningVoxel) delete mVoxel;
}

//===========================================================================
//
// 
//
//===========================================================================

#if 0 // for later
struct FVoxelVertexHash
{
	// Returns the hash value for a key.
	hash_t Hash(const FVoxelVertex &key) { return (hash_t)FLOAT2FIXED(key.x+256*key.y+65536*key.z); }

	// Compares two keys, returning zero if they are the same.
	int Compare(const FVoxelVertex &left, const FVoxelVertex &right) 
	{ 
		return left.x != right.x || left.y != right.y || left.z != right.z || left.u != right.u || left.v != right.v;
	}
};

struct FIndexInit
{
	void Init(unsigned int &value)
	{
		value = 0xffffffff;
	}
};


typedef TMap<FVoxelVertex, unsigned int, FVoxelVertexHash, FIndexInit> FVoxelMap;

#endif

//===========================================================================
//
// 
//
//===========================================================================

void FVoxelModel::AddVertex(FVoxelVertex &vert)
{
	// should use the map to optimize the indices later
	mIndices.Push(mVertices.Push(vert));
}

//===========================================================================
//
// 
//
//===========================================================================

void FVoxelModel::AddFace(int x1, int y1, int z1, int x2, int y2, int z2, int x3, int y3, int z3, int x4, int y4, int z4, BYTE col)
{
	float PivotX = FIXED2FLOAT(mVoxel->Mips[0].PivotX);
	float PivotY = FIXED2FLOAT(mVoxel->Mips[0].PivotX);
	float PivotZ = FIXED2FLOAT(mVoxel->Mips[0].PivotX);
	int h = mVoxel->Mips[0].SizeZ;
	FVoxelVertex vert;

	vert.u = ((col & 15) * 255 / 16) + 7;
	vert.v = ((col / 16) * 255 / 16) + 7;

	vert.x = -x1 + PivotX;
	vert.z = y1 - PivotX;
	vert.y = h - z1 + PivotX;
	AddVertex(vert);

	vert.x = -x2 + PivotX;
	vert.z = y2 - PivotX;
	vert.y = h - z2 + PivotX;
	AddVertex(vert);

	vert.x = -x4 + PivotX;
	vert.z = y4 - PivotX;
	vert.y = h - z4 + PivotX;
	AddVertex(vert);

	vert.x = -x3 + PivotX;
	vert.z = y3 - PivotX;
	vert.y = h - z3 + PivotX;
	AddVertex(vert);

}

//===========================================================================
//
// 
//
//===========================================================================

void FVoxelModel::MakeSlabPolys(int x, int y, kvxslab_t *voxptr)
{
	const BYTE *col = voxptr->col;
	int zleng = voxptr->zleng;
	int ztop = voxptr->ztop;
	int cull = voxptr->backfacecull;

	if (cull & 16)
	{
		AddFace(x, y, ztop, x+1, y, ztop, x, y+1, ztop, x+1, y+1, ztop, *col);
	}

	for(int z = ztop; z < ztop+zleng; z++, col++)
	{
		if (cull & 1)
		{
			AddFace(x, y, z, x, y+1, z, x, y, z+1, x, y+1, z+1, *col);
		}
		if (cull & 2)
		{
			AddFace(x+1, y+1, z, x+1, y, z, x+1, y+1, z+1, x+1, y, z+1, *col);
		}
		if (cull & 4)
		{
			AddFace(x, y, z, x+1, y, z, x, y, z+1, x+1, y, z+1, *col);
		}
		if (cull & 8)
		{
			AddFace(x+1, y+1, z, x, y+1, z, x+1, y+1, z+1, x, y+1, z+1, *col);
		}
	}
	if (cull & 32)
	{
		int z = ztop+zleng-1;
		AddFace(x+1, y, z+1, x, y, z+1, x+1, y+1, z+1, x, y+1, z+1, *col);
	}
}

//===========================================================================
//
// 
//
//===========================================================================

void FVoxelModel::Initialize()
{
	FVoxelMipLevel *mip = &mVoxel->Mips[0];
	for (int x = 0; x < mip->SizeX; x++)
	{
		BYTE *slabxoffs = &mip->SlabData[mip->OffsetX[x]];
		short *xyoffs = &mip->OffsetXY[x * (mip->SizeY + 1)];
		for (int y = 0; y < mip->SizeY; y++)
		{
			kvxslab_t *voxptr = (kvxslab_t *)(slabxoffs + xyoffs[y]);
			kvxslab_t *voxend = (kvxslab_t *)(slabxoffs + xyoffs[y+1]);
			for (; voxptr < voxend; voxptr = (kvxslab_t *)((BYTE *)voxptr + voxptr->zleng + 3))
			{
				MakeSlabPolys(x, y, voxptr);
			}
		}
	}
}

//===========================================================================
//
// 
//
//===========================================================================

bool FVoxelModel::Load(const char * fn, int lumpnum, const char * buffer, int length)
{
	return false;	// not needed
}

//===========================================================================
//
// 
//
//===========================================================================

void FVoxelModel::MakeGLData()
{
	// todo: create the vertex buffer
}

//===========================================================================
//
// 
//
//===========================================================================

void FVoxelModel::CleanGLData()
{
	if (mVBO != NULL)
	{
		// todo: delete the vertex buffer
		mVBO = NULL;
	}
}

//===========================================================================
//
// Voxels don't have frames so always return 0
//
//===========================================================================

int FVoxelModel::FindFrame(const char * name)
{
	return 0;
}

//===========================================================================
//
// 
//
//===========================================================================

void FVoxelModel::RenderFrame(FTexture * skin, int frame, int cm, Matrix3x4 *m2v, int translation)
{
	FMaterial * tex = FMaterial::ValidateTexture(skin);
	tex->Bind(cm, 0, translation);

	gl.Begin(GL_QUADS);
	for(unsigned i=0;i < mIndices.Size(); i++)
	{
		FVoxelVertex *vert = &mVertices[mIndices[i]];
		gl.TexCoord2f(vert->u/255.f, vert->v/255.f);
		gl.Vertex3fv(&vert->x);
	}
	gl.End();
}

//===========================================================================
//
// Voxels never interpolate between frames
//
//===========================================================================

void FVoxelModel::RenderFrameInterpolated(FTexture * skin, int frame, int frame2, double inter, int cm, Matrix3x4 *m2v, int translation)
{
	RenderFrame(skin, frame, cm, m2v, translation);
}

