/**
 * @file lltoolfocus.cpp
 * @brief A tool to set the build focus point.
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

// File includes
#include "lltoolfocus.h"

// Library includes
#include "v3math.h"
#include "llfontgl.h"
#include "llui.h"
#include <unistd.h>  // For usleep()

// Viewer includes
#include "llagent.h"
#include "llagentcamera.h"
#include "llbutton.h"
#include "llviewercontrol.h"
#include "llviewerinput.h"//<FS:JL> Mouse movement by Singularity
#include "lldrawable.h"
#include <cstdlib>  // For getenv()
#include "lltooltip.h"
#include "llhudmanager.h"
#include "llfloatertools.h"
#include "llselectmgr.h"
#include "llstatusbar.h"
#include "lltoolmgr.h"
#include "llviewercamera.h"
#include "llviewerobject.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llmorphview.h"
#include "llfloaterreg.h"
#include "llfloatercamera.h"
#include "llmenugl.h"
#include "llwindowsdl2.h"

// Globals
bool gCameraBtnZoom = true;
bool gCameraBtnOrbit = false;
bool gCameraBtnPan = false;

const S32 SLOP_RANGE = 4;

//
// Camera - shared functionality
//

LLToolCamera::LLToolCamera()
:   LLTool(std::string("Camera")),
    mAccumX(0),
    mAccumY(0),
    mMouseDownX(0),
    mMouseDownY(0),
    mMouseDownWindowX(0),
    mMouseDownWindowY(0),
    mOutsideSlopX(false),
    mOutsideSlopY(false),
    mValidClickPoint(false),
    mClickPickPending(false),
    mValidSelection(false),
    mMouseSteering(false),
    mMouseUpX(0),
    mMouseUpY(0),
    mMouseUpMask(MASK_NONE)
{ }


LLToolCamera::~LLToolCamera()
{ }

// virtual
void LLToolCamera::handleSelect()
{
    if (gFloaterTools)
    {
        gFloaterTools->setStatusText("camera");
        // in case we start from tools floater, we count any selection as valid
        mValidSelection = gFloaterTools->getVisible();
    }
}

// virtual
void LLToolCamera::handleDeselect()
{
//  gAgent.setLookingAtAvatar(false);

    // Make sure that temporary selection won't pass anywhere except pie tool.
    MASK override_mask = gKeyboard ? gKeyboard->currentMask(true) : 0;
    if (!mValidSelection && (override_mask != MASK_NONE || (gFloaterTools && gFloaterTools->getVisible())))
    {
        LLMenuGL::sMenuContainer->hideMenus();
        LLSelectMgr::getInstance()->validateSelection();
    }
}

bool LLToolCamera::handleMouseDown(S32 x, S32 y, MASK mask)
{
    // Ensure a mouseup
    setMouseCapture(true);

    // Enable relative mouse mode for Alt+drag camera controls with window grab for desktop isolation
    if (mask & (MASK_ALT | MASK_ORBIT | MASK_PAN) || gCameraBtnOrbit || gCameraBtnPan || gCameraBtnZoom)
    {
        // Save cursor position before entering relative mode
        LLWindowSDL* sdl_window = dynamic_cast<LLWindowSDL*>(gViewerWindow->getWindow());
        if (sdl_window && sdl_window->getSDLWindow())
        {
            sdl_window->setRelativeModeState(true);
            SDL_SetRelativeMouseMode(SDL_TRUE);
            SDL_SetWindowGrab(sdl_window->getSDLWindow(), SDL_TRUE);
            SDL_RaiseWindow(sdl_window->getSDLWindow());
        }
        LL_DEBUGS("Mouse") << "Enabled relative mouse mode with window grab for camera controls" << LL_ENDL;
    }

    // call the base class to propogate info to sim
    LLTool::handleMouseDown(x, y, mask);

    mAccumX = 0;
    mAccumY = 0;

    mOutsideSlopX = false;
    mOutsideSlopY = false;

    mValidClickPoint = false;

    // Sometimes Windows issues down and up events near simultaneously
    // without giving async pick a chance to trigged
    // Ex: mouse from numlock emulation
    mClickPickPending = true;

    // If mouse capture gets ripped away, claim we moused up
    // at the point we moused down. JC
    mMouseUpX = x;
    mMouseUpY = y;
    mMouseUpMask = mask;

    // Only hide cursor manually if not using SDL relative mouse mode
    // (SDL relative mode handles cursor hiding automatically)
    if (!gViewerWindow->getWindow()->isInRelativeMouseMode())
    {
        gViewerWindow->hideCursor();
    }

    gViewerWindow->pickAsync(x, y, mask, pickCallback, /*bool pick_transparent*/ false, /*bool pick_rigged*/ false, /*bool pick_unselectable*/ true);

    return true;
}

