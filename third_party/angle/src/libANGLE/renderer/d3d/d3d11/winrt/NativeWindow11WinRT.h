//
// Copyright 2016 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// NativeWindow11WinRT.h: Implementation of NativeWindow11 for a XAML
// SwapChainPanel.
//
// Renderer11 already asks for this class when ANGLE_ENABLE_WINDOWS_UWP is set;
// only the files were missing, because the vendored copy of ANGLE carries just
// the win32 window -- Firefox is not shipped as a store app. This is the other
// half, cut down to the one case that exists on a phone.
//
// Upstream's version also handles an ICoreWindow and picks between the two at
// runtime. There is no CoreWindow to draw into here: an app container hands out
// no HWND, the browser has always been drawn into a XAML element, and that
// element is what this presents to.

#ifndef LIBANGLE_RENDERER_D3D_D3D11_WINRT_NATIVEWINDOW11WINRT_H_
#define LIBANGLE_RENDERER_D3D_D3D11_WINRT_NATIVEWINDOW11WINRT_H_

#include "libANGLE/renderer/d3d/d3d11/NativeWindow11.h"

struct ISwapChainPanelNative;
struct IInspectable;

namespace ABI
{
namespace Windows
{
namespace UI
{
namespace Core
{
struct ICoreDispatcher;
}
}  // namespace UI
}  // namespace Windows
}  // namespace ABI

namespace rx
{

// What the shell asked this build to do with the swap chain, and where to say
// what happened. Both live in NativeWindow11WinRT.cpp; SwapChain11 needs them
// too, and it is generic code that knows nothing of panels.
//
//   0  make the swap chain, hand it to the panel, present -- the real thing
//   1  make it, present, but never hand it to the panel
//   2  make it, hand it over, but never present
//
// The window is hidden by the system six tenths of a second after the swap
// chain exists, every time, and with nothing raised in between. These two
// halves are the only two things that happen in that gap.
int AngleUwpMode();
void AngleUwpNote(const char *text);

// The swap chain's buffers made once. The keyboard and the system's bars
// change the panel's size many times a minute, and stock ANGLE answers each
// change with ResizeBuffers: three new buffers of 13-14 MB each, wanted in one
// piece apiece, which a phone's address space may not have. The shell hands
// over the largest size the panel can have while the phone is held as it is
// (the window's), the buffers are made that size, and a smaller panel is shown
// through IDXGISwapChain2::SetSourceSize, which presents the top-left of them.
//
//   0  buffers the size of the panel, resized with it -- as it was
//   1  buffers at the capacity, the panel's part shown by source size
//   2  the same, and with every source size a transform that undoes a stretch
//      of the shown part to the buffers' size, should Windows do that
//
// The mode is 0 for as long as no capacity has been handed over.
int AngleUwpCapacityMode();
// False when there is none.
bool AngleUwpPanelCapacity(LONG *width, LONG *height);
// Shows width x height of the chain's buffers, and reads back what the chain
// then says its source size is. E_NOINTERFACE without IDXGISwapChain2.
HRESULT AngleUwpSetSourceSize(IDXGISwapChain *chain,
                              UINT width,
                              UINT height,
                              UINT bufferWidth,
                              UINT bufferHeight,
                              int mode,
                              UINT *readBackWidth,
                              UINT *readBackHeight);

class NativeWindow11WinRT : public NativeWindow11
{
  public:
    NativeWindow11WinRT(EGLNativeWindowType window, bool hasAlpha);
    ~NativeWindow11WinRT() override;

    bool initialize() override;
    bool getClientRect(LPRECT rect) const override;
    bool isIconic() const override;

    HRESULT createSwapChain(ID3D11Device *device,
                            IDXGIFactory *factory,
                            DXGI_FORMAT format,
                            UINT width,
                            UINT height,
                            UINT samples,
                            IDXGISwapChain **swapChain) override;

    void commitChange() override;

    static bool IsValidNativeWindow(EGLNativeWindowType window);

  private:
    // The panel this presents to, and the size it was last given. The size is
    // remembered rather than asked for because it belongs to the UI thread and
    // this is called from the renderer's.
    ISwapChainPanelNative *mPanel;
  // The thread the panel belongs to. Giving it a swap chain from any other
  // thread takes the system compositor down with it.
  ABI::Windows::UI::Core::ICoreDispatcher *mDispatcher;
    bool mHasAlpha;
    LONG mWidth;
    LONG mHeight;
};

}  // namespace rx

#endif  // LIBANGLE_RENDERER_D3D_D3D11_WINRT_NATIVEWINDOW11WINRT_H_
