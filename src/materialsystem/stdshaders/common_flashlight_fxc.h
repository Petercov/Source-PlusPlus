//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Common pixel shader code specific to flashlights
//
// $NoKeywords: $
//
//=============================================================================//
#ifndef COMMON_FLASHLIGHT_FXC_H_
#define COMMON_FLASHLIGHT_FXC_H_

#include "common_ps_fxc.h"

#if SHADER_MODEL_PS_3_0
#define NEW_SHADOW_FILTERS // Comment if you want to enable retail shadow filter.

#define	PSREG_UBERLIGHT_SMOOTH_EDGE_0			c33
#define	PSREG_UBERLIGHT_SMOOTH_EDGE_1			c34
#define	PSREG_UBERLIGHT_SMOOTH_EDGE_OOW			c35
#define	PSREG_UBERLIGHT_SHEAR_ROUND				c36
#define	PSREG_UBERLIGHT_AABB					c37
#define PSREG_UBERLIGHT_WORLD_TO_LIGHT			c38
//		PSREG_UBERLIGHT_WORLD_TO_LIGHT			c39
//		PSREG_UBERLIGHT_WORLD_TO_LIGHT			c40
//		PSREG_UBERLIGHT_WORLD_TO_LIGHT			c41

// Vectorized smoothstep for doing three smoothsteps at once.  Used by uberlight
float3 smoothstep3( float3 edge0, float3 edge1, float3 OneOverWidth, float3 x )
{
	x = saturate((x - edge0) * OneOverWidth);	// Scale, bias and saturate x to the range of zero to one
	return x*x*(3-2*x);							// Evaluate polynomial
}

// Superellipse soft clipping
//
// Input:
//   - Point Q on the x-y plane
//   - The equations of two superellipses (with major/minor axes given by
//     a,b and A,B for the inner and outer ellipses, respectively)
//   - This is changed a bit from the original RenderMan code to be better vectorized
//
// Return value:
//   - 0 if Q was inside the inner ellipse
//   - 1 if Q was outside the outer ellipse
//   - smoothly varying from 0 to 1 in between
float2 ClipSuperellipse( float2 Q,			// Point on the xy plane
						 float4 aAbB,		// Dimensions of superellipses
						 float2 rounds )	// Same roundness for both ellipses
{
	float2 qr, Qabs = abs(Q);				// Project to +x +y quadrant

	float2 bx_Bx = Qabs.x * aAbB.zw;
	float2 ay_Ay = Qabs.y * aAbB.xy;

	qr.x = pow( pow(bx_Bx.x, rounds.x) + pow(ay_Ay.x, rounds.x), rounds.y );  // rounds.x = 2 / roundness
	qr.y = pow( pow(bx_Bx.y, rounds.x) + pow(ay_Ay.y, rounds.x), rounds.y );  // rounds.y = -roundness/2

	return qr * aAbB.xy * aAbB.zw;
}

// Volumetric light shaping
//
// Inputs:
//   - the point being shaded, in the local light space
//   - all information about the light shaping, including z smooth depth
//     clipping, superellipse xy shaping, and distance falloff.
// Return value:
//   - attenuation factor based on the falloff and shaping
float uberlight(float3 PL,					// Point in light space

				float3 smoothEdge0,			// edge0 for three smooth steps
				float3 smoothEdge1,			// edge1 for three smooth steps
				float3 smoothOneOverWidth,	// width of three smooth steps

				float2 shear,				// shear in X and Y
				float4 aAbB,				// Superellipse dimensions
				float2 rounds )				// two functions of roundness packed together
{
	float2 qr = ClipSuperellipse( (PL / PL.z) - shear, aAbB, rounds );

	smoothEdge0.x = qr.x;					// Fill in the dynamic parts of the smoothsteps
	smoothEdge1.x = qr.y;					// The other components are pre-computed outside of the shader
	smoothOneOverWidth.x = 1.0f / ( qr.y - qr.x );
	float3 x = float3( 1, PL.z, PL.z );

	float3 atten3 = smoothstep3( smoothEdge0, smoothEdge1, smoothOneOverWidth, x );

	// Modulate the three resulting attenuations (flipping the sense of the attenuation from the superellipse and the far clip)
	return (1.0f - atten3.x) * atten3.y * (1.0f - atten3.z);
}

#endif