void LLToolCamera::pickCallback(const LLPickInfo& pick_info)
{
    LLToolCamera* camera = LLToolCamera::getInstance();
    if (!camera->mClickPickPending)
    {
        return;
    }
    camera->mClickPickPending = false;

    // Save both screen and window coordinates for proper restoration
    S32 mouse_x, mouse_y;
    LLUI::getInstance()->getMousePositionScreen(&mouse_x, &mouse_y);
    camera->mMouseDownX = mouse_x;
    camera->mMouseDownY = mouse_y;
    
    // Also save window coordinates for XWayland cursor restoration
    LLCoordWindow window_pos;
    if (gViewerWindow->getWindow() && gViewerWindow->getWindow()->getCursorPosition(&window_pos))
    {
        camera->mMouseDownWindowX = window_pos.mX;
        camera->mMouseDownWindowY = window_pos.mY;
    }
    else
    {
        // Fallback to pick coordinates if cursor position unavailable
        camera->mMouseDownWindowX = pick_info.mMousePt.mX;
        camera->mMouseDownWindowY = pick_info.mMousePt.mY;
    }
    
    
    LL_INFOS("Camera") << "Saving initial mouse position - Screen: " 
                       << camera->mMouseDownX << ", " << camera->mMouseDownY 
                       << " Window: " << camera->mMouseDownWindowX << ", " << camera->mMouseDownWindowY
                       << " (pick_info was: " << pick_info.mMousePt.mX 
                       << ", " << pick_info.mMousePt.mY << ")" << LL_ENDL;

    // Hide cursor during camera operations (SDL relative mode will handle mouse capture)
    gViewerWindow->hideCursor();

    // Potentially recenter if click outside rectangle
    LLViewerObject* hit_obj = pick_info.getObject();

    // Check for hit the sky, or some other invalid point
    if (!hit_obj && pick_info.mPosGlobal.isExactlyZero())
    {
        camera->mValidClickPoint = false;
        return;
    }

    // check for hud attachments
    if (hit_obj && hit_obj->isHUDAttachment())
    {
        LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
        if (!selection->getObjectCount() || selection->getSelectType() != SELECT_TYPE_HUD)
        {
            camera->mValidClickPoint = false;
            return;
        }
    }

    if( CAMERA_MODE_CUSTOMIZE_AVATAR == gAgentCamera.getCameraMode() )
    {
        bool good_customize_avatar_hit = false;
        if( hit_obj )
        {
            if (isAgentAvatarValid() && (hit_obj == gAgentAvatarp))
            {
                // It's you
                good_customize_avatar_hit = true;
            }
            else if (hit_obj->isAttachment() && hit_obj->permYouOwner())
            {
                // It's an attachment that you're wearing
                good_customize_avatar_hit = true;
            }
        }

        if( !good_customize_avatar_hit )
        {
            camera->mValidClickPoint = false;
            return;
        }

        if( gMorphView )
        {
            gMorphView->setCameraDrivenByKeys( false );
        }
    }
    //RN: check to see if this is mouse-driving as opposed to ALT-zoom or Focus tool
    else if (pick_info.mKeyMask & MASK_ALT ||
            (LLToolMgr::getInstance()->getCurrentTool()->getName() == "Camera"))
    {
        LLViewerObject* hit_obj = pick_info.getObject();
        if (hit_obj)
        {
            // ...clicked on a world object, so focus at its position
            if (!hit_obj->isHUDAttachment())
            {
                gAgentCamera.setFocusOnAvatar(false, ANIMATE);
                gAgentCamera.setFocusGlobal(pick_info);
            }
        }
        else if (!pick_info.mPosGlobal.isExactlyZero())
        {
            // Hit the ground
            gAgentCamera.setFocusOnAvatar(false, ANIMATE);
            gAgentCamera.setFocusGlobal(pick_info);
        }

        bool zoom_tool = gCameraBtnZoom && (LLToolMgr::getInstance()->getBaseTool() == LLToolCamera::getInstance());
        // <FS:Ansariel> Replace frequently called gSavedSettings
        static LLCachedControl<bool> sFreezeTime(gSavedSettings, "FreezeTime");
        // </FS:Ansariel>
        if (!(pick_info.mKeyMask & MASK_ALT) &&
            !LLFloaterCamera::inFreeCameraMode() &&
            !zoom_tool &&
            gAgentCamera.cameraThirdPerson() &&
            gViewerWindow->getLeftMouseDown() &&
            // <FS:Ansariel> Replace frequently called gSavedSettings
            //!gSavedSettings.getBOOL("FreezeTime") &&
            !sFreezeTime &&
            // </FS:Ansariel>
            (hit_obj == gAgentAvatarp ||
             (hit_obj && hit_obj->isAttachment() && LLVOAvatar::findAvatarFromAttachment(hit_obj)->isSelf())))
        {
            LLToolCamera::getInstance()->mMouseSteering = true;
        }

    }

    camera->mValidClickPoint = true;

    if( CAMERA_MODE_CUSTOMIZE_AVATAR == gAgentCamera.getCameraMode() )
    {
        gAgentCamera.setFocusOnAvatar(false, false);

        LLVector3d cam_pos = gAgentCamera.getCameraPositionGlobal();

        gAgentCamera.setCameraPosAndFocusGlobal( cam_pos, pick_info.mPosGlobal, pick_info.mObjectID);
    }
}


