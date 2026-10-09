// GeckoW10m: Clear11's shaders for Direct3D feature level 9_3, the level of
// the Adreno 3xx in the Lumia 550, 650 and 950.
//
// Clear11 draws a quad whenever ClearRenderTargetView cannot do the clear: a
// colour mask (every gl.clear on an alpha:false WebGL context, because Gecko
// masks alpha there), a scissor without ClearView, part of the stencil. Its
// other shaders are vs_4_0/ps_4_0, which a 9_3 device refuses to create, so
// at that level every such clear failed. These are built for the level 9
// profiles instead.
//
// A level 9 vertex shader has no SV_VertexID, so the quad's corners come from
// the vertex buffer Clear11 already keeps for this level (POSITION, four
// floats), and the depth to clear to comes from the vertex shader as the
// quad's z rather than from the pixel shader. ES 2 clears only to floats, so
// there are no integer variants; 9_3 has four render targets, so four colour
// variants.
//
// Compiled once, offline, into shaders/compiled/ (the build does not run
// fxc); see the command lines at the end.

cbuffer ColorAndDepthDataFloat : register(b0)
{
    float4 color_Float   : packoffset(c0);
    float  zValueF_Float : packoffset(c1);
}

void VS_Clear_FL9(in float4 inPosition : POSITION, out float4 outPosition : SV_POSITION)
{
    outPosition = float4(inPosition.xy, zValueF_Float, 1.0f);
}

struct PS_OutputFloat1_FL9
{
    float4 color0 : SV_TARGET0;
};

struct PS_OutputFloat2_FL9
{
    float4 color0 : SV_TARGET0;
    float4 color1 : SV_TARGET1;
};

struct PS_OutputFloat3_FL9
{
    float4 color0 : SV_TARGET0;
    float4 color1 : SV_TARGET1;
    float4 color2 : SV_TARGET2;
};

struct PS_OutputFloat4_FL9
{
    float4 color0 : SV_TARGET0;
    float4 color1 : SV_TARGET1;
    float4 color2 : SV_TARGET2;
    float4 color3 : SV_TARGET3;
};

PS_OutputFloat1_FL9 PS_ClearFloat1_FL9()
{
    PS_OutputFloat1_FL9 outColor;
    outColor.color0 = color_Float;
    return outColor;
}

PS_OutputFloat2_FL9 PS_ClearFloat2_FL9()
{
    PS_OutputFloat2_FL9 outColor;
    outColor.color0 = color_Float;
    outColor.color1 = color_Float;
    return outColor;
}

PS_OutputFloat3_FL9 PS_ClearFloat3_FL9()
{
    PS_OutputFloat3_FL9 outColor;
    outColor.color0 = color_Float;
    outColor.color1 = color_Float;
    outColor.color2 = color_Float;
    return outColor;
}

PS_OutputFloat4_FL9 PS_ClearFloat4_FL9()
{
    PS_OutputFloat4_FL9 outColor;
    outColor.color0 = color_Float;
    outColor.color1 = color_Float;
    outColor.color2 = color_Float;
    outColor.color3 = color_Float;
    return outColor;
}

// Depth or stencil only. Level 9 will not draw without a colour output (the
// translator adds a dummy one for the same reason), and with no render target
// bound this one goes nowhere.
float4 PS_ClearDepth_FL9() : SV_TARGET0
{
    return float4(0.0f, 0.0f, 0.0f, 1.0f);
}

// fxc /nologo /E VS_Clear_FL9 /T vs_4_0_level_9_3 /Vn g_VS_Clear_FL9 /Fh compiled/clear11_fl9vs.h Clear11_FL9.hlsl
// fxc /nologo /E PS_ClearFloat1_FL9 /T ps_4_0_level_9_3 /Vn g_PS_ClearFloat1_FL9 /Fh compiled/clearfloat11_fl9ps1.h Clear11_FL9.hlsl
// fxc /nologo /E PS_ClearFloat2_FL9 /T ps_4_0_level_9_3 /Vn g_PS_ClearFloat2_FL9 /Fh compiled/clearfloat11_fl9ps2.h Clear11_FL9.hlsl
// fxc /nologo /E PS_ClearFloat3_FL9 /T ps_4_0_level_9_3 /Vn g_PS_ClearFloat3_FL9 /Fh compiled/clearfloat11_fl9ps3.h Clear11_FL9.hlsl
// fxc /nologo /E PS_ClearFloat4_FL9 /T ps_4_0_level_9_3 /Vn g_PS_ClearFloat4_FL9 /Fh compiled/clearfloat11_fl9ps4.h Clear11_FL9.hlsl
// fxc /nologo /E PS_ClearDepth_FL9 /T ps_4_0_level_9_3 /Vn g_PS_ClearDepth_FL9 /Fh compiled/cleardepth11_fl9ps.h Clear11_FL9.hlsl