// JasonM - TODO: remove this simpleton version
float DoShadow( sampler DepthSampler, float4 texCoord )
{
	const float g_flShadowBias = 0.0005f;
	float2 uoffset = float2( 0.5f/512.f, 0.0f );
	float2 voffset = float2( 0.0f, 0.5f/512.f );
	float3 projTexCoord = texCoord.xyz / texCoord.w;
	float4 flashlightDepth = float4(	tex2D( DepthSampler, projTexCoord.xy + uoffset + voffset ).x,
										tex2D( DepthSampler, projTexCoord.xy + uoffset - voffset ).x,
										tex2D( DepthSampler, projTexCoord.xy - uoffset + voffset ).x,
										tex2D( DepthSampler, projTexCoord.xy - uoffset - voffset ).x	);

#	if ( defined( REVERSE_DEPTH_ON_X360 ) )
	{
		flashlightDepth = 1.0f - flashlightDepth;
	}
#	endif

	float shadowed = 0.0f;
	float z = texCoord.z/texCoord.w;
	float4 dz = float4(z,z,z,z) - (flashlightDepth + float4( g_flShadowBias, g_flShadowBias, g_flShadowBias, g_flShadowBias));
	float4 shadow = float4(0.25f,0.25f,0.25f,0.25f);

	if( dz.x <= 0.0f )
		shadowed += shadow.x;
	if( dz.y <= 0.0f )
		shadowed += shadow.y;
	if( dz.z <= 0.0f )
		shadowed += shadow.z;
	if( dz.w <= 0.0f )
		shadowed += shadow.w;

	return shadowed;
}


float DoShadowNvidiaRAWZOneTap( sampler DepthSampler, const float4 shadowMapPos )
{
	float ooW = 1.0f / shadowMapPos.w;								// 1 / w
	float3 shadowMapCenter_objDepth = shadowMapPos.xyz * ooW;		// Do both projections at once

	float2 shadowMapCenter = shadowMapCenter_objDepth.xy;			// Center of shadow filter
	float objDepth = shadowMapCenter_objDepth.z;					// Object depth in shadow space

	float fDepth = dot(tex2D(DepthSampler, shadowMapCenter).arg, float3(0.996093809371817670572857294849, 0.0038909914428586627756752238080039, 1.5199185323666651467481343000015e-5));

	return fDepth > objDepth;
}


float DoShadowNvidiaRAWZ( sampler DepthSampler, const float4 shadowMapPos )
{
	float fE = 1.0f / 512.0f;	 // Epsilon

	float ooW = 1.0f / shadowMapPos.w;								// 1 / w
	float3 shadowMapCenter_objDepth = shadowMapPos.xyz * ooW;		// Do both projections at once

	float2 shadowMapCenter = shadowMapCenter_objDepth.xy;			// Center of shadow filter
	float objDepth = shadowMapCenter_objDepth.z;					// Object depth in shadow space

	float4 vDepths;
	vDepths.x = dot(tex2D(DepthSampler, shadowMapCenter + float2(  fE,  fE )).arg, float3(0.996093809371817670572857294849, 0.0038909914428586627756752238080039, 1.5199185323666651467481343000015e-5));
	vDepths.y = dot(tex2D(DepthSampler, shadowMapCenter + float2( -fE,  fE )).arg, float3(0.996093809371817670572857294849, 0.0038909914428586627756752238080039, 1.5199185323666651467481343000015e-5));
	vDepths.z = dot(tex2D(DepthSampler, shadowMapCenter + float2(  fE, -fE )).arg, float3(0.996093809371817670572857294849, 0.0038909914428586627756752238080039, 1.5199185323666651467481343000015e-5));
	vDepths.w = dot(tex2D(DepthSampler, shadowMapCenter + float2( -fE, -fE )).arg, float3(0.996093809371817670572857294849, 0.0038909914428586627756752238080039, 1.5199185323666651467481343000015e-5));

	return dot(vDepths > objDepth.xxxx, float4(0.25, 0.25, 0.25, 0.25));
}


float DoShadowNvidiaCheap( sampler DepthSampler, const float4 shadowMapPos )
{
	float fTexelEpsilon = 1.0f / 1024.0f;

	float ooW = 1.0f / shadowMapPos.w;								// 1 / w
	float3 shadowMapCenter_objDepth = shadowMapPos.xyz * ooW;		// Do both projections at once

	float2 shadowMapCenter = shadowMapCenter_objDepth.xy;			// Center of shadow filter
	float objDepth = shadowMapCenter_objDepth.z;					// Object depth in shadow space

	float4 vTaps;
	vTaps.x = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2(  fTexelEpsilon,  fTexelEpsilon), objDepth, 1 ) ).x;
	vTaps.y = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2( -fTexelEpsilon,  fTexelEpsilon), objDepth, 1 ) ).x;
	vTaps.z = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2(  fTexelEpsilon, -fTexelEpsilon), objDepth, 1 ) ).x;
	vTaps.w = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2( -fTexelEpsilon, -fTexelEpsilon), objDepth, 1 ) ).x;

	return dot(vTaps, float4(0.25, 0.25, 0.25, 0.25));
}