// "Let go" of the mouse, for example on mouse up or when
// we lose mouse capture.  This ensures that cursor becomes visible
// if a modal dialog pops up during Alt-Zoom. JC
void LLToolCamera::releaseMouse()
{
    // Need to tell the sim that the mouse button is up, since this
    // tool is no longer working and cursor is visible (despite actual
    // mouse button status).
    LLTool::handleMouseUp(mMouseUpX, mMouseUpY, mMouseUpMask);

    gViewerWindow->showCursor();

    //for the situation when left click was performed on the Agent
    if (!LLFloaterCamera::inFreeCameraMode())
    {
        LLToolMgr::getInstance()->clearTransientTool();
    }

    // Simple XWayland fix: Show cursor when camera operation ends
    if (gViewerWindow->getWindow()->isRunningUnderXWayland())
    {
        gViewerWindow->showCursor();
    }

    mMouseSteering = false;
    mValidClickPoint = false;
    mOutsideSlopX = false;
    mOutsideSlopY = false;
}


bool LLToolCamera::handleMouseUp(S32 x, S32 y, MASK mask)
{
    // Claim that we're mousing up somewhere
    mMouseUpX = x;
    mMouseUpY = y;
    mMouseUpMask = mask;

    if (hasMouseCapture())
    {
        // Do not move camera if we haven't gotten a pick
        if (!mClickPickPending)
        {
            if (mValidClickPoint)
            {
                if (CAMERA_MODE_CUSTOMIZE_AVATAR == gAgentCamera.getCameraMode())
                {
                    LLCoordGL mouse_pos;
                    LLVector3 focus_pos = gAgent.getPosAgentFromGlobal(gAgentCamera.getFocusGlobal());
                    bool success = LLViewerCamera::getInstance()->projectPosAgentToScreen(focus_pos, mouse_pos);
                    if (success)
                    {
                        LLUI::getInstance()->setMousePositionScreen(mouse_pos.mX, mouse_pos.mY);
                    }
                }
                else
                {
                    // Restore cursor to original position after camera operation
                    LLUI::getInstance()->setMousePositionScreen(mMouseDownX, mMouseDownY);
                }
            }
            else
            {
                // not a valid zoomable object - restore cursor to original position
                LLUI::getInstance()->setMousePositionScreen(mMouseDownX, mMouseDownY);
            }
        }

        // Disable relative mouse mode and window grab for camera controls
        LLWindowSDL* sdl_window = dynamic_cast<LLWindowSDL*>(gViewerWindow->getWindow());
        if (sdl_window && sdl_window->getSDLWindow())
        {
            SDL_SetWindowGrab(sdl_window->getSDLWindow(), SDL_FALSE);
            SDL_SetRelativeMouseMode(SDL_FALSE);
            sdl_window->setRelativeModeState(false);
        }
        
        LL_DEBUGS("Mouse") << "Disabled relative mouse mode and window grab for camera controls" << LL_ENDL;

        // calls releaseMouse() internally
        setMouseCapture(false);
    }
    else
    {
        releaseMouse();
    }

    return true;
}

