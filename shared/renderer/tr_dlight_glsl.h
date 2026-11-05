/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2013 - 2015, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

// tr_dlight_glsl.h -- the dynamic light pass as a GLSL program (r_dlightGLSL) of the single player and multiplayer
// renderers, and the choice of the stage a dynamic light goes on.
//
// Included once, by the tr_shade.cpp of each renderer, after R_DrawElements and R_BindAnimatedImage (the GLSL entry
// points come from renderer/tr_glsl.h).

#pragma once

// r_dlightGLSL 2: how deep the bumps the surface texture is read as are, in units (for the brightest texel)
#define DLIGHT_BUMP_DEPTH	1.5f

// the first opaque stage that is not a lightmap, an environment map or a fog: the surface texture that a dynamic
// light has to be multiplied with. NULL when there is none, or without multitexture.
static shaderStage_t *RB_DlightDiffuseStage( void ) {
	int i;

	if ( !tess.shader || !qglActiveTextureARB ) {
		return NULL;
	}

	for ( i = 0; i < tess.shader->numUnfoggedPasses; i++ ) {
		const int blendBits = (GLS_SRCBLEND_BITS+GLS_DSTBLEND_BITS);
		shaderStage_t *stage = &tess.shader->stages[i];

		if ( ((stage->bundle[0].image && !stage->bundle[0].isLightmap && !stage->bundle[0].numTexMods && stage->bundle[0].tcGen != TCGEN_ENVIRONMENT_MAPPED && stage->bundle[0].tcGen != TCGEN_FOG) ||
			 (stage->bundle[1].image && !stage->bundle[1].isLightmap && !stage->bundle[1].numTexMods && stage->bundle[1].tcGen != TCGEN_ENVIRONMENT_MAPPED && stage->bundle[1].tcGen != TCGEN_FOG)) &&
			(stage->stateBits & blendBits) == 0 )
		{ //only use non-lightmap opaque stages
			return stage;
		}
	}
	return NULL;
}

/*
===================
ProjectDlightGLSL

Dynamic lights as a shader pass: every pixel of a lit surface is lit by its own distance to the light and by the
angle between its (interpolated) normal and the light, where the passes above light every triangle as a flat
plane. The result is added in the same way as the passes above, so the look stays close to the original, the
surface just gets the shading of its shape. Returns qfalse, having touched nothing, when the program cannot be
built, so that the caller uses the old passes.
===================
*/
static const char *dlightVertexSource =
	"#version 120\n"
	"varying vec3 vPos;\n"
	"varying vec3 vNormal;\n"
	"void main() {\n"
	"	vPos = gl_Vertex.xyz;\n"
	"	vNormal = gl_Normal;\n"
	"	gl_TexCoord[0] = gl_MultiTexCoord0;\n"
	"	gl_Position = ftransform();\n"
	"}\n";