#if defined( NEW_SHADOW_FILTERS )
float DoShadowNvidiaPCF3x3Box( sampler DepthSampler, const float3 shadowMapPos )
#else
float DoShadowNvidiaPCF3x3Box( sampler DepthSampler, const float4 shadowMapPos )
#endif
{
	float fTexelEpsilon = 1.0f / 1024.0f;

#if !defined( NEW_SHADOW_FILTERS )
	float ooW = 1.0f / shadowMapPos.w;								// 1 / w
	float3 shadowMapCenter_objDepth = shadowMapPos.xyz * ooW;		// Do both projections at once
#else
	float3 shadowMapCenter_objDepth = shadowMapPos.xyz;
#endif
	float2 shadowMapCenter = shadowMapCenter_objDepth.xy;			// Center of shadow filter
	float objDepth = shadowMapCenter_objDepth.z;					// Object depth in shadow space

	float4 vOneTaps;
	vOneTaps.x = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2(  fTexelEpsilon,  fTexelEpsilon ), objDepth, 1 ) ).x;
	vOneTaps.y = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2( -fTexelEpsilon,  fTexelEpsilon ), objDepth, 1 ) ).x;
	vOneTaps.z = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2(  fTexelEpsilon, -fTexelEpsilon ), objDepth, 1 ) ).x;
	vOneTaps.w = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2( -fTexelEpsilon, -fTexelEpsilon ), objDepth, 1 ) ).x;
	float flOneTaps = dot( vOneTaps, float4(1.0f / 9.0f, 1.0f / 9.0f, 1.0f / 9.0f, 1.0f / 9.0f));

	float4 vTwoTaps;
	vTwoTaps.x = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2(  fTexelEpsilon,  0 ), objDepth, 1 ) ).x;
	vTwoTaps.y = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2( -fTexelEpsilon,  0 ), objDepth, 1 ) ).x;
	vTwoTaps.z = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2(  0, -fTexelEpsilon ), objDepth, 1 ) ).x;
	vTwoTaps.w = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2(  0, -fTexelEpsilon ), objDepth, 1 ) ).x;
	float flTwoTaps = dot( vTwoTaps, float4(1.0f / 9.0f, 1.0f / 9.0f, 1.0f / 9.0f, 1.0f / 9.0f));

	float flCenterTap = tex2Dproj( DepthSampler, float4( shadowMapCenter, objDepth, 1 ) ).x * (1.0f / 9.0f);

	// Sum all 9 Taps
	return flOneTaps + flTwoTaps + flCenterTap;
}