// XWayland detection removed - now handled at core level in llviewerwindow.cpp

static bool right_hold_mouse_walk = false;//<FS:JL> Mouse movement by Singularity

bool LLToolCamera::handleHover(S32 x, S32 y, MASK mask)
{
    //<FS:JL> Mouse movement by Singularity
    if (right_hold_mouse_walk)
    {
        agent_push_forward(KEYSTATE_LEVEL);
    }
    //</FS:JL>

    S32 dx = gViewerWindow->getCurrentMouseDX();
    S32 dy = gViewerWindow->getCurrentMouseDY();

    LL_DEBUGS("UserInput") << "LLToolCamera::handleHover: dx=" << dx << " dy=" << dy 
                          << " mask=0x" << std::hex << mask << std::dec
                          << " gCameraBtnZoom=" << gCameraBtnZoom 
                          << " gCameraBtnOrbit=" << gCameraBtnOrbit 
                          << " gCameraBtnPan=" << gCameraBtnPan << LL_ENDL;

    if (hasMouseCapture() && mValidClickPoint)
    {
        mAccumX += llabs(dx);
        mAccumY += llabs(dy);

        if (mAccumX >= SLOP_RANGE)
        {
            mOutsideSlopX = true;
        }

        if (mAccumY >= SLOP_RANGE)
        {
            mOutsideSlopY = true;
        }
    }

    if (mOutsideSlopX || mOutsideSlopY)
    {
        if (!mValidClickPoint)
        {
            LL_DEBUGS("UserInput") << "hover handled by LLToolFocus [invalid point]" << LL_ENDL;
            gViewerWindow->setCursor(UI_CURSOR_NO);
            gViewerWindow->showCursor();
            return true;
        }

        if (gCameraBtnOrbit ||
            mask == MASK_ORBIT ||
            mask == (MASK_ALT | MASK_ORBIT))
        {
            // Orbit tool
            if (hasMouseCapture())
            {
                F32 RADIANS_PER_PIXEL = 360.f * DEG_TO_RAD / gViewerWindow->getWorldViewWidthScaled();
                
                // Increase sensitivity for relative mouse mode (XWayland)
                if (gViewerWindow->getWindow() && gViewerWindow->getWindow()->isInRelativeMouseMode())
                {
                    RADIANS_PER_PIXEL *= 10.0f; // Increase sensitivity for relative mode
                }

                if (dx != 0)
                {
                    gAgentCamera.cameraOrbitAround( -dx * RADIANS_PER_PIXEL );
                }

                if (dy != 0)
                {
                    gAgentCamera.cameraOrbitOver( -dy * RADIANS_PER_PIXEL );
                }
            }
            LL_DEBUGS("UserInput") << "hover handled by LLToolFocus [active]" << LL_ENDL;
        }
        else if (   gCameraBtnPan ||
                    mask == MASK_PAN ||
                    mask == (MASK_PAN | MASK_ALT) )
        {
            // Pan tool
            if (hasMouseCapture())
            {
                LLVector3d camera_to_focus = gAgentCamera.getCameraPositionGlobal();
                camera_to_focus -= gAgentCamera.getFocusGlobal();
                F32 dist = (F32) camera_to_focus.normVec();

                // Adjusted panning sensitivity to match mouselook feel
                // Further reduced from 1.5 to 0.8 for much slower, more precise panning
                F32 pan_sensitivity = 0.8f;
                
                // Apply additional reduction in relative mouse mode for consistency
                if (gViewerWindow->getWindow() && gViewerWindow->getWindow()->isInRelativeMouseMode())
                {
                    pan_sensitivity *= 0.6f; // Further reduce for relative mode
                }
                
                F32 meters_per_pixel = pan_sensitivity * dist / gViewerWindow->getWorldViewWidthScaled();

                if (dx != 0)
                {
                    gAgentCamera.cameraPanLeft( dx * meters_per_pixel );
                }

                if (dy != 0)
                {
                    gAgentCamera.cameraPanUp( -dy * meters_per_pixel );
                }
            }
            LL_DEBUGS("UserInput") << "hover handled by LLToolPan" << LL_ENDL;
        }
        else if (gCameraBtnZoom)
        {
            // Zoom tool
            LL_DEBUGS("UserInput") << "LLToolZoom: dx=" << dx << " dy=" << dy 
                                  << " mOutsideSlopY=" << mOutsideSlopY 
                                  << " mMouseSteering=" << mMouseSteering << LL_ENDL;
            if (hasMouseCapture())
            {

                F32 RADIANS_PER_PIXEL = 360.f * DEG_TO_RAD / gViewerWindow->getWorldViewWidthScaled();
                
                // Increase sensitivity for relative mouse mode (XWayland)
                if (gViewerWindow->getWindow() && gViewerWindow->getWindow()->isInRelativeMouseMode())
                {
                    RADIANS_PER_PIXEL *= 10.0f; // Increase sensitivity for relative mode
                }

                if (dx != 0)
                {
                    LL_DEBUGS("UserInput") << "LLToolZoom: cameraOrbitAround dx=" << dx << LL_ENDL;
                    gAgentCamera.cameraOrbitAround( -dx * RADIANS_PER_PIXEL );
                }

                const F32 IN_FACTOR = 0.99f;

                if (dy != 0 && mOutsideSlopY )
                {
                    if (mMouseSteering)
                    {
                        LL_DEBUGS("UserInput") << "LLToolZoom: cameraOrbitOver dy=" << dy << LL_ENDL;
                        gAgentCamera.cameraOrbitOver( -dy * RADIANS_PER_PIXEL );
                    }
                    else
                    {
                        // Apply sensitivity multiplier for zoom in relative mode
                        F32 zoom_sensitivity = 1.0f;
                        if (gViewerWindow->getWindow() && gViewerWindow->getWindow()->isInRelativeMouseMode())
                        {
                            zoom_sensitivity = 3.0f; // Increase zoom sensitivity for relative mode
                        }
                        
                        LL_DEBUGS("UserInput") << "LLToolZoom: cameraZoomIn dy=" << dy 
                                              << " sensitivity=" << zoom_sensitivity << LL_ENDL;
                        gAgentCamera.cameraZoomIn((F32)pow( IN_FACTOR, dy * zoom_sensitivity ) );
                    }
                }
                else if (dy != 0)
                {
                    LL_DEBUGS("UserInput") << "LLToolZoom: dy=" << dy << " but mOutsideSlopY=" << mOutsideSlopY << LL_ENDL;
                }
            }

            LL_DEBUGS("UserInput") << "hover handled by LLToolZoom" << LL_ENDL;
        }
    }

    if (gCameraBtnOrbit ||
        mask == MASK_ORBIT ||
        mask == (MASK_ALT | MASK_ORBIT))
    {
        gViewerWindow->setCursor(UI_CURSOR_TOOLCAMERA);
    }
    else if (   gCameraBtnPan ||
                mask == MASK_PAN ||
                mask == (MASK_PAN | MASK_ALT) )
    {
        gViewerWindow->setCursor(UI_CURSOR_TOOLPAN);
    }
    else
    {
        gViewerWindow->setCursor(UI_CURSOR_TOOLZOOMIN);
    }

    return true;
}

