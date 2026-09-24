/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_GECKOW10MPANELSWAPCHAIN_H
#define MOZILLA_GFX_GECKOW10MPANELSWAPCHAIN_H

#include <windows.h>

struct IDXGISwapChain1;

namespace mozilla::layers {

// Whether the shell has given the engine a XAML SwapChainPanel.
bool GeckoW10mHasPanel();

// Gives a composition swap chain to that panel, on the panel's own thread,
// and undoes the panel's composition scale so one buffer pixel is one screen
// pixel. See GeckoW10mPanelSwapChain.cpp.
HRESULT GeckoW10mAttachSwapChainToPanel(IDXGISwapChain1* aSwapChain);

}  // namespace mozilla::layers

#endif  // MOZILLA_GFX_GECKOW10MPANELSWAPCHAIN_H
