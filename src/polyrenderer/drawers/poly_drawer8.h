/*
**  Projected triangle drawer
**  Copyright (c) 2016 Magnus Norddahl
**
**  This software is provided 'as-is', without any express or implied
**  warranty.  In no event will the authors be held liable for any damages
**  arising from the use of this software.
**
**  Permission is granted to anyone to use this software for any purpose,
**  including commercial applications, and to alter it and redistribute it
**  freely, subject to the following restrictions:
**
**  1. The origin of this software must not be misrepresented; you must not
**     claim that you wrote the original software. If you use this software
**     in a product, an acknowledgment in the product documentation would be
**     appreciated but is not required.
**  2. Altered source versions must be plainly marked as such, and must not be
**     misrepresented as being the original software.
**  3. This notice may not be removed or altered from any source distribution.
**
*/

#pragma once

#include "screen_triangle.h"

template<typename BlendT, typename SamplerT>
class TriScreenDrawer8
{
public:
	static void Execute(const TriDrawTriangleArgs *args, WorkerThreadData *thread)
	{
		using namespace TriScreenDrawerModes;

		int numSpans = thread->NumFullSpans;
		auto fullSpans = thread->FullSpans;
		int numBlocks = thread->NumPartialBlocks;
		auto partialBlocks = thread->PartialBlocks;
		int startX = thread->StartX;
		int startY = thread->StartY;

		auto flags = args->uniforms->flags;
		bool is_fixed_light = (flags & TriUniforms::fixed_light) == TriUniforms::fixed_light;
		uint32_t lightmask = is_fixed_light ? 0 : 0xffffffff;
		auto colormaps = args->colormaps;
		uint32_t srcalpha = args->uniforms->srcalpha;
		uint32_t destalpha = args->uniforms->destalpha;

		// Calculate gradients
		const TriVertex &v1 = *args->v1;
		const TriVertex &v2 = *args->v2;
		const TriVertex &v3 = *args->v3;
		ScreenTriangleStepVariables gradientX;
		ScreenTriangleStepVariables gradientY;
		ScreenTriangleStepVariables start;
		gradientX.W = FindGradientX(v1.x, v1.y, v2.x, v2.y, v3.x, v3.y, v1.w, v2.w, v3.w);
		gradientY.W = FindGradientY(v1.x, v1.y, v2.x, v2.y, v3.x, v3.y, v1.w, v2.w, v3.w);
		start.W = v1.w + gradientX.W * (startX - v1.x) + gradientY.W * (startY - v1.y);
		for (int i = 0; i < TriVertex::NumVarying; i++)
		{
			gradientX.Varying[i] = FindGradientX(v1.x, v1.y, v2.x, v2.y, v3.x, v3.y, v1.varying[i] * v1.w, v2.varying[i] * v2.w, v3.varying[i] * v3.w);
			gradientY.Varying[i] = FindGradientY(v1.x, v1.y, v2.x, v2.y, v3.x, v3.y, v1.varying[i] * v1.w, v2.varying[i] * v2.w, v3.varying[i] * v3.w);
			start.Varying[i] = v1.varying[i] * v1.w + gradientX.Varying[i] * (startX - v1.x) + gradientY.Varying[i] * (startY - v1.y);
		}

		// Output
		uint8_t * RESTRICT destOrg = args->dest;
		int pitch = args->pitch;

		// Light
		uint32_t light = args->uniforms->light;
		float shade = (64.0f - (light * 255 / 256 + 12.0f) * 32.0f / 128.0f) / 32.0f;
		float globVis = args->uniforms->globvis * (1.0f / 32.0f);

		// Sampling stuff
		uint32_t color = args->uniforms->color;
		const uint8_t * RESTRICT translation = args->translation;
		const uint8_t * RESTRICT texPixels = args->texturePixels;
		uint32_t texWidth = args->textureWidth;
		uint32_t texHeight = args->textureHeight;

		for (int i = 0; i < numSpans; i++)
		{
			const auto &span = fullSpans[i];

			uint8_t *dest = destOrg + span.X + span.Y * pitch;
			int width = span.Length;
			int height = 8;

			ScreenTriangleStepVariables blockPosY;
			blockPosY.W = start.W + gradientX.W * (span.X - startX) + gradientY.W * (span.Y - startY);
			for (int j = 0; j < TriVertex::NumVarying; j++)
				blockPosY.Varying[j] = start.Varying[j] + gradientX.Varying[j] * (span.X - startX) + gradientY.Varying[j] * (span.Y - startY);

			for (int y = 0; y < height; y++)
			{
				ScreenTriangleStepVariables blockPosX = blockPosY;

				float rcpW = 0x01000000 / blockPosX.W;
				int32_t varyingPos[TriVertex::NumVarying];
				for (int j = 0; j < TriVertex::NumVarying; j++)
					varyingPos[j] = (int32_t)(blockPosX.Varying[j] * rcpW);

				fixed_t lightpos = FRACUNIT - (int)(clamp(shade - MIN(24.0f / 32.0f, globVis * blockPosY.W), 0.0f, 31.0f / 32.0f) * (float)FRACUNIT);
				lightpos = (lightpos & lightmask) | ((light << 8) & ~lightmask);

				for (int x = 0; x < width; x++)
				{
					blockPosX.W += gradientX.W * 8;
					for (int j = 0; j < TriVertex::NumVarying; j++)
						blockPosX.Varying[j] += gradientX.Varying[j] * 8;

					rcpW = 0x01000000 / blockPosX.W;
					int32_t varyingStep[TriVertex::NumVarying];
					for (int j = 0; j < TriVertex::NumVarying; j++)
					{
						int32_t nextPos = (int32_t)(blockPosX.Varying[j] * rcpW);
						varyingStep[j] = (nextPos - varyingPos[j]) / 8;
					}

					fixed_t lightnext = FRACUNIT - (fixed_t)(clamp(shade - MIN(24.0f / 32.0f, globVis * blockPosX.W), 0.0f, 31.0f / 32.0f) * (float)FRACUNIT);
					fixed_t lightstep = (lightnext - lightpos) / 8;
					lightstep = lightstep & lightmask;

					for (int ix = 0; ix < 8; ix++)
					{
						int lightshade = lightpos >> 8;
						uint8_t bgcolor = dest[x * 8 + ix];
						uint8_t fgcolor = Sample(varyingPos[0], varyingPos[1], texPixels, texWidth, texHeight, color, translation);
						uint32_t fgshade = SampleShade(varyingPos[0], varyingPos[1], texPixels, texWidth, texHeight);
						dest[x * 8 + ix] = ShadeAndBlend(fgcolor, bgcolor, fgshade, lightshade, colormaps, srcalpha, destalpha);
						for (int j = 0; j < TriVertex::NumVarying; j++)
							varyingPos[j] += varyingStep[j];
						lightpos += lightstep;
					}
				}

				blockPosY.W += gradientY.W;
				for (int j = 0; j < TriVertex::NumVarying; j++)
					blockPosY.Varying[j] += gradientY.Varying[j];

				dest += pitch;
			}
		}

		for (int i = 0; i < numBlocks; i++)
		{
			const auto &block = partialBlocks[i];

			ScreenTriangleStepVariables blockPosY;
			blockPosY.W = start.W + gradientX.W * (block.X - startX) + gradientY.W * (block.Y - startY);
			for (int j = 0; j < TriVertex::NumVarying; j++)
				blockPosY.Varying[j] = start.Varying[j] + gradientX.Varying[j] * (block.X - startX) + gradientY.Varying[j] * (block.Y - startY);

			uint8_t *dest = destOrg + block.X + block.Y * pitch;
			uint32_t mask0 = block.Mask0;
			uint32_t mask1 = block.Mask1;

			// mask0 loop:
			for (int y = 0; y < 4; y++)
			{
				ScreenTriangleStepVariables blockPosX = blockPosY;

				float rcpW = 0x01000000 / blockPosX.W;
				int32_t varyingPos[TriVertex::NumVarying];
				for (int j = 0; j < TriVertex::NumVarying; j++)
					varyingPos[j] = (int32_t)(blockPosX.Varying[j] * rcpW);

				fixed_t lightpos = FRACUNIT - (fixed_t)(clamp(shade - MIN(24.0f / 32.0f, globVis * blockPosY.W), 0.0f, 31.0f / 32.0f) * (float)FRACUNIT);
				lightpos = (lightpos & lightmask) | ((light << 8) & ~lightmask);

				blockPosX.W += gradientX.W * 8;
				for (int j = 0; j < TriVertex::NumVarying; j++)
					blockPosX.Varying[j] += gradientX.Varying[j] * 8;

				rcpW = 0x01000000 / blockPosX.W;
				int32_t varyingStep[TriVertex::NumVarying];
				for (int j = 0; j < TriVertex::NumVarying; j++)
				{
					int32_t nextPos = (int32_t)(blockPosX.Varying[j] * rcpW);
					varyingStep[j] = (nextPos - varyingPos[j]) / 8;
				}

				fixed_t lightnext = FRACUNIT - (fixed_t)(clamp(shade - MIN(24.0f / 32.0f, globVis * blockPosX.W), 0.0f, 31.0f / 32.0f) * (float)FRACUNIT);
				fixed_t lightstep = (lightnext - lightpos) / 8;
				lightstep = lightstep & lightmask;

				for (int x = 0; x < 8; x++)
				{
					if (mask0 & (1 << 31))
					{
						int lightshade = lightpos >> 8;
						uint8_t bgcolor = dest[x];
						uint8_t fgcolor = Sample(varyingPos[0], varyingPos[1], texPixels, texWidth, texHeight, color, translation);
						uint32_t fgshade = SampleShade(varyingPos[0], varyingPos[1], texPixels, texWidth, texHeight);
						dest[x] = ShadeAndBlend(fgcolor, bgcolor, fgshade, lightshade, colormaps, srcalpha, destalpha);
					}

					for (int j = 0; j < TriVertex::NumVarying; j++)
						varyingPos[j] += varyingStep[j];
					lightpos += lightstep;

					mask0 <<= 1;
				}

				blockPosY.W += gradientY.W;
				for (int j = 0; j < TriVertex::NumVarying; j++)
					blockPosY.Varying[j] += gradientY.Varying[j];

				dest += pitch;
			}

			// mask1 loop:
			for (int y = 0; y < 4; y++)
			{
				ScreenTriangleStepVariables blockPosX = blockPosY;

				float rcpW = 0x01000000 / blockPosX.W;
				int32_t varyingPos[TriVertex::NumVarying];
				for (int j = 0; j < TriVertex::NumVarying; j++)
					varyingPos[j] = (int32_t)(blockPosX.Varying[j] * rcpW);

				fixed_t lightpos = FRACUNIT - (fixed_t)(clamp(shade - MIN(24.0f / 32.0f, globVis * blockPosY.W), 0.0f, 31.0f / 32.0f) * (float)FRACUNIT);
				lightpos = (lightpos & lightmask) | ((light << 8) & ~lightmask);

				blockPosX.W += gradientX.W * 8;
				for (int j = 0; j < TriVertex::NumVarying; j++)
					blockPosX.Varying[j] += gradientX.Varying[j] * 8;

				rcpW = 0x01000000 / blockPosX.W;
				int32_t varyingStep[TriVertex::NumVarying];
				for (int j = 0; j < TriVertex::NumVarying; j++)
				{
					int32_t nextPos = (int32_t)(blockPosX.Varying[j] * rcpW);
					varyingStep[j] = (nextPos - varyingPos[j]) / 8;
				}

				fixed_t lightnext = FRACUNIT - (fixed_t)(clamp(shade - MIN(24.0f / 32.0f, globVis * blockPosX.W), 0.0f, 31.0f / 32.0f) * (float)FRACUNIT);
				fixed_t lightstep = (lightnext - lightpos) / 8;
				lightstep = lightstep & lightmask;

				for (int x = 0; x < 8; x++)
				{
					if (mask1 & (1 << 31))
					{
						int lightshade = lightpos >> 8;
						uint8_t bgcolor = dest[x];
						uint8_t fgcolor = Sample(varyingPos[0], varyingPos[1], texPixels, texWidth, texHeight, color, translation);
						uint32_t fgshade = SampleShade(varyingPos[0], varyingPos[1], texPixels, texWidth, texHeight);
						dest[x] = ShadeAndBlend(fgcolor, bgcolor, fgshade, lightshade, colormaps, srcalpha, destalpha);
					}

					for (int j = 0; j < TriVertex::NumVarying; j++)
						varyingPos[j] += varyingStep[j];
					lightpos += lightstep;

					mask1 <<= 1;
				}

				blockPosY.W += gradientY.W;
				for (int j = 0; j < TriVertex::NumVarying; j++)
					blockPosY.Varying[j] += gradientY.Varying[j];

				dest += pitch;
			}
		}
	}

private:
	FORCEINLINE static unsigned int Sample(int32_t u, int32_t v, const uint8_t *texPixels, int texWidth, int texHeight, uint32_t color, const uint8_t *translation)
	{
		using namespace TriScreenDrawerModes;

		uint8_t texel;
		if (SamplerT::Mode == (int)Samplers::Shaded || SamplerT::Mode == (int)Samplers::Fill)
		{
			return color;
		}
		else if (SamplerT::Mode == (int)Samplers::Translated)
		{
			uint32_t texelX = ((((uint32_t)u << 8) >> 16) * texWidth) >> 16;
			uint32_t texelY = ((((uint32_t)v << 8) >> 16) * texHeight) >> 16;
			return translation[texPixels[texelX * texHeight + texelY]];
		}
		else
		{
			uint32_t texelX = ((((uint32_t)u << 8) >> 16) * texWidth) >> 16;
			uint32_t texelY = ((((uint32_t)v << 8) >> 16) * texHeight) >> 16;
			texel = texPixels[texelX * texHeight + texelY];
		}

		if (SamplerT::Mode == (int)Samplers::Skycap)
		{
			int start_fade = 2; // How fast it should fade out

			int alpha_top = clamp(v >> (16 - start_fade), 0, 256);
			int alpha_bottom = clamp(((2 << 24) - v) >> (16 - start_fade), 0, 256);
			int a = MIN(alpha_top, alpha_bottom);
			int inv_a = 256 - a;

			if (a == 256)
				return texel;

			uint32_t texelrgb = GPalette.BaseColors[texel].d;
			uint32_t r = RPART(texelrgb);
			uint32_t g = GPART(texelrgb);
			uint32_t b = BPART(texelrgb);
			uint32_t capcolor_red = RPART(color);
			uint32_t capcolor_green = GPART(color);
			uint32_t capcolor_blue = BPART(color);
			r = (r * a + capcolor_red * inv_a + 127) >> 8;
			g = (g * a + capcolor_green * inv_a + 127) >> 8;
			b = (b * a + capcolor_blue * inv_a + 127) >> 8;
			return RGB256k.All[((r >> 2) << 12) | ((g >> 2) << 6) | (b >> 2)];
		}
		else
		{
			return texel;
		}
	}