//
//	1	4	7	4	1
//	4	20	33	20	4
//	7	33	55	33	7
//	4	20	33	20	4
//	1	4	7	4	1
//
#if defined( NEW_SHADOW_FILTERS )
float DoShadowNvidiaPCF5x5Gaussian( sampler DepthSampler, const float3 shadowMapPos, const float2 vShadowTweaks )
#else
float DoShadowNvidiaPCF5x5Gaussian( sampler DepthSampler, const float4 shadowMapPos )
#endif
{
#if defined( NEW_SHADOW_FILTERS )
	float fEpsilonX    = vShadowTweaks.x;
	float fTwoEpsilonX = 2.0f * fEpsilonX;
	float fEpsilonY    = vShadowTweaks.y;
	float fTwoEpsilonY = 2.0f * fEpsilonY;
#else
	float fEpsilonX    = 1.0 / 512.0;
	float fTwoEpsilonX = 2.0f * fEpsilonX;
	float fEpsilonY    = fEpsilonX;
	float fTwoEpsilonY = fTwoEpsilonX;
#endif

#if defined( NEW_SHADOW_FILTERS )	
	float3 shadowMapCenter_objDepth = shadowMapPos;					// Do both projections at once
#else
	float3 shadowMapCenter_objDepth = shadowMapPos.xyz/ shadowMapPos.w;		// Do both projections at once
#endif

	float2 shadowMapCenter = shadowMapCenter_objDepth.xy;			// Center of shadow filter
	float objDepth = shadowMapCenter_objDepth.z;					// Object depth in shadow space

	float4 vOneTaps;
	vOneTaps.x = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2(  fTwoEpsilonX,  fTwoEpsilonY ), objDepth, 1 ) ).x;
	vOneTaps.y = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2( -fTwoEpsilonX,  fTwoEpsilonY ), objDepth, 1 ) ).x;
	vOneTaps.z = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2(  fTwoEpsilonX, -fTwoEpsilonY ), objDepth, 1 ) ).x;
	vOneTaps.w = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2( -fTwoEpsilonX, -fTwoEpsilonY ), objDepth, 1 ) ).x;
	float flOneTaps = dot( vOneTaps, float4(1.0f / 331.0f, 1.0f / 331.0f, 1.0f / 331.0f, 1.0f / 331.0f));

	float4 vSevenTaps;
	vSevenTaps.x = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2(  fTwoEpsilonX,  0 ), objDepth, 1 ) ).x;
	vSevenTaps.y = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2( -fTwoEpsilonX,  0 ), objDepth, 1 ) ).x;
	vSevenTaps.z = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2(  0, fTwoEpsilonY ), objDepth, 1 ) ).x;
	vSevenTaps.w = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2(  0, -fTwoEpsilonY ), objDepth, 1 ) ).x;
	float flSevenTaps = dot( vSevenTaps, float4( 7.0f / 331.0f, 7.0f / 331.0f, 7.0f / 331.0f, 7.0f / 331.0f ) );

	float4 vFourTapsA, vFourTapsB;
	vFourTapsA.x = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2(  fTwoEpsilonX,  fEpsilonY    ), objDepth, 1 ) ).x;
	vFourTapsA.y = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2(  fEpsilonX,     fTwoEpsilonY ), objDepth, 1 ) ).x;
	vFourTapsA.z = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2( -fEpsilonX,     fTwoEpsilonY ), objDepth, 1 ) ).x;
	vFourTapsA.w = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2( -fTwoEpsilonX,  fEpsilonY    ), objDepth, 1 ) ).x;
	vFourTapsB.x = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2( -fTwoEpsilonX, -fEpsilonY    ), objDepth, 1 ) ).x;
	vFourTapsB.y = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2( -fEpsilonX,    -fTwoEpsilonY ), objDepth, 1 ) ).x;
	vFourTapsB.z = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2(  fEpsilonX,    -fTwoEpsilonY ), objDepth, 1 ) ).x;
	vFourTapsB.w = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2(  fTwoEpsilonX, -fEpsilonY    ), objDepth, 1 ) ).x;
	float flFourTapsA = dot( vFourTapsA, float4( 4.0f / 331.0f, 4.0f / 331.0f, 4.0f / 331.0f, 4.0f / 331.0f ) );
	float flFourTapsB = dot( vFourTapsB, float4( 4.0f / 331.0f, 4.0f / 331.0f, 4.0f / 331.0f, 4.0f / 331.0f ) );

	float4 v20Taps;
	v20Taps.x = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2(  fEpsilonX,  fEpsilonY ), objDepth, 1 ) ).x;
	v20Taps.y = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2( -fEpsilonX,  fEpsilonY ), objDepth, 1 ) ).x;
	v20Taps.z = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2(  fEpsilonX, -fEpsilonY ), objDepth, 1 ) ).x;
	v20Taps.w = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2( -fEpsilonX, -fEpsilonY ), objDepth, 1 ) ).x;
	float fl20Taps = dot( v20Taps, float4(20.0f / 331.0f, 20.0f / 331.0f, 20.0f / 331.0f, 20.0f / 331.0f));

	float4 v33Taps;
	v33Taps.x = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2(  fEpsilonX,  0 ), objDepth, 1 ) ).x;
	v33Taps.y = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2( -fEpsilonX,  0 ), objDepth, 1 ) ).x;
	v33Taps.z = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2(  0, fEpsilonY ), objDepth, 1 ) ).x;
	v33Taps.w = tex2Dproj( DepthSampler, float4( shadowMapCenter + float2(  0, -fEpsilonY ), objDepth, 1 ) ).x;
	float fl33Taps = dot( v33Taps, float4(33.0f / 331.0f, 33.0f / 331.0f, 33.0f / 331.0f, 33.0f / 331.0f));

	float flCenterTap = tex2Dproj( DepthSampler, float4( shadowMapCenter, objDepth, 1 ) ).x * (55.0f / 331.0f);

	// Sum all 25 Taps
	return flOneTaps + flSevenTaps + flFourTapsA + flFourTapsB + fl20Taps + fl33Taps + flCenterTap;
}


float DoShadowATICheap( sampler DepthSampler, const float4 shadowMapPos )
{
    float2 shadowMapCenter = shadowMapPos.xy/shadowMapPos.w;
	float objDepth = shadowMapPos.z / shadowMapPos.w;
	float fSampleDepth = tex2D( DepthSampler, shadowMapCenter ).x;

	objDepth = min( objDepth, 0.99999 ); //HACKHACK: On 360, surfaces at or past the far flashlight plane have an abrupt cutoff. This is temp until a smooth falloff is implemented

	return fSampleDepth > objDepth;
}