static const char *dlightFragmentSource =
	"#version 120\n"
	"uniform sampler2D uDiffuse;\n"
	"uniform vec4 uLight;\n"		// xyz: position in the space of the surface, w: radius
	"uniform vec3 uColor;\n"
	"uniform float uTextured;\n"
	"uniform float uAlphaTest;\n"	// 0 none, 1 alpha > 0, 2 alpha < 0.5, 3 alpha >= 0.5, 4 alpha >= 0.75, as the stage the light goes on
	"uniform float uBump;\n"		// how deep the bumps of the surface are (r_dlightGLSL 2), in units for the brightest texel, 0 flat
	"varying vec3 vPos;\n"
	"varying vec3 vNormal;\n"
	"float height( vec2 st ) { return dot( texture2D( uDiffuse, st ).rgb, vec3( 0.299, 0.587, 0.114 ) ) * uBump; }\n"
	"void main() {\n"
	"	vec3 toLight = uLight.xyz - vPos;\n"
	"	float dist2 = dot( toLight, toLight );\n"
	"	float atten = clamp( 1.0 - dist2 / ( uLight.w * uLight.w ), 0.0, 1.0 );\n"
	"	float facing = 1.0;\n"
	"	float nlen2 = dot( vNormal, vNormal );\n"
	// the bumps: the surface texture read as a height, its slope across the pixel tilting the normal (the surface
	// gradient of Mikkelsen, "Bump Mapping Unparametrized Surfaces on the GPU", 2010: no tangents needed)
	"	vec2 st = gl_TexCoord[0].st;\n"
	"	vec2 stX = dFdx( st ), stY = dFdy( st );\n"
	"	vec3 posX = dFdx( vPos ), posY = dFdy( vPos );\n"
	"	float h = 0.0, hX = 0.0, hY = 0.0;\n"
	"	if ( uBump > 0.0 && uTextured > 0.5 ) {\n"	// (read outside the branch below, which can differ between pixels)
	"		h = height( st ); hX = height( st + stX ) - h; hY = height( st + stY ) - h;\n"
	"	}\n"
	"	if ( nlen2 > 0.0001 ) {\n"	// the terrain has no normals, it is lit like the old passes did
	"		vec3 n = vNormal * inversesqrt( nlen2 );\n"
	"		vec3 r1 = cross( posY, n ), r2 = cross( n, posX );\n"
	"		float det = dot( posX, r1 );\n"
	"		vec3 bumpedN = normalize( max( abs( det ), 1e-8 ) * n - sign( det ) * ( hX * r1 + hY * r2 ) );\n"
	"		float ndl = dot( n, toLight ) * inversesqrt( max( dist2, 0.0001 ) );\n"
	"		float bumped = dot( bumpedN, toLight ) * inversesqrt( max( dist2, 0.0001 ) );\n"
	"		facing = smoothstep( 0.0, 0.25, ndl ) * mix( 1.0, max( bumped, 0.0 ), 0.6 );\n"
	"	}\n"
	"	vec3 light = uColor * ( atten * facing );\n"
	"	if ( uTextured > 0.5 ) {\n"
	"		vec4 diffuse = texture2D( uDiffuse, gl_TexCoord[0].st );\n"
	"		if ( ( uAlphaTest > 0.5 && uAlphaTest < 1.5 && diffuse.a <= 0.0 ) ||\n"
	"			 ( uAlphaTest > 1.5 && uAlphaTest < 2.5 && diffuse.a >= 0.5 ) ||\n"
	"			 ( uAlphaTest > 2.5 && uAlphaTest < 3.5 && diffuse.a < 0.5 ) ||\n"
	"			 ( uAlphaTest > 3.5 && diffuse.a < 0.75 ) ) {\n"
	"			discard;\n"	// the stage is cut out here, so it gets no light here
	"		}\n"
	"		light *= diffuse.rgb;\n"
	"	}\n"
	"	gl_FragColor = vec4( light, 1.0 );\n"
	"}\n";

// The program, built when first needed, for the GL context of R_GLSL_Generation (it used to be kept in tr, which is
// cleared at every level load while the context stays, so it was built again, and the old one leaked, every time).
// failed stops a build that failed from being tried again for every surface.
static struct {
	int		generation;
	GLuint	program;
	GLint	uniforms[6];
	qboolean	failed;
} dlightGlsl;