	FORCEINLINE static unsigned int SampleShade(int32_t u, int32_t v, const uint8_t *texPixels, int texWidth, int texHeight)
	{
		using namespace TriScreenDrawerModes;

		if (SamplerT::Mode == (int)Samplers::Shaded)
		{
			uint32_t texelX = ((((uint32_t)u << 8) >> 16) * texWidth) >> 16;
			uint32_t texelY = ((((uint32_t)v << 8) >> 16) * texHeight) >> 16;
			unsigned int sampleshadeout = texPixels[texelX * texHeight + texelY];
			sampleshadeout += sampleshadeout >> 7; // 255 -> 256
			return sampleshadeout;
		}
		else
		{
			return 0;
		}
	}

	FORCEINLINE static uint8_t ShadeAndBlend(uint8_t fgcolor, uint8_t bgcolor, uint32_t fgshade, uint32_t lightshade, const uint8_t *colormaps, uint32_t srcalpha, uint32_t destalpha)
	{
		using namespace TriScreenDrawerModes;

		lightshade = ((256 - lightshade) * NUMCOLORMAPS) & 0xffffff00;
		uint8_t shadedfg = colormaps[lightshade + fgcolor];

		if (BlendT::Mode == (int)BlendModes::Opaque)
		{
			return shadedfg;
		}
		else if (BlendT::Mode == (int)BlendModes::Masked)
		{
			return (fgcolor != 0) ? shadedfg : bgcolor;
		}
		else if (BlendT::Mode == (int)BlendModes::AddSrcColorOneMinusSrcColor)
		{
			int32_t fg_r = GPalette.BaseColors[shadedfg].r;
			int32_t fg_g = GPalette.BaseColors[shadedfg].g;
			int32_t fg_b = GPalette.BaseColors[shadedfg].b;
			int32_t bg_r = GPalette.BaseColors[bgcolor].r;
			int32_t bg_g = GPalette.BaseColors[bgcolor].g;
			int32_t bg_b = GPalette.BaseColors[bgcolor].b;
			int32_t inv_fg_r = 256 - (fg_r + (fg_r >> 7));
			int32_t inv_fg_g = 256 - (fg_g + (fg_g >> 7));
			int32_t inv_fg_b = 256 - (fg_b + (fg_b >> 7));
			fg_r = MIN<int32_t>(fg_r + ((bg_r * inv_fg_r + 127) >> 8), 255);
			fg_g = MIN<int32_t>(fg_g + ((bg_g * inv_fg_g + 127) >> 8), 255);
			fg_b = MIN<int32_t>(fg_b + ((bg_b * inv_fg_b + 127) >> 8), 255);

			shadedfg = RGB256k.All[((fg_r >> 2) << 12) | ((fg_g >> 2) << 6) | (fg_b >> 2)];
			return (fgcolor != 0) ? shadedfg : bgcolor;
		}
		else if (BlendT::Mode == (int)BlendModes::Shaded)
		{
			uint32_t alpha = fgshade;
			uint32_t inv_alpha = 256 - fgshade;
			int32_t fg_r = GPalette.BaseColors[shadedfg].r;
			int32_t fg_g = GPalette.BaseColors[shadedfg].g;
			int32_t fg_b = GPalette.BaseColors[shadedfg].b;
			int32_t bg_r = GPalette.BaseColors[bgcolor].r;
			int32_t bg_g = GPalette.BaseColors[bgcolor].g;
			int32_t bg_b = GPalette.BaseColors[bgcolor].b;

			fg_r = (fg_r * alpha + bg_r * inv_alpha + 127) >> 8;
			fg_g = (fg_g * alpha + bg_g * inv_alpha + 127) >> 8;
			fg_b = (fg_b * alpha + bg_b * inv_alpha + 127) >> 8;

			shadedfg = RGB256k.All[((fg_r >> 2) << 12) | ((fg_g >> 2) << 6) | (fg_b >> 2)];
			return (alpha != 0) ? shadedfg : bgcolor;
		}
		else if (BlendT::Mode == (int)BlendModes::AddClampShaded)
		{
			uint32_t alpha = fgshade;
			int32_t fg_r = GPalette.BaseColors[shadedfg].r;
			int32_t fg_g = GPalette.BaseColors[shadedfg].g;
			int32_t fg_b = GPalette.BaseColors[shadedfg].b;
			int32_t bg_r = GPalette.BaseColors[bgcolor].r;
			int32_t bg_g = GPalette.BaseColors[bgcolor].g;
			int32_t bg_b = GPalette.BaseColors[bgcolor].b;

			fg_r = MIN<int32_t>(bg_r + ((fg_r * alpha + 127) >> 8), 255);
			fg_g = MIN<int32_t>(bg_g + ((fg_g * alpha + 127) >> 8), 255);
			fg_b = MIN<int32_t>(bg_b + ((fg_b * alpha + 127) >> 8), 255);

			shadedfg = RGB256k.All[((fg_r >> 2) << 12) | ((fg_g >> 2) << 6) | (fg_b >> 2)];

			return (alpha != 0) ? shadedfg : bgcolor;
		}
		else
		{
			int32_t fg_r = GPalette.BaseColors[shadedfg].r;
			int32_t fg_g = GPalette.BaseColors[shadedfg].g;
			int32_t fg_b = GPalette.BaseColors[shadedfg].b;
			int32_t bg_r = GPalette.BaseColors[bgcolor].r;
			int32_t bg_g = GPalette.BaseColors[bgcolor].g;
			int32_t bg_b = GPalette.BaseColors[bgcolor].b;

			if (BlendT::Mode == (int)BlendModes::AddClamp)
			{
				fg_r = MIN<int32_t>((fg_r * srcalpha + bg_r * destalpha + 127) >> 8, 255);
				fg_g = MIN<int32_t>((fg_g * srcalpha + bg_g * destalpha + 127) >> 8, 255);
				fg_b = MIN<int32_t>((fg_b * srcalpha + bg_b * destalpha + 127) >> 8, 255);
			}
			else if (BlendT::Mode == (int)BlendModes::SubClamp)
			{
				fg_r = MAX<int32_t>((fg_r * srcalpha - bg_r * destalpha + 127) >> 8, 0);
				fg_g = MAX<int32_t>((fg_g * srcalpha - bg_g * destalpha + 127) >> 8, 0);
				fg_b = MAX<int32_t>((fg_b * srcalpha - bg_b * destalpha + 127) >> 8, 0);
			}
			else if (BlendT::Mode == (int)BlendModes::RevSubClamp)
			{
				fg_r = MAX<int32_t>((bg_r * srcalpha - fg_r * destalpha + 127) >> 8, 0);
				fg_g = MAX<int32_t>((bg_g * srcalpha - fg_g * destalpha + 127) >> 8, 0);
				fg_b = MAX<int32_t>((bg_b * srcalpha - fg_b * destalpha + 127) >> 8, 0);
			}

			shadedfg = RGB256k.All[((fg_r >> 2) << 12) | ((fg_g >> 2) << 6) | (fg_b >> 2)];
			return (fgcolor != 0) ? shadedfg : bgcolor;
		}
	}

	static float FindGradientX(float x0, float y0, float x1, float y1, float x2, float y2, float c0, float c1, float c2)
	{
		float top = (c1 - c2) * (y0 - y2) - (c0 - c2) * (y1 - y2);
		float bottom = (x1 - x2) * (y0 - y2) - (x0 - x2) * (y1 - y2);
		return top / bottom;
	}

	static float FindGradientY(float x0, float y0, float x1, float y1, float x2, float y2, float c0, float c1, float c2)
	{
		float top = (c1 - c2) * (x0 - x2) - (c0 - c2) * (x1 - x2);
		float bottom = (x0 - x2) * (y1 - y2) - (x1 - x2) * (y0 - y2);
		return top / bottom;
	}
};