// Poisson disc, randomly rotated at different UVs
float DoShadowPoisson16Sample( sampler DepthSampler, sampler RandomRotationSampler, const float3 vProjCoords, const float2 vScreenPos, const float4 vShadowTweaks, bool bNvidiaHardwarePCF, bool bFetch4 )
{
	float2 vPoissonOffset[8] = { float2(  0.3475f,  0.0042f ), float2(  0.8806f,  0.3430f ), float2( -0.0041f, -0.6197f ), float2(  0.0472f,  0.4964f ),
								 float2( -0.3730f,  0.0874f ), float2( -0.9217f, -0.3177f ), float2( -0.6289f,  0.7388f ), float2(  0.5744f, -0.7741f ) };

	float flScaleOverMapSize = vShadowTweaks.x * 2;		// Tweak parameters to shader
	float2 vNoiseOffset = vShadowTweaks.zw;
	float4 vLightDepths = 0, accum = 0.0f;
	float2 rotOffset = 0;

	float2 shadowMapCenter = vProjCoords.xy;			// Center of shadow filter
	float objDepth = min( vProjCoords.z, 0.99999 );		// Object depth in shadow space

	// 2D Rotation Matrix setup
	float3 RMatTop = 0, RMatBottom = 0;
#if defined(SHADER_MODEL_PS_2_0) || defined(SHADER_MODEL_PS_2_B) || defined(SHADER_MODEL_PS_3_0)
	RMatTop.xy = tex2D( RandomRotationSampler, cFlashlightScreenScale.xy * (vScreenPos * 0.5 + 0.5) + vNoiseOffset).xy * 2.0 - 1.0;
	RMatBottom.xy = float2(-1.0, 1.0) * RMatTop.yx;	// 2x2 rotation matrix in 4-tuple
#endif

	RMatTop *= flScaleOverMapSize;				// Scale up kernel while accounting for texture resolution
	RMatBottom *= flScaleOverMapSize;

	RMatTop.z = shadowMapCenter.x;				// To be added in d2adds generated below
	RMatBottom.z = shadowMapCenter.y;
	
	float fResult = 0.0f;

	if ( bNvidiaHardwarePCF )
	{
		rotOffset.x = dot (RMatTop.xy,    vPoissonOffset[0].xy) + RMatTop.z;
		rotOffset.y = dot (RMatBottom.xy, vPoissonOffset[0].xy) + RMatBottom.z;
		vLightDepths.x += tex2Dproj( DepthSampler, float4(rotOffset, objDepth, 1) ).x;

		rotOffset.x = dot (RMatTop.xy,    vPoissonOffset[1].xy) + RMatTop.z;
		rotOffset.y = dot (RMatBottom.xy, vPoissonOffset[1].xy) + RMatBottom.z;
		vLightDepths.y += tex2Dproj( DepthSampler, float4(rotOffset, objDepth, 1) ).x;

		rotOffset.x = dot (RMatTop.xy,    vPoissonOffset[2].xy) + RMatTop.z;
		rotOffset.y = dot (RMatBottom.xy, vPoissonOffset[2].xy) + RMatBottom.z;
		vLightDepths.z += tex2Dproj( DepthSampler, float4(rotOffset, objDepth, 1) ).x;

		rotOffset.x = dot (RMatTop.xy,    vPoissonOffset[3].xy) + RMatTop.z;
		rotOffset.y = dot (RMatBottom.xy, vPoissonOffset[3].xy) + RMatBottom.z;
		vLightDepths.w += tex2Dproj( DepthSampler, float4(rotOffset, objDepth, 1) ).x;

		rotOffset.x = dot (RMatTop.xy,    vPoissonOffset[4].xy) + RMatTop.z;
		rotOffset.y = dot (RMatBottom.xy, vPoissonOffset[4].xy) + RMatBottom.z;
		vLightDepths.x += tex2Dproj( DepthSampler, float4(rotOffset, objDepth, 1) ).x;

		rotOffset.x = dot (RMatTop.xy,    vPoissonOffset[5].xy) + RMatTop.z;
		rotOffset.y = dot (RMatBottom.xy, vPoissonOffset[5].xy) + RMatBottom.z;
		vLightDepths.y += tex2Dproj( DepthSampler, float4(rotOffset, objDepth, 1) ).x;

		rotOffset.x = dot (RMatTop.xy,    vPoissonOffset[6].xy) + RMatTop.z;
		rotOffset.y = dot (RMatBottom.xy, vPoissonOffset[6].xy) + RMatBottom.z;
		vLightDepths.z += tex2Dproj( DepthSampler, float4(rotOffset, objDepth, 1) ).x;

		rotOffset.x = dot (RMatTop.xy,    vPoissonOffset[7].xy) + RMatTop.z;
		rotOffset.y = dot (RMatBottom.xy, vPoissonOffset[7].xy) + RMatBottom.z;
		vLightDepths.w += tex2Dproj( DepthSampler, float4(rotOffset, objDepth, 1) ).x;

		// First, search for blockers
		return dot( vLightDepths, float4( 0.25, 0.25, 0.25, 0.25) );
	}
	else if ( bFetch4 )
	{
		for( int i=0; i<8; i++ )
		{
			rotOffset.x = dot (RMatTop.xy,    vPoissonOffset[i].xy) + RMatTop.z;
			rotOffset.y = dot (RMatBottom.xy, vPoissonOffset[i].xy) + RMatBottom.z;
			vLightDepths = tex2D( DepthSampler, rotOffset.xy );
			accum += (vLightDepths > objDepth.xxxx);
		}

		return dot( accum, float4( 1.0f/32.0f, 1.0f/32.0f, 1.0f/32.0f, 1.0f/32.0f) );
	}
	else	// ATI vanilla hardware shadow mapping
	{
		for( int i=0; i<2; i++ )
		{
			rotOffset.x = dot (RMatTop.xy,    vPoissonOffset[4*i+0].xy) + RMatTop.z;
			rotOffset.y = dot (RMatBottom.xy, vPoissonOffset[4*i+0].xy) + RMatBottom.z;
			vLightDepths.x = tex2D( DepthSampler, rotOffset.xy ).x;

			rotOffset.x = dot (RMatTop.xy,    vPoissonOffset[4*i+1].xy) + RMatTop.z;
			rotOffset.y = dot (RMatBottom.xy, vPoissonOffset[4*i+1].xy) + RMatBottom.z;
			vLightDepths.y = tex2D( DepthSampler, rotOffset.xy ).x;

			rotOffset.x = dot (RMatTop.xy,    vPoissonOffset[4*i+2].xy) + RMatTop.z;
			rotOffset.y = dot (RMatBottom.xy, vPoissonOffset[4*i+2].xy) + RMatBottom.z;
			vLightDepths.z = tex2D( DepthSampler, rotOffset.xy ).x;

			rotOffset.x = dot (RMatTop.xy,    vPoissonOffset[4*i+3].xy) + RMatTop.z;
			rotOffset.y = dot (RMatBottom.xy, vPoissonOffset[4*i+3].xy) + RMatBottom.z;
			vLightDepths.w = tex2D( DepthSampler, rotOffset.xy ).x;

			accum += (vLightDepths > objDepth.xxxx);
		}

		return dot( accum, float4( 0.125, 0.125, 0.125, 0.125 ) );
	}
}

