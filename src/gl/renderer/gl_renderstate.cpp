/*
** gl_renderstate.cpp
** Render state maintenance
**
**---------------------------------------------------------------------------
** Copyright 2009 Christoph Oelckers
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
** 5. Full disclosure of the entire project's source code, except for third
**    party libraries is mandatory. (NOTE: This clause is non-negotiable!)
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
#include "gl/system/gl_cvars.h"
#include "gl/shaders/gl_shader.h"
#include "gl/renderer/gl_renderer.h"
#include "gl/renderer/gl_renderstate.h"
#include "gl/renderer/gl_colormap.h"

FRenderState gl_RenderState;
int FStateAttr::ChangeCounter;

//==========================================================================
//
// Set texture shader info
//
//==========================================================================

int FRenderState::SetupShader(bool cameratexture, int &shaderindex, int &cm, float warptime)
{
	bool usecmshader;
	int softwarewarp = 0;

	// fixme: move this check into shader class
	if (shaderindex == 3)
	{
		// Brightmap should not be used.
		if (!mBrightmapEnabled || cm >= CM_FIRSTSPECIALCOLORMAP)
		{
			shaderindex = 0;
		}
	}

	if (gl.shadermodel == 4)
	{
		usecmshader = cm > CM_DEFAULT && cm < CM_FIRSTSPECIALCOLORMAP + SpecialColormaps.Size() && 
			mTextureMode != TM_MASK;
	}
	else if (gl.shadermodel == 3)
	{
		usecmshader = (cameratexture || gl_colormap_shader) && 
			cm > CM_DEFAULT && cm < CM_FIRSTSPECIALCOLORMAP + SpecialColormaps.Size() && 
			mTextureMode != TM_MASK;

		if (!gl_brightmap_shader && shaderindex >= 3) 
		{
			shaderindex = 0;
		}
		else if (!gl_warp_shader && shaderindex < 3)
		{
			softwarewarp = shaderindex;
			shaderindex = 0;
		}
	}
	else
	{
		usecmshader = (gl.shadermodel == 2 && cameratexture);
		softwarewarp = shaderindex < 3? shaderindex : 0;
		shaderindex = 0;
	}

	mEffectState = shaderindex;
	mColormapState = usecmshader? cm : CM_DEFAULT;
	if (usecmshader) cm = CM_DEFAULT;
	mWarpTime = warptime;
	return softwarewarp;
}


//==========================================================================
//
// Apply shader settings
//
//==========================================================================

bool FRenderState::ApplyShader()
{
	bool useshaders = false;
	FShader *activeShader = NULL;

	if (mSpecialEffect > 0)
	{
		activeShader = GLRenderer->mShaderManager->BindEffect(mSpecialEffect);
	}
	else
	{
		switch (gl.shadermodel)
		{
		case 2:
			useshaders = (mTextureEnabled && mColormapState != CM_DEFAULT);
			break;

		case 3:
			useshaders = (
				mEffectState != 0 ||	// special shaders
				(mFogEnabled && (gl_fogmode == 2 || gl_fog_shader) && gl_fogmode != 0) || // fog requires a shader
				(mTextureEnabled && (mEffectState != 0 || mColormapState)) ||		// colormap
				mGlowEnabled		// glow requires a shader
				);
			break;

		case 4:
			useshaders = (
				mEffectState != 0 ||	// special shaders
				(mFogEnabled && gl_fogmode != 0) || // fog requires a shader
				(mTextureEnabled && mColormapState) ||	// colormap
				mGlowEnabled ||		// glow requires a shader
				mLightEnabled
				);
			break;

		default:
			break;
		}

		if (useshaders)
		{
			FShaderContainer *shd = GLRenderer->mShaderManager->Get(mTextureEnabled? mEffectState : 4);

			if (shd != NULL)
			{
				activeShader = shd->Bind(mColormapState, mGlowEnabled, mWarpTime, mLightEnabled);
			}
		}
	}

	if (activeShader)
	{
		int fogset = 0;
		if (mFogEnabled)
		{
			if ((mFogColor & 0xffffff) == 0)
			{
				fogset = gl_fogmode;
			}
			else
			{
				fogset = -gl_fogmode;
			}
		}

		if (fogset != activeShader->currentfogenabled)
		{
			gl.Uniform1i(activeShader->fogenabled_index, (activeShader->currentfogenabled = fogset)); 
		}
		if (mTextureMode != activeShader->currenttexturemode)
		{
			gl.Uniform1i(activeShader->texturemode_index, (activeShader->currenttexturemode = mTextureMode)); 
		}
		if (activeShader->currentcamerapos.Update(&mCameraPos))
		{
			gl.Uniform3fv(activeShader->camerapos_index, 1, mCameraPos.vec); 
		}
		if (mLightParms[0] != activeShader->currentlightfactor || 
			mLightParms[1] != activeShader->currentlightdist)
		{
			activeShader->currentlightdist = mLightParms[1];
			activeShader->currentlightfactor = mLightParms[0];
			gl.Uniform2fv(activeShader->lightparms_index, 1, mLightParms);
		}
		if (mFogColor != activeShader->currentfogcolor ||
			mFogDensity != activeShader->currentfogdensity)
		{
			const float LOG2E = 1.442692f;	// = 1/log(2)

			activeShader->currentfogcolor = mFogColor;
			activeShader->currentfogdensity = mFogDensity;

			// premultiply the density with as much as possible here to reduce shader
			// exection time.
			gl.Uniform4f (activeShader->fogcolor_index, mFogColor.r/255.f, mFogColor.g/255.f, 
							mFogColor.b/255.f, mFogDensity * (-LOG2E / 64000.f));
		}
		if (mGlowEnabled)
		{
			gl.Uniform4fv(activeShader->glowtopcolor_index, 1, mGlowTop.vec);
			gl.Uniform4fv(activeShader->glowbottomcolor_index, 1, mGlowBottom.vec);
		}
		return true;
	}
	return false;
}


//==========================================================================
//
// Apply State
//
//==========================================================================

void FRenderState::Apply(bool forcenoshader)
{
	if (forcenoshader || !ApplyShader())
	{
		GLRenderer->mShaderManager->SetActiveShader(NULL);
		if (mTextureMode != ffTextureMode)
		{
			gl.SetTextureMode((ffTextureMode = mTextureMode));
		}
		if (mTextureEnabled != ffTextureEnabled)
		{
			if ((ffTextureEnabled = mTextureEnabled)) gl.Enable(GL_TEXTURE_2D);
			else gl.Disable(GL_TEXTURE_2D);
		}
		if (mFogEnabled != ffFogEnabled)
		{
			if ((ffFogEnabled = mFogEnabled)) 
			{
				gl.Enable(GL_FOG);
			}
			else gl.Disable(GL_FOG);
		}
		if (mFogEnabled)
		{
			if (ffFogColor != mFogColor)
			{
				ffFogColor = mFogColor;
				GLfloat FogColor[4]={mFogColor.r/255.0f,mFogColor.g/255.0f,mFogColor.b/255.0f,0.0f};
				gl.Fogfv(GL_FOG_COLOR, FogColor);
			}
			if (ffFogDensity != mFogDensity)
			{
				gl.Fogf(GL_FOG_DENSITY, mFogDensity/64000.f);
				ffFogDensity=mFogDensity;
			}
		}
		if (mSpecialEffect != ffSpecialEffect)
		{
			switch (ffSpecialEffect)
			{
			case EFF_SPHEREMAP:
				gl.Disable(GL_TEXTURE_GEN_T);
				gl.Disable(GL_TEXTURE_GEN_S);

			default:
				break;
			}
			switch (mSpecialEffect)
			{
			case EFF_SPHEREMAP:
				// Use sphere mapping for this
				gl.Enable(GL_TEXTURE_GEN_T);
				gl.Enable(GL_TEXTURE_GEN_S);
				gl.TexGeni(GL_S,GL_TEXTURE_GEN_MODE,GL_SPHERE_MAP);
				gl.TexGeni(GL_T,GL_TEXTURE_GEN_MODE,GL_SPHERE_MAP);
				break;

			default:
				break;
			}
			ffSpecialEffect = mSpecialEffect;
		}
	}

}

