/*
** gl_shader.cpp
**
** GLSL shader handling
**
**---------------------------------------------------------------------------
** Copyright 2004-2005 Christoph Oelckers
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
#include "c_cvars.h"
#include "v_video.h"
#include "name.h"
#include "w_wad.h"
#include "i_system.h"
#include "doomerrors.h"
#include "v_palette.h"

#include "gl/renderer/gl_renderer.h"
#include "gl/renderer/gl_renderstate.h"
#include "gl/system/gl_cvars.h"
#include "gl/shaders/gl_shader.h"
#include "gl/textures/gl_material.h"

// these will only have an effect on SM3 cards.
// For SM4 they are always on and for SM2 always off
CVAR(Bool, gl_warp_shader, false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG|CVAR_NOINITCALL)
CVAR(Bool, gl_fog_shader, false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG|CVAR_NOINITCALL)
CVAR(Bool, gl_colormap_shader, false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG|CVAR_NOINITCALL)
CVAR(Bool, gl_brightmap_shader, false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG|CVAR_NOINITCALL)
CVAR(Bool, gl_glow_shader, true, CVAR_ARCHIVE|CVAR_GLOBALCONFIG|CVAR_NOINITCALL)


extern long gl_frameMS;

//==========================================================================
//
//
//
//==========================================================================

bool FShader::Load(const char * name, const char * vert_prog_lump, const char * frag_prog_lump, const char * proc_prog_lump, const char * defines)
{
	static char buffer[10000];

	if (gl.shadermodel > 0)
	{
		int vp_lump = Wads.CheckNumForFullName(vert_prog_lump);
		if (vp_lump == -1) I_Error("Unable to load '%s'", vert_prog_lump);
		FMemLump vp_data = Wads.ReadLump(vp_lump);

		int fp_lump = Wads.CheckNumForFullName(frag_prog_lump);
		if (fp_lump == -1) I_Error("Unable to load '%s'", frag_prog_lump);
		FMemLump fp_data = Wads.ReadLump(fp_lump);


		FString fp_comb;
		if (gl.shadermodel < 4) fp_comb = "#define NO_SM4\n";
		// This uses GetChars on the strings to get rid of terminating 0 characters.
		fp_comb << defines << fp_data.GetString().GetChars() << "\n";

		if (proc_prog_lump != NULL)
		{
			int pp_lump = Wads.CheckNumForFullName(proc_prog_lump);
			if (pp_lump == -1) I_Error("Unable to load '%s'", proc_prog_lump);
			FMemLump pp_data = Wads.ReadLump(pp_lump);

			fp_comb << pp_data.GetString().GetChars();
		}

		hVertProg = gl.CreateShader(GL_VERTEX_SHADER);
		hFragProg = gl.CreateShader(GL_FRAGMENT_SHADER);	


		int vp_size = (int)vp_data.GetSize();
		int fp_size = (int)fp_comb.Len();

		const char *vp_ptr = vp_data.GetString().GetChars();
		const char *fp_ptr = fp_comb.GetChars();

		gl.ShaderSource(hVertProg, 1, &vp_ptr, &vp_size);
		gl.ShaderSource(hFragProg, 1, &fp_ptr, &fp_size);

		gl.CompileShader(hVertProg);
		gl.CompileShader(hFragProg);

		hShader = gl.CreateProgram();

		gl.AttachShader(hShader, hVertProg);
		gl.AttachShader(hShader, hFragProg);

		gl.BindAttribLocation(hShader, VATTR_GLOWDISTANCE, "glowdistance");

		gl.LinkProgram(hShader);
	
		gl.GetProgramInfoLog(hShader, 10000, NULL, buffer);
		if (*buffer) 
		{
			Printf("Init Shader '%s':\n%s\n", name, buffer);
		}
		int linked;
		gl.GetObjectParameteriv(hShader, GL_LINK_STATUS, &linked);
		timer_index = gl.GetUniformLocation(hShader, "timer");
		desaturation_index = gl.GetUniformLocation(hShader, "desaturation_factor");
		fogenabled_index = gl.GetUniformLocation(hShader, "fogenabled");
		texturemode_index = gl.GetUniformLocation(hShader, "texturemode");
		camerapos_index = gl.GetUniformLocation(hShader, "camerapos");
		lightparms_index = gl.GetUniformLocation(hShader, "lightparms");
		colormapstart_index = gl.GetUniformLocation(hShader, "colormapstart");
		colormaprange_index = gl.GetUniformLocation(hShader, "colormaprange");
		lightrange_index = gl.GetUniformLocation(hShader, "lightrange");
		fogcolor_index = gl.GetUniformLocation(hShader, "fogcolor");

		glowbottomcolor_index = gl.GetUniformLocation(hShader, "bottomglowcolor");
		glowtopcolor_index = gl.GetUniformLocation(hShader, "topglowcolor");
		
		gl.UseProgram(hShader);

		int texture_index = gl.GetUniformLocation(hShader, "texture2");
		if (texture_index > 0) gl.Uniform1i(texture_index, 1);

		texture_index = gl.GetUniformLocation(hShader, "lightIndex");
		if (texture_index > 0) gl.Uniform1i(texture_index, 13);
		texture_index = gl.GetUniformLocation(hShader, "lightRGB");
		if (texture_index > 0) gl.Uniform1i(texture_index, 14);
		texture_index = gl.GetUniformLocation(hShader, "lightPositions");
		if (texture_index > 0) gl.Uniform1i(texture_index, 15);

		gl.UseProgram(0);
		return !!linked;
	}
	return false;
}

//==========================================================================
//
//
//
//==========================================================================

FShader::~FShader()
{
	gl.DeleteProgram(hShader);
	gl.DeleteShader(hVertProg);
	gl.DeleteShader(hFragProg);
}


//==========================================================================
//
//
//
//==========================================================================

bool FShader::Bind(float Speed)
{
	GLRenderer->mShaderManager->SetActiveShader(this);
	if (timer_index >=0 && Speed > 0.f) gl.Uniform1f(timer_index, gl_frameMS*Speed/1000.f);
	return true;
}

//==========================================================================
//
//
//
//==========================================================================

FShaderContainer::FShaderContainer(const char *ShaderName, const char *ShaderPath)
{
	const char * shaderdefines[] = {
		"#define NO_GLOW\n#define NO_DESATURATE\n",
		"#define NO_DESATURATE\n",
		"#define NO_GLOW\n",
		"\n",
		"#define NO_GLOW\n#define NO_DESATURATE\n#define DYNLIGHT\n",
		"#define NO_DESATURATE\n#define DYNLIGHT\n",
		"#define NO_GLOW\n#define DYNLIGHT\n",
		"\n#define DYNLIGHT\n"
	};

	const char * shaderdesc[] = {
		"::default",
		"::glow",
		"::desaturate",
		"::glow+desaturate",
		"::default+dynlight",
		"::glow+dynlight",
		"::desaturate+dynlight",
		"::glow+desaturate+dynlight",
	};

	FString name;

	name << ShaderName << "::colormap";

	try
	{
		shader_cm = new FShader;
		if (!shader_cm->Load(name, "shaders/glsl/main_colormap.vp", "shaders/glsl/main_colormap.fp", ShaderPath, "\n"))
		{
			delete shader_cm;
			shader_cm = NULL;
		}
	}
	catch(CRecoverableError &err)
	{
		shader_cm = NULL;
		Printf("Unable to load shader %s:\n%s\n", name.GetChars(), err.GetMessage());
		I_Error("");
	}

	if (gl.shadermodel > 2)
	{
		for(int i = 0;i < NUM_SHADERS; i++)
		{
			FString name;

			name << ShaderName << shaderdesc[i];

			try
			{
				shader[i] = new FShader;
				if (!shader[i]->Load(name, "shaders/glsl/main.vp", "shaders/glsl/main.fp", ShaderPath, shaderdefines[i]))
				{
					delete shader[i];
					shader[i] = NULL;
				}
			}
			catch(CRecoverableError &err)
			{
				shader[i] = NULL;
				Printf("Unable to load shader %s:\n%s\n", name.GetChars(), err.GetMessage());
				I_Error("");
			}
			if (i==3 && !(gl.flags & RFL_TEXTUREBUFFER))
			{
				shader[4] = shader[5] = shader[6] = shader[7] = 0;
				break;
			}
		}
	}
	else memset(shader, 0, sizeof(shader));
}

//==========================================================================
//
//
//
//==========================================================================
FShaderContainer::~FShaderContainer()
{
	delete shader_cm;
	for(int i = 0;i < NUM_SHADERS; i++)
	{
		if (shader[i] != NULL)
		{
			delete shader[i];
			shader[i] = NULL;
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

FShader *FShaderContainer::Bind(int cm, bool glowing, float Speed, bool lights)
{
	FShader *sh=NULL;

	if (cm >= CM_FIRSTSPECIALCOLORMAP && cm < CM_FIRSTSPECIALCOLORMAP + SpecialColormaps.Size())
	{
		// these are never used with any kind of lighting or fog
		sh = shader_cm;
		// [BB] If there was a problem when loading the shader, sh is NULL here.
		if( sh )
		{
			FSpecialColormap *map = &SpecialColormaps[cm - CM_FIRSTSPECIALCOLORMAP];
			sh->Bind(Speed);
			float m[3]= {map->ColorizeEnd[0] - map->ColorizeStart[0], 
				map->ColorizeEnd[1] - map->ColorizeStart[1], map->ColorizeEnd[2] - map->ColorizeStart[2]};

			gl.Uniform3fv(sh->colormapstart_index, 1, map->ColorizeStart);
			gl.Uniform3fv(sh->colormaprange_index, 1, m);
		}
	}
	else
	{
		bool desat = cm>=CM_DESAT1 && cm<=CM_DESAT31;
		sh = shader[glowing + 2*desat + 4*lights];
		// [BB] If there was a problem when loading the shader, sh is NULL here.
		if( sh )
		{
			sh->Bind(Speed);
			if (desat)
			{
				gl.Uniform1f(sh->desaturation_index, 1.f-float(cm-CM_DESAT0)/(CM_DESAT31-CM_DESAT0));
			}
		}
	}
	return sh;
}


//==========================================================================
//
//
//
//==========================================================================
struct FDefaultShader 
{
	const char * ShaderName;
	const char * gettexelfunc;
};

static const FDefaultShader defaultshaders[]=
{	
	{"Default",	"shaders/glsl/func_normal.fp"},
	{"Warp 1",	"shaders/glsl/func_warp1.fp"},
	{"Warp 2",	"shaders/glsl/func_warp2.fp"},
	{"Brightmap","shaders/glsl/func_brightmap.fp"},
	{"No Texture", "shaders/glsl/func_notexture.fp"},
	{NULL,NULL}
	
};

struct FEffectShader
{
	const char *ShaderName;
	const char *vp;
	const char *fp1;
	const char *fp2;
	const char *defines;
};

static const FEffectShader effectshaders[]=
{
	{"fogboundary", "shaders/glsl/main.vp", "shaders/glsl/fogboundary.fp", NULL, "#define NO_GLOW\n"},
	{"spheremap", "shaders/glsl/main_spheremap.vp", "shaders/glsl/main.fp", "shaders/glsl/func_normal.fp", "#define NO_GLOW\n#define NO_DESATURATE\n"}
};


//==========================================================================
//
//
//
//==========================================================================

FShaderManager::FShaderManager()
{
	mActiveShader = mEffectShaders[0] = mEffectShaders[1] = NULL;
	if (gl.shadermodel > 0)
	{
		for(int i=0;defaultshaders[i].ShaderName != NULL;i++)
		{
			FShaderContainer * shc = new FShaderContainer(defaultshaders[i].ShaderName, defaultshaders[i].gettexelfunc);
			mTextureEffects.Push(shc);
			if (gl.shadermodel <= 2) return;	// SM2 will only initialize the default shader
		}

		for(int i=0;i<NUM_EFFECTS;i++)
		{
			FShader *eff = new FShader();
			if (!eff->Load(effectshaders[i].ShaderName, effectshaders[i].vp, effectshaders[i].fp1,
							effectshaders[i].fp2, effectshaders[i].defines))
			{
				delete eff;
			}
			else mEffectShaders[i] = eff;
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

FShaderManager::~FShaderManager()
{
	SetActiveShader(NULL);
	for(unsigned int i=0;i<mTextureEffects.Size();i++)
	{
		if (mTextureEffects[i] != NULL) delete mTextureEffects[i];
	}
	for(int i=0;i<NUM_EFFECTS;i++)
	{
		if (mEffectShaders[i] != NULL) delete mEffectShaders[i];
	}
}

//==========================================================================
//
//
//
//==========================================================================

int FShaderManager::Find(const char * shn)
{
	FName sfn = shn;

	for(unsigned int i=0;i<mTextureEffects.Size();i++)
	{
		if (mTextureEffects[i]->Name == sfn)
		{
			return i;
		}
	}
	return -1;
}

//==========================================================================
//
//
//
//==========================================================================

void FShaderManager::SetActiveShader(FShader *sh)
{
	// shadermodel needs to be tested here because without it UseProgram will be NULL.
	if (gl.shadermodel > 0 && mActiveShader != sh)
	{
		gl.UseProgram(sh == NULL? 0 : sh->GetHandle());
		mActiveShader = sh;
	}
}

//==========================================================================
//
//
//
//==========================================================================

FShader *FShaderManager::BindEffect(int effect)
{
	if (effect > 0 && effect <= NUM_EFFECTS)
	{
		mEffectShaders[effect-1]->Bind(0);
		return mEffectShaders[effect-1];
	}
	return NULL;
}

//==========================================================================
//
// Dynamic light stuff
//
//==========================================================================

/*
void FShader::SetLightRange(int start, int end, int forceadd)
{
	gl.Uniform3i(lightrange_index, start, end, forceadd);
}

void gl_SetLightRange(int first, int last, int forceadd)
{
	if (gl_activeShader) gl_activeShader->SetLightRange(first, last, forceadd);
}
*/