float DoFlashlightShadow( sampler DepthSampler, sampler RandomRotationSampler, float3 vProjCoords, float2 vScreenPos, int nShadowLevel, float4 vShadowTweaks, bool bAllowHighQuality, bool bForceSimple = false )
{
	float flShadow = 1.0f;

	if( nShadowLevel == NVIDIA_PCF_POISSON )
#if defined( NEW_SHADOW_FILTERS )
		flShadow = DoShadowNvidiaPCF5x5Gaussian( DepthSampler, vProjCoords, float2( 1.0 / 2048.0, 1.0 / 2048.0 ) );
#else		
		flShadow = DoShadowPoisson16Sample( DepthSampler, RandomRotationSampler, vProjCoords, vScreenPos, vShadowTweaks, true, false );
#endif
	else if( nShadowLevel == ATI_NOPCF )
		flShadow = DoShadowPoisson16Sample( DepthSampler, RandomRotationSampler, vProjCoords, vScreenPos, vShadowTweaks, false, false );
	else if( nShadowLevel == ATI_NO_PCF_FETCH4 )
		flShadow = DoShadowPoisson16Sample( DepthSampler, RandomRotationSampler, vProjCoords, vScreenPos, vShadowTweaks, false, true );

	return flShadow;
}

float3 SpecularLight( const float3 vWorldNormal, const float3 vLightDir, const float fSpecularExponent,
					  const float3 vEyeDir, const bool bDoSpecularWarp, in sampler specularWarpSampler, float fFresnel )
{
	float3 result = float3(0.0f, 0.0f, 0.0f);

	//float3 vReflect = reflect( -vEyeDir, vWorldNormal );
	float3 vReflect = 2 * vWorldNormal * dot(vWorldNormal, vEyeDir) - vEyeDir; // Reflect view through normal
	float3 vSpecular = saturate(dot( vReflect, vLightDir ));		// L.R	(use half-angle instead?)
	vSpecular = pow( vSpecular.x, fSpecularExponent );				// Raise to specular power

	// Optionally warp as function of scalar specular and fresnel
	if ( bDoSpecularWarp )
		vSpecular *= tex2D( specularWarpSampler, float2(vSpecular.x, fFresnel) ).xyz; // Sample at { (L.R)^k, fresnel }

	return vSpecular;
}

