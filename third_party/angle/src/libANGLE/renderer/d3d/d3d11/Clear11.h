//
// Copyright 2013 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// Clear11.h: Framebuffer clear utility class.

#ifndef LIBANGLE_RENDERER_D3D_D3D11_CLEAR11_H_
#define LIBANGLE_RENDERER_D3D_D3D11_CLEAR11_H_

#include <map>
#include <vector>

#include "libANGLE/Error.h"
#include "libANGLE/Framebuffer.h"
#include "libANGLE/angletypes.h"
#include "libANGLE/renderer/d3d/d3d11/renderer11_utils.h"

namespace rx
{
class Renderer11;
class RenderTarget11;
struct ClearParameters;

template <typename T>
struct RtvDsvClearInfo
{
    T r, g, b, a;
    float z;
    float c1padding[3];
};

class Clear11 : angle::NonCopyable
{
  public:
    explicit Clear11(Renderer11 *renderer);
    ~Clear11();

    // Clears the framebuffer with the supplied clear parameters, assumes that the framebuffer is
    // currently applied.
    angle::Result clearFramebuffer(const gl::Context *context,
                                   const ClearParameters &clearParams,
                                   const gl::FramebufferState &fboData);

  private:
    class ShaderManager final : angle::NonCopyable
    {
      public:
        ShaderManager();
        ~ShaderManager();
        angle::Result getShadersAndLayout(const gl::Context *context,
                                          Renderer11 *renderer,
                                          const INT clearType,
                                          const uint32_t numRTs,
                                          const bool hasLayeredLayout,
                                          const d3d11::InputLayout **il,
                                          const d3d11::VertexShader **vs,
                                          const d3d11::GeometryShader **gs,
                                          const d3d11::PixelShader **ps);

      private:
        constexpr static size_t kNumShaders = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;

        d3d11::LazyShader<ID3D11VertexShader> mVs;
        d3d11::LazyShader<ID3D11VertexShader> mVsMultiview;
        d3d11::LazyShader<ID3D11GeometryShader> mGsMultiview;
        d3d11::LazyShader<ID3D11PixelShader> mPsDepth;
        std::array<d3d11::LazyShader<ID3D11PixelShader>, kNumShaders> mPsFloat;
        std::array<d3d11::LazyShader<ID3D11PixelShader>, kNumShaders> mPsUInt;
        std::array<d3d11::LazyShader<ID3D11PixelShader>, kNumShaders> mPsSInt;

#if defined(ANGLE_ENABLE_WINDOWS_UWP)
        // GeckoW10m: a feature level 9_3 device refuses the shaders above, which are
        // vs_4_0/ps_4_0 only. These are built for level 9 (shaders/Clear11_FL9.hlsl) and take
        // the quad from the vertex buffer Clear11 keeps for that level, through mIl9.
        constexpr static size_t kNumShadersFL9 = D3D_FL9_3_SIMULTANEOUS_RENDER_TARGET_COUNT;

        d3d11::LazyInputLayout mIl9;
        d3d11::LazyShader<ID3D11VertexShader> mVs9;
        d3d11::LazyShader<ID3D11PixelShader> mPs9Depth;
        std::array<d3d11::LazyShader<ID3D11PixelShader>, kNumShadersFL9> mPs9Float;
#endif
    };

    bool useVertexBuffer() const;
    angle::Result ensureConstantBufferCreated(const gl::Context *context);
    angle::Result ensureVertexBufferCreated(const gl::Context *context);
    angle::Result ensureResourcesInitialized(const gl::Context *context);

    Renderer11 *mRenderer;
    bool mResourcesInitialized;

    // States
    d3d11::RasterizerState mScissorEnabledRasterizerState;
    d3d11::RasterizerState mScissorDisabledRasterizerState;
    gl::DepthStencilState mDepthStencilStateKey;
    d3d11::BlendStateKey mBlendStateKey;

    // Shaders and shader resources
    ShaderManager mShaderManager;
    d3d11::Buffer mConstantBuffer;
    d3d11::Buffer mVertexBuffer;

    // Buffer data and draw parameters
    RtvDsvClearInfo<float> mShaderData;
};

}  // namespace rx

#endif  // LIBANGLE_RENDERER_D3D_D3D11_CLEAR11_H_
