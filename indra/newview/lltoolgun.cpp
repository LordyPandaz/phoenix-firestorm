/**
 * @file lltoolgun.cpp
 * @brief LLToolGun class implementation
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "lltoolgun.h"

#include "llviewerwindow.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llviewercontrol.h"
#include "llsky.h"
#include "llappviewer.h"
#include "llresmgr.h"
#include "llfontgl.h"
#include "llui.h"
#include "llviewertexturelist.h"
#include "llviewercamera.h"
// [RLVa:KB] - Checked: 2014-02-24 (RLVa-1.4.10)
#include "llfocusmgr.h"
// [/RLVa:KB]
#include "llhudmanager.h"
#include "lltoolmgr.h"
#include "lltoolgrab.h"
#include "lluiimage.h"
// Linden library includes
#include "llwindow.h"           // setMouseClipping()
#include "llwindowsdl2.h"       // setCameraControlActive()

LLToolGun::LLToolGun( LLToolComposite* composite )
:   LLTool( std::string("gun"), composite ),
        mIsSelected(false),
        mSmoothedDeltaX(0.0f),
        mSmoothedDeltaY(0.0f)
{
    // <FS:Ansariel> Performance tweak
    mCrosshairp = LLUI::getUIImage("crosshairs.tga");
}

void LLToolGun::handleSelect()
{
// [RLVa:KB] - Checked: 2014-02-24 (RLVa-1.4.10)
    if (gFocusMgr.getAppHasFocus())
    {
// [/RLVa:KB]
        gViewerWindow->hideCursor();
        gViewerWindow->moveCursorToCenter();
        gViewerWindow->getWindow()->setMouseClipping(true);
        
        // Enable relative mouse mode for mouselook with window grab for desktop isolation
        LLWindowSDL* sdl_window = dynamic_cast<LLWindowSDL*>(gViewerWindow->getWindow());
        if (sdl_window && sdl_window->getSDLWindow())
        {
            sdl_window->setRelativeModeState(true);
            SDL_SetRelativeMouseMode(SDL_TRUE);
            SDL_SetWindowGrab(sdl_window->getSDLWindow(), SDL_TRUE);
            SDL_RaiseWindow(sdl_window->getSDLWindow());
            LL_DEBUGS("Mouse") << "Enabled relative mouse mode with window grab for mouselook" << LL_ENDL;
        }
        
        mIsSelected = true;
        
        // Reset smoothing values to prevent initial jump
        mSmoothedDeltaX = 0.0f;
        mSmoothedDeltaY = 0.0f;
// [RLVa:KB] - Checked: 2014-02-24 (RLVa-1.4.10)
    }
// [/RLVa:KB]
}

void LLToolGun::handleDeselect()
{
    gViewerWindow->moveCursorToCenter();
    gViewerWindow->showCursor();
    
    // Disable relative mouse mode and window grab for mouselook
    LLWindowSDL* sdl_window = dynamic_cast<LLWindowSDL*>(gViewerWindow->getWindow());
    if (sdl_window && sdl_window->getSDLWindow())
    {
        SDL_SetWindowGrab(sdl_window->getSDLWindow(), SDL_FALSE);
        SDL_SetRelativeMouseMode(SDL_FALSE);
        sdl_window->setRelativeModeState(false);
        LL_DEBUGS("Mouse") << "Disabled relative mouse mode and window grab for mouselook" << LL_ENDL;
    }
    
    gViewerWindow->getWindow()->setMouseClipping(false);
    mIsSelected = false;
}

bool LLToolGun::handleMouseDown(S32 x, S32 y, MASK mask)
{
    gGrabTransientTool = this;
    LLToolMgr::getInstance()->getCurrentToolset()->selectTool( LLToolGrab::getInstance() );

    return LLToolGrab::getInstance()->handleMouseDown(x, y, mask);
}

bool LLToolGun::handleHover(S32 x, S32 y, MASK mask)
{
    if( gAgentCamera.cameraMouselook() && mIsSelected )
    {
        const F32 NOMINAL_MOUSE_SENSITIVITY = 0.0025f;

        // <FS:Ansariel> Use faster LLCachedControl
        //F32 mouse_sensitivity = gSavedSettings.getF32("MouseSensitivity");
        static LLCachedControl<F32> mouseSensitivity(gSavedSettings, "MouseSensitivity");
        F32 mouse_sensitivity = (F32)mouseSensitivity;
        // </FS:Ansariel> Use faster LLCachedControl
        mouse_sensitivity = clamp_rescale(mouse_sensitivity, 0.f, 15.f, 0.5f, 2.75f) * NOMINAL_MOUSE_SENSITIVITY;

        // Increase sensitivity for relative mouse mode (XWayland)
        if (gViewerWindow->getWindow() && gViewerWindow->getWindow()->isInRelativeMouseMode())
        {
            mouse_sensitivity *= 3.5f; // Optimized for smooth control with movement smoothing
        }

        // ...move the view with the mouse

        // get mouse movement delta
        S32 dx = -gViewerWindow->getCurrentMouseDX();
        S32 dy = gViewerWindow->getCurrentMouseDY();

        LL_DEBUGS("MouseLook") << "Raw mouse deltas: dx=" << dx << ", dy=" << dy << LL_ENDL;

        if (dx != 0 || dy != 0)
        {
            // ...actually moved off center
            // Clamp deltas to prevent jumps from accumulated values
            const S32 MAX_MOUSE_DELTA = 300; // Increased for smoother fast movements
            S32 original_dx = dx, original_dy = dy;
            dx = llclamp(dx, -MAX_MOUSE_DELTA, MAX_MOUSE_DELTA);
            dy = llclamp(dy, -MAX_MOUSE_DELTA, MAX_MOUSE_DELTA);
            
            if (dx != original_dx || dy != original_dy)
            {
                LL_WARNS("MouseLook") << "Mouse delta clamped from (" << original_dx << "," << original_dy 
                                     << ") to (" << dx << "," << dy << ")" << LL_ENDL;
            }
            
            // Apply exponential moving average smoothing for jitter reduction
            const F32 SMOOTHING_ALPHA = 0.35f; // Balance between smoothness and responsiveness
            mSmoothedDeltaX = SMOOTHING_ALPHA * (F32)dx + (1.0f - SMOOTHING_ALPHA) * mSmoothedDeltaX;
            mSmoothedDeltaY = SMOOTHING_ALPHA * (F32)dy + (1.0f - SMOOTHING_ALPHA) * mSmoothedDeltaY;
            
            // <FS:Ansariel> Use faster LLCachedControl
            //if (gSavedSettings.getBOOL("InvertMouse"))
            static LLCachedControl<bool> invertMouse(gSavedSettings, "InvertMouse");
            F32 pitch_amount = mouse_sensitivity * (invertMouse ? mSmoothedDeltaY : -mSmoothedDeltaY);
            F32 rotate_amount = mouse_sensitivity * mSmoothedDeltaX;
            
            LL_DEBUGS("MouseLook") << "Raw deltas: (" << dx << "," << dy << ")"
                                  << ", Smoothed: (" << mSmoothedDeltaX << "," << mSmoothedDeltaY << ")"
                                  << ", sensitivity: " << mouse_sensitivity 
                                  << ", pitch_amount: " << pitch_amount 
                                  << ", rotate_amount: " << rotate_amount << LL_ENDL;
            
            gAgent.pitch(pitch_amount);
            LLVector3 skyward = gAgent.getReferenceUpVector();
            gAgent.rotate(rotate_amount, skyward.mV[VX], skyward.mV[VY], skyward.mV[VZ]);
            
            LL_DEBUGS("MouseLook") << "Agent pitch applied: " << pitch_amount 
                                  << ", rotate applied: " << rotate_amount << LL_ENDL;

            // <FS:Ansariel> Use faster LLCachedControl
            //if (gSavedSettings.getBOOL("MouseSun"))
            static LLCachedControl<bool> mouseSun(gSavedSettings, "MouseSun");
            if (mouseSun)
            {
                LLVector3 sunpos = LLViewerCamera::getInstance()->getAtAxis();
                gSky.setSunDirectionCFR(sunpos);
                gSavedSettings.setVector3("SkySunDefaultPosition", LLViewerCamera::getInstance()->getAtAxis());
            }

            if (gSavedSettings.getBOOL("MouseMoon"))
            {
                LLVector3 moonpos = LLViewerCamera::getInstance()->getAtAxis();
                gSky.setMoonDirectionCFR(moonpos);
                gSavedSettings.setVector3("SkyMoonDefaultPosition", LLViewerCamera::getInstance()->getAtAxis());
            }

            gViewerWindow->moveCursorToCenter();
            gViewerWindow->hideCursor();
        }

        LL_DEBUGS("UserInput") << "hover handled by LLToolGun (mouselook)" << LL_ENDL;
    }
    else
    {
        LL_DEBUGS("UserInput") << "hover handled by LLToolGun (not mouselook)" << LL_ENDL;
    }

    // HACK to avoid assert: error checking system makes sure that the cursor is set during every handleHover.  This is actually a no-op since the cursor is hidden.
    gViewerWindow->setCursor(UI_CURSOR_ARROW);

    return true;
}

void LLToolGun::draw()
{
    // <FS:Ansariel> Use faster LLCachedControl
    //if( gSavedSettings.getBOOL("ShowCrosshairs") )
    static LLCachedControl<bool> showCrosshairs(gSavedSettings, "ShowCrosshairs");
    if (showCrosshairs)
    {
        // <FS:Ansariel> Performance tweak
        //LLUIImagePtr crosshair = LLUI::getUIImage("crosshairs.tga");
        //crosshair->draw(
        //  ( gViewerWindow->getWorldViewRectScaled().getWidth() - crosshair->getWidth() ) / 2,
        //  ( gViewerWindow->getWorldViewRectScaled().getHeight() - crosshair->getHeight() ) / 2);
        mCrosshairp->draw(
            ( gViewerWindow->getWorldViewRectScaled().getWidth() - mCrosshairp->getWidth() ) / 2,
            ( gViewerWindow->getWorldViewRectScaled().getHeight() - mCrosshairp->getHeight() ) / 2);
        // </FS:Ansariel> Performance tweak
    }
}