void DoSpecularFlashlight( float3 flashlightPos, float3 worldPos, float4 flashlightSpacePosition, float3 worldNormal,  
					float3 attenuationFactors, float farZ, sampler FlashlightSampler, sampler FlashlightDepthSampler, sampler RandomRotationSampler,
					int nShadowLevel, bool bDoShadows, bool bAllowHighQuality, const float2 vScreenPos, const float fSpecularExponent, const float3 vEyeDir,
					const bool bDoSpecularWarp, sampler specularWarpSampler, float fFresnel, float4 vShadowTweaks,

					// Outputs of this shader...separate shadowed diffuse and specular from the flashlight
					out float3 diffuseLighting, out float3 specularLighting )
{
	float3 vProjCoords = flashlightSpacePosition.xyz / flashlightSpacePosition.w;
	float3 flashlightColor = tex2D( FlashlightSampler, vProjCoords.xy ).xyz;

#if defined(SHADER_MODEL_PS_2_B) || defined(SHADER_MODEL_PS_3_0)
	flashlightColor *= flashlightSpacePosition.w > 0;	// Catch back projection (PC-only, ps2b and up)
#endif

#if defined(SHADER_MODEL_PS_2_0) || defined(SHADER_MODEL_PS_2_B) || defined(SHADER_MODEL_PS_3_0)
	flashlightColor *= cFlashlightColor.xyz;						// Flashlight color
#endif

	float3 delta = flashlightPos - worldPos;
	float3 L = normalize( delta );
	float distSquared = dot( delta, delta );
	float dist = sqrt( distSquared );

	float endFalloffFactor = RemapValClamped( dist, farZ, 0.6f * farZ, 0.0f, 1.0f );

	// Attenuation for light and to fade out shadow over distance
	float fAtten = saturate( dot( attenuationFactors, float3( 1.0f, 1.0f/dist, 1.0f/distSquared ) ) );

	// Shadowing and coloring terms
#if (defined(SHADER_MODEL_PS_2_B) || defined(SHADER_MODEL_PS_3_0))
	if ( bDoShadows )
	{
		float flShadow = DoFlashlightShadow( FlashlightDepthSampler, RandomRotationSampler, vProjCoords, vScreenPos, nShadowLevel, vShadowTweaks, bAllowHighQuality );
		float flAttenuated = lerp( flShadow, 1.0f, vShadowTweaks.y );	// Blend between fully attenuated and not attenuated
		flShadow = saturate( lerp( flAttenuated, flShadow, fAtten ) );	// Blend between shadow and above, according to light attenuation
		flashlightColor *= flShadow;									// Shadow term
	}
#endif

	diffuseLighting = fAtten;
#if defined(SHADER_MODEL_PS_2_0) || defined(SHADER_MODEL_PS_2_B) || defined(SHADER_MODEL_PS_3_0)
		diffuseLighting *= saturate( dot( L.xyz, worldNormal.xyz ) + flFlashlightNoLambertValue ); // Lambertian term
#else
		diffuseLighting *= saturate( dot( L.xyz, worldNormal.xyz ) ); // Lambertian (not Half-Lambert) term
#endif
	diffuseLighting *= flashlightColor;
	diffuseLighting *= endFalloffFactor;

	// Specular term (masked by diffuse)
	specularLighting = diffuseLighting * SpecularLight ( worldNormal, L, fSpecularExponent, vEyeDir, bDoSpecularWarp, specularWarpSampler, fFresnel );
}