//<FS:JL> Mouse movement by Singularity
bool LLToolCamera::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    if (mMouseSteering)
    {
        agent_push_forward(KEYSTATE_DOWN);
        right_hold_mouse_walk = true;
        return true;
    }
    else
    {
        return false;
    }
}

bool LLToolCamera::handleRightMouseUp(S32 x, S32 y, MASK mask)
{
    if (mMouseSteering || right_hold_mouse_walk)
    {
        agent_push_forward(KEYSTATE_UP);
        right_hold_mouse_walk = false;
        return true;
    }
    else
    {
        return false;
    }
}
//</FS:JL>

void LLToolCamera::onMouseCaptureLost()
{
    // Disable relative mouse mode and window grab when capture is lost
    LLWindowSDL* sdl_window = dynamic_cast<LLWindowSDL*>(gViewerWindow->getWindow());
    if (sdl_window && sdl_window->getSDLWindow())
    {
        SDL_SetWindowGrab(sdl_window->getSDLWindow(), SDL_FALSE);
        SDL_SetRelativeMouseMode(SDL_FALSE);
        sdl_window->setRelativeModeState(false);
    }
    if (sdl_window)
    {
        sdl_window->setRelativeModeState(false);
    }
    
    LL_DEBUGS("Mouse") << "Disabled relative mouse mode and window grab on capture lost" << LL_ENDL;

    releaseMouse();
    // <FS:Ansariel> Mouse movement by Singularity
    handleRightMouseUp(0,0,0);
}