static qboolean ProjectDlightGLSL( void ) {
	int				l, i;
	glIndex_t		hitIndexes[SHADER_MAX_INDEXES];
	shaderStage_t	*dStage;

	if ( !backEnd.refdef.num_dlights ) {
		return qtrue;
	}

	if ( dlightGlsl.generation != R_GLSL_Generation() ) {
		memset( &dlightGlsl, 0, sizeof( dlightGlsl ) );	// a new GL context, the old program went with the old one
		dlightGlsl.generation = R_GLSL_Generation();
	}
	if ( !dlightGlsl.program ) {
		if ( dlightGlsl.failed ) {
			return qfalse;
		}
		dlightGlsl.program = R_GLSL_BuildProgram( "dlight", dlightVertexSource, dlightFragmentSource );
		if ( !dlightGlsl.program ) {
			dlightGlsl.failed = qtrue;
			return qfalse;
		}
		dlightGlsl.uniforms[0] = qglGetUniformLocation( dlightGlsl.program, "uDiffuse" );
		dlightGlsl.uniforms[1] = qglGetUniformLocation( dlightGlsl.program, "uLight" );
		dlightGlsl.uniforms[2] = qglGetUniformLocation( dlightGlsl.program, "uColor" );
		dlightGlsl.uniforms[3] = qglGetUniformLocation( dlightGlsl.program, "uTextured" );
		dlightGlsl.uniforms[4] = qglGetUniformLocation( dlightGlsl.program, "uAlphaTest" );
		dlightGlsl.uniforms[5] = qglGetUniformLocation( dlightGlsl.program, "uBump" );
	}

	dStage = RB_DlightDiffuseStage();

	GL_SelectTexture( 0 );
	if ( dStage ) {
		// with the texture the light is added to the surface colour, without it the light is multiplied in
		if ( dStage->bundle[0].image && !dStage->bundle[0].isLightmap && !dStage->bundle[0].numTexMods && dStage->bundle[0].tcGen != TCGEN_ENVIRONMENT_MAPPED && dStage->bundle[0].tcGen != TCGEN_FOG ) {
			R_BindAnimatedImage( &dStage->bundle[0] );
		} else {
			R_BindAnimatedImage( &dStage->bundle[1] );
		}
		qglTexCoordPointer( 2, GL_FLOAT, sizeof( tess.texCoords[0] ), tess.texCoords[0][0] );
		GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );
	} else {
		GL_State( GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ONE );
	}

	// The passes above draw with GLS_DEPTHFUNC_EQUAL, which only works while the position of a vertex comes out of
	// the pipeline in exactly the same way every time. A shader does not promise that, so this pass draws on top of
	// the depth of the surface with a small offset towards the viewer instead. Where the stage is cut out by an
	// alpha test the shader does the same cut, so that the light does not land on what is behind it.
	qglEnable( GL_POLYGON_OFFSET_FILL );
	if ( tess.shader->polygonOffset ) {
		qglPolygonOffset( r_offsetFactor->value - 1.0f, r_offsetUnits->value - 2.0f );	// a decal already has its own
	} else {
		qglPolygonOffset( -1.0f, -2.0f );
	}

	{
		float alphaTest = 0.0f;
		if ( dStage ) {
			switch ( dStage->stateBits & GLS_ATEST_BITS ) {
			case GLS_ATEST_GT_0:	alphaTest = 1.0f;	break;
			case GLS_ATEST_LT_80:	alphaTest = 2.0f;	break;
			case GLS_ATEST_GE_80:	alphaTest = 3.0f;	break;
			case GLS_ATEST_GE_C0:	alphaTest = 4.0f;	break;
			}
		}
		qglUseProgram( dlightGlsl.program );
		qglUniform1i( dlightGlsl.uniforms[0], 0 );
		qglUniform1f( dlightGlsl.uniforms[3], dStage ? 1.0f : 0.0f );
		qglUniform1f( dlightGlsl.uniforms[4], alphaTest );
		qglUniform1f( dlightGlsl.uniforms[5], ( r_dlightGLSL->integer >= 2 ) ? DLIGHT_BUMP_DEPTH : 0.0f );
	}
	qglEnableClientState( GL_NORMAL_ARRAY );
	qglNormalPointer( GL_FLOAT, sizeof( tess.normal[0] ), tess.normal );

	for ( l = 0; l < backEnd.refdef.num_dlights; l++ ) {
		const dlight_t	*dl;
		int				numIndexes = 0;

		if ( !( tess.dlightBits & ( 1 << l ) ) ) {
			continue;	// this surface definately doesn't have any of this light
		}

		dl = &backEnd.refdef.dlights[l];

		// the triangles whose box, grown by the radius, holds the light
		for ( i = 0; i < tess.numIndexes; i += 3 ) {
			const float	*v[3] = { tess.xyz[tess.indexes[i]], tess.xyz[tess.indexes[i + 1]], tess.xyz[tess.indexes[i + 2]] };
			int			k;

			for ( k = 0; k < 3; k++ ) {
				const float lo = Q_min( v[0][k], Q_min( v[1][k], v[2][k] ) );
				const float hi = Q_max( v[0][k], Q_max( v[1][k], v[2][k] ) );
				if ( dl->transformed[k] + dl->radius < lo || dl->transformed[k] - dl->radius > hi ) {
					break;
				}
			}
			if ( k < 3 ) {
				continue;
			}
			hitIndexes[numIndexes++] = tess.indexes[i];
			hitIndexes[numIndexes++] = tess.indexes[i + 1];
			hitIndexes[numIndexes++] = tess.indexes[i + 2];
		}

		if ( !numIndexes ) {
			continue;
		}

		const float		light[4] = { dl->transformed[0], dl->transformed[1], dl->transformed[2], dl->radius };
		qglUniform4fv( dlightGlsl.uniforms[1], 1, light );
		qglUniform3f( dlightGlsl.uniforms[2], dl->color[0], dl->color[1], dl->color[2] );
		R_DrawElements( numIndexes, hitIndexes );

		backEnd.pc.c_totalIndexes += numIndexes;
		backEnd.pc.c_dlightIndexes += numIndexes;
	}

	qglDisableClientState( GL_NORMAL_ARRAY );
	qglUseProgram( 0 );
	if ( tess.shader->polygonOffset ) {
		qglPolygonOffset( r_offsetFactor->value, r_offsetUnits->value );
	} else {
		qglDisable( GL_POLYGON_OFFSET_FILL );
	}
	return qtrue;
}