// Diffuse only version
float3 DoFlashlight( float3 flashlightPos, float3 worldPos, float4 flashlightSpacePosition, float3 worldNormal, 
					float3 attenuationFactors, float farZ, sampler FlashlightSampler, sampler FlashlightDepthSampler,
					sampler RandomRotationSampler, int nShadowLevel, bool bDoShadows, bool bAllowHighQuality,
					const float2 vScreenPos, bool bClip, float4 vShadowTweaks = float4(3/1024.0f, 0.0005f, 0.0f, 0.0f), bool bHasNormal = true )
{
	if ( flashlightSpacePosition.w < 0 )
	{
		return float3(0,0,0);
	}
	else
	{
	float3 vProjCoords = flashlightSpacePosition.xyz / flashlightSpacePosition.w;
	float3 flashlightColor = tex2D( FlashlightSampler, vProjCoords.xy ).xyz;

#if defined(SHADER_MODEL_PS_2_0) || defined(SHADER_MODEL_PS_2_B) || defined(SHADER_MODEL_PS_3_0)
	flashlightColor *= cFlashlightColor.xyz;						// Flashlight color
#endif

	float3 delta = flashlightPos - worldPos;
	float3 L = normalize( delta );
	float distSquared = dot( delta, delta );
	float dist = sqrt( distSquared );

	float endFalloffFactor = RemapValClamped( dist, farZ, 0.6f * farZ, 0.0f, 1.0f );

	// Attenuation for light and to fade out shadow over distance
	float fAtten = saturate( dot( attenuationFactors, float3( 1.0f, 1.0f/dist, 1.0f/distSquared ) ) );

	// Shadowing and coloring terms
#if (defined(SHADER_MODEL_PS_2_B) || defined(SHADER_MODEL_PS_3_0))
	if ( bDoShadows )
	{
		float flShadow = DoFlashlightShadow( FlashlightDepthSampler, RandomRotationSampler, vProjCoords, vScreenPos, nShadowLevel, vShadowTweaks, bAllowHighQuality );
		float flAttenuated = lerp( saturate( flShadow ), 1.0f, vShadowTweaks.y );	// Blend between fully attenuated and not attenuated
		flShadow = saturate( lerp( flAttenuated, flShadow, fAtten ) );	// Blend between shadow and above, according to light attenuation
		flashlightColor *= flShadow;									// Shadow term
	}
#endif

	float3 diffuseLighting = fAtten;

	float flLDotWorldNormal;
	if ( bHasNormal )
	{
		flLDotWorldNormal = dot( L.xyz, worldNormal.xyz );
	}
	else
	{
		flLDotWorldNormal = 1.0f;
	}

#if defined(SHADER_MODEL_PS_2_0) || defined(SHADER_MODEL_PS_2_B) || defined(SHADER_MODEL_PS_3_0)
	diffuseLighting *= saturate( flLDotWorldNormal + flFlashlightNoLambertValue ); // Lambertian term
#else
	diffuseLighting *= saturate( flLDotWorldNormal ); // Lambertian (not Half-Lambert) term
#endif

	diffuseLighting *= flashlightColor;
	diffuseLighting *= endFalloffFactor;

	return diffuseLighting;
	}
}

#ifdef NEW_SHADOW_FILTERS
float DoCascadedShadow( sampler depthSampler, sampler randomSampler, float3 worldNormal, float3 lightDirection,
	float3 closePosition, float3 worldPosition, int nShadowLevel, float3 cascadedStepData,
	float2 vScreenPos, float4 vShadowTweaks, const bool bCheckDot = true )
{
	float cascadedDot = bCheckDot ? dot( -lightDirection, worldNormal ) : abs( dot( -lightDirection, worldNormal ) );

	// 0.0 samples far cascade, 1.0 samples close cascade.
	float blendCascades = step(closePosition.x, 0.49) * step(0.01, closePosition.x) *
		step(closePosition.y, 0.99) * step(0.01, closePosition.y);

	// Select the cascade.
	closePosition.xy = lerp(closePosition.xy * cascadedStepData.x + cascadedStepData.yz, closePosition.xy, blendCascades);

	float shadow;
	//if ( nShadowLevel == NVIDIA_PCF_POISSON )
		shadow = DoShadowNvidiaPCF5x5Gaussian( depthSampler, closePosition, float2( 1.0 / 2048.0, 1.0 / 1024.0 ) );
	//else if( nShadowLevel == ATI_NOPCF )
	//	shadow = DoShadowPoisson16Sample( depthSampler, randomSampler, closePosition, vScreenPos, vShadowTweaks, false, false );
	//else //if( nShadowLevel == ATI_NO_PCF_FETCH4 )
	//	shadow = DoShadowPoisson16Sample( depthSampler, randomSampler, closePosition, vScreenPos, vShadowTweaks, false, true );

	float weight = lerp( saturate( saturate( abs( closePosition.x * 4.0 - 3.0 ) - 0.9 ) * 10.0 +
		saturate( abs( closePosition.y * 2.0 - 1.0 ) - 0.9 ) * 10.0 ), 0.0f, blendCascades);
	shadow = lerp( shadow, 1.0, weight ); // * saturate( cascadedDot * 12.0 );

	return lerp(1.0, shadow, smoothstep(-0.1, 0.1, cascadedDot)); //lerp(0.0, shadow, step(0.0, cascadedDot));
}
#endif

#endif //#ifndef COMMON_FLASHLIGHT_FXC_H_
