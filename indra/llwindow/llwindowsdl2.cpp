/**
 * @file llwindowsdl.cpp
 * @brief SDL implementation of LLWindow class
 * @author This module has many fathers, and it shows.
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

#if LL_SDL2

#include "linden_common.h"

#include "llwindowsdl.h"

#include "llwindowcallbacks.h"
#include "llkeyboardsdl.h"

#include "llerror.h"
#include "llgl.h"
#include "llstring.h"
#include "lldir.h"
#include "llfindlocale.h"
#include "llframetimer.h"

// For XWayland fractional scaling detection
#include <cstdio>
#include <cstring>
#include <string>
#include <exception>
#include <algorithm>

// if there is a better methood to get at the settings from llwindow/ let me know! -Zi
#include "llcontrol.h"
extern LLControlGroup gSavedSettings;


#ifdef LL_GLIB
#include <glib.h>
#endif

extern "C" {
# include "fontconfig/fontconfig.h"
}

#if LL_LINUX
// not necessarily available on random SDL platforms, so #if LL_LINUX
// for execv(), waitpid(), fork()
# include <unistd.h>
# include <sys/types.h>
# include <sys/wait.h>
# include <stdio.h>
#endif // LL_LINUX

extern bool gDebugWindowProc;

const S32 MAX_NUM_RESOLUTIONS = 200;

// static variable for ATI mouse cursor crash work-around:
static bool ATIbug = false;

// XWayland grab operation timeout (5 seconds)
const F64 LLWindowSDL::GRAB_TIMEOUT = 5.0;

//
// LLWindowSDL
//

#if LL_X11
# include <X11/Xutil.h>
#endif //LL_X11

// TOFU HACK -- (*exactly* the same hack as LLWindowMacOSX for a similar
// set of reasons): Stash a pointer to the LLWindowSDL object here and
// maintain in the constructor and destructor.  This assumes that there will
// be only one object of this class at any time.  Currently this is true.
static LLWindowSDL *gWindowImplementation = NULL;

// extern "C" Bool XineramaIsActive (Display *dpy)
// {
//  return 0;
// }
void maybe_lock_display(void)
{
    if (gWindowImplementation && gWindowImplementation->Lock_Display) {
        gWindowImplementation->Lock_Display();
    }
}


void maybe_unlock_display(void)
{
    if (gWindowImplementation && gWindowImplementation->Unlock_Display) {
        gWindowImplementation->Unlock_Display();
    }
}


#if LL_X11
// static
Window LLWindowSDL::get_SDL_XWindowID(void)
{
    if (gWindowImplementation) {
        return gWindowImplementation->mSDL_XWindowID;
    }
    return None;
}

//static
Display* LLWindowSDL::get_SDL_Display(void)
{
    if (gWindowImplementation) {
        return gWindowImplementation->mSDL_Display;
    }
    return NULL;
}
#endif // LL_X11

#if LL_X11

// Clipboard handing via native X11, base on the implementation in Cool VL by Henri Beauchamp

namespace
{
    std::array<Atom, 3> gSupportedAtoms;

    Atom XA_CLIPBOARD;
    Atom XA_TARGETS;
    Atom PVT_PASTE_BUFFER;
    // Unused in the current clipboard implementation -Zi
    // long const MAX_PASTE_BUFFER_SIZE = 16383;

    void filterSelectionRequest( XEvent aEvent )
    {
        auto *display = LLWindowSDL::getSDLDisplay();
        auto &request = aEvent.xselectionrequest;

        XSelectionEvent reply { SelectionNotify, aEvent.xany.serial, aEvent.xany.send_event, display,
                                request.requestor, request.selection, request.target,
                                request.property,request.time };

        if (request.target == XA_TARGETS)
        {
            XChangeProperty(display, request.requestor, request.property,
                            XA_ATOM, 32, PropModeReplace,
                            (unsigned char *) &gSupportedAtoms.front(), gSupportedAtoms.size());
        }
        else if (std::find(gSupportedAtoms.begin(), gSupportedAtoms.end(), request.target) !=
                 gSupportedAtoms.end())
        {
            std::string utf8;
            if (request.selection == XA_PRIMARY)
                utf8 = wstring_to_utf8str(gWindowImplementation->getPrimaryText());
            else
                utf8 = wstring_to_utf8str(gWindowImplementation->getSecondaryText());

            XChangeProperty(display, request.requestor, request.property,
                            request.target, 8, PropModeReplace,
                            (unsigned char *) utf8.c_str(), utf8.length());
        }
        else if (request.selection == XA_CLIPBOARD)
        {
            // Did not have what they wanted, so no property set
            reply.property = None;
        }
        else
            return;

        XSendEvent(request.display, request.requestor, False, NoEventMask, (XEvent *) &reply);
        XSync(display, False);
    }

    void filterSelectionClearRequest( XEvent aEvent )
    {
        auto &request = aEvent.xselectionrequest;
        if (request.selection == XA_PRIMARY)
            gWindowImplementation->clearPrimaryText();
        else if (request.selection == XA_CLIPBOARD)
            gWindowImplementation->clearSecondaryText();
    }

    int x11_clipboard_filter(void*, SDL_Event *evt)
    {
        Display *display = LLWindowSDL::getSDLDisplay();
        if (!display)
            return 1;

        if (evt->type != SDL_SYSWMEVENT)
            return 1;

        auto xevent = evt->syswm.msg->msg.x11.event;

        if (xevent.type == SelectionRequest)
            filterSelectionRequest( xevent );
        else if (xevent.type == SelectionClear)
            filterSelectionClearRequest( xevent );
        return 1;
    }

    bool grab_property(Display* display, Window window, Atom selection, Atom target)
    {
        if( !display )
            return false;

        maybe_lock_display();

        XDeleteProperty(display, window, PVT_PASTE_BUFFER);
        XFlush(display);

        XConvertSelection(display, selection, target, PVT_PASTE_BUFFER, window,  CurrentTime);

        // Unlock the connection so that the SDL event loop may function
        maybe_unlock_display();

        const auto start{ SDL_GetTicks() };
        const auto end{ start + 1000 };

        XEvent xevent {};
        bool response = false;

        do
        {
            SDL_Event event {};

            // Wait for an event
            SDL_WaitEvent(&event);

            // If the event is a window manager event
            if (event.type == SDL_SYSWMEVENT)
            {
                xevent = event.syswm.msg->msg.x11.event;

                if (xevent.type == SelectionNotify && xevent.xselection.requestor == window)
                    response = true;
            }
        } while (!response && SDL_GetTicks() < end );

        return response && xevent.xselection.property != None;
    }
}

void LLWindowSDL::initialiseX11Clipboard()
{
    if (!mSDL_Display)
        return;

    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
    SDL_SetEventFilter(x11_clipboard_filter, nullptr);

    maybe_lock_display();

    XA_CLIPBOARD = XInternAtom(mSDL_Display, "CLIPBOARD", False);

    gSupportedAtoms[0] = XInternAtom(mSDL_Display, "UTF8_STRING", False);
    gSupportedAtoms[1] = XInternAtom(mSDL_Display, "COMPOUND_TEXT", False);
    gSupportedAtoms[2] = XA_STRING;

    // TARGETS atom
    XA_TARGETS = XInternAtom(mSDL_Display, "TARGETS", False);

    // SL_PASTE_BUFFER atom
    PVT_PASTE_BUFFER = XInternAtom(mSDL_Display, "FS_PASTE_BUFFER", False);

    maybe_unlock_display();
}

bool LLWindowSDL::getSelectionText( Atom aSelection, Atom aType, LLWString &text )
{
    if( !mSDL_Display )
        return false;

    if( !grab_property(mSDL_Display, mSDL_XWindowID, aSelection,aType ) )
        return false;

    maybe_lock_display();

    Atom type;
    int format {};
    unsigned long len {}, size {};
    unsigned char* data = nullptr;

    // get type and size of the clipboard contents first
    XGetWindowProperty( mSDL_Display, mSDL_XWindowID,
                        PVT_PASTE_BUFFER, 0, 0, False,
                        AnyPropertyType, &type, &format, &len,
                        &size, &data);
    XFree(data);

    // now get the real data, we don't really have a size limit here, but we need
    // to tell the X11 clipboard how much space we have, which happens to be exactly
    // the size of the current clipboard contents
    unsigned long remaining {};
    int res = XGetWindowProperty(mSDL_Display, mSDL_XWindowID,
                                 PVT_PASTE_BUFFER, 0, size, False,
                                 AnyPropertyType, &type, &format, &len,
                                 &remaining, &data);
    if (data && len)
    {
        text = LLWString(
                utf8str_to_wstring(reinterpret_cast< char const *>( data ) )
                );
        XFree(data);
    }

    maybe_unlock_display();
    return res == Success;
}

bool LLWindowSDL::getSelectionText(Atom selection, LLWString& text)
{
    if (!mSDL_Display)
        return false;

    maybe_lock_display();

    Window owner = XGetSelectionOwner(mSDL_Display, selection);
    if (owner == None)
    {
        if (selection == XA_PRIMARY)
        {
            owner = DefaultRootWindow(mSDL_Display);
            selection = XA_CUT_BUFFER0;
        }
        else
        {
            maybe_unlock_display();
            return false;
        }
    }

    maybe_unlock_display();

    for( Atom atom : gSupportedAtoms )
    {
        if(getSelectionText(selection, atom, text ) )
            return true;
    }

    return false;
}

bool LLWindowSDL::setSelectionText(Atom selection, const LLWString& text)
{
    maybe_lock_display();

    if (selection == XA_PRIMARY)
    {
        std::string utf8 = wstring_to_utf8str(text);
        XStoreBytes(mSDL_Display, utf8.c_str(), utf8.length() + 1);
        mPrimaryClipboard = text;
    }
    else
        mSecondaryClipboard = text;

    XSetSelectionOwner(mSDL_Display, selection, mSDL_XWindowID, CurrentTime);

    auto owner = XGetSelectionOwner(mSDL_Display, selection);

    maybe_unlock_display();

    return owner == mSDL_XWindowID;
}

Display* LLWindowSDL::getSDLDisplay()
{
    if (gWindowImplementation)
        return gWindowImplementation->mSDL_Display;
    return nullptr;
}

#endif


LLWindowSDL::LLWindowSDL(LLWindowCallbacks* callbacks,
             const std::string& title, S32 x, S32 y, S32 width,
             S32 height, U32 flags,
             bool fullscreen, bool clearBg,
             bool enable_vsync, bool use_gl,
             // <FS:LO> Legacy cursor setting from main program
             //bool ignore_pixel_depth, U32 fsaa_samples,)
             bool ignore_pixel_depth, U32 fsaa_samples, bool useLegacyCursors)
    : LLWindow(callbacks, fullscreen, flags),
      Lock_Display(NULL),
      //Unlock_Display(NULL), mGamma(1.0f)
      Unlock_Display(NULL), mGamma(1.0f),
      mUseLegacyCursors(useLegacyCursors), // </FS:LO>
      mRunningUnderXWayland(false),  // Initialize XWayland detection flag
      mSavedCursorPos(-1, -1),       // Initialize cursor tracking to invalid position
      mInGrabOperation(false),       // Not in grab operation initially
      mSkipNextWarpEvent(false),     // Don't skip warp events initially  
      mLastUngrabTime(0.0),          // No previous ungrab operation
      mGrabOperationStartTime(0.0),  // No grab operation started
      mLastValidMousePos(0, 0),      // No last valid mouse position
      mRelativeDeltaX(0),            // No relative motion initially
      mRelativeDeltaY(0),            // No relative motion initially
      mRelativeModeActive(false),    // Relative mode not active initially
      mPreRelativeModePos(0, 0),     // No saved position initially
      mRestoreCursorOnModeChange(true), // Enable cursor restoration by default
      mXWaylandDPIScale(1.0f),       // Default DPI scale
      mDetectedDPI(96),              // Default DPI
      mDPIScalingInitialized(false), // DPI not yet initialized
      mXWaylandTempCursorHidden(false), // No temporary cursor hide initially
      mDPIScaleX(1.0f),              // Default DPI scale X
      mDPIScaleY(1.0f),              // Default DPI scale Y
      mDrawableWidth(0),             // No drawable size initially
      mDrawableHeight(0),            // No drawable size initially
      mXWaylandFractionalScaling(false), // XWayland fractional scaling not detected initially
      mWaylandScaleFactor(1.0f),     // Default Wayland scale factor
      mCoordinateCompensationX(1.0f), // Default X coordinate compensation
      mCoordinateCompensationY(1.0f)  // Default Y coordinate compensation
{
    // Initialize the keyboard
    gKeyboard = new LLKeyboardSDL();
    gKeyboard->setCallbacks(callbacks);
    // Note that we can't set up key-repeat until after SDL has init'd video

    // Ignore use_gl for now, only used for drones on PC
    mWindow = NULL;
    mContext = {};
    mNeedsResize = false;
    mOverrideAspectRatio = 0.f;
    mGrabbyKeyFlags = 0;
    mReallyCapturedCount = 0;
    mHaveInputFocus = -1;
    mIsMinimized = -1;
    mFSAASamples = fsaa_samples;

    // IME - International input compositing, i.e. for Japanese / Chinese text input
    // Preeditor means here the actual XUI input field currently in use
    mIMEEnabled = false;
    mPreeditor = nullptr;

    // Initialize VSync reliability tracking
    mVSyncEnabled = enable_vsync;
    mCurrentSwapInterval = 0;
    mLastVerifiedSwapInterval = 0;
    mLastVSyncVerifyTime = 0.0;
    
    // Initialize Frame Pacing System
    initializeFramePacing();
    
    // Initialize Multi-Monitor VSync Support
    initializeMultiMonitorVSync();

#if LL_X11
    mSDL_XWindowID = None;
    mSDL_Display = NULL;
#endif // LL_X11

    // Assume 4:3 aspect ratio until we know better
    mOriginalAspectRatio = 1024.0 / 768.0;

    if (title.empty())
        mWindowTitle = "SDL Window";  // *FIX: (?)
    else
        mWindowTitle = title;

    // Create the GL context and set it up for windowed or fullscreen, as appropriate.
    if(createContext(x, y, width, height, 32, fullscreen, enable_vsync))
    {
        gGLManager.initGL();

        //start with arrow cursor
        initCursors(useLegacyCursors); // <FS:LO> Legacy cursor setting from main program
        setCursor( UI_CURSOR_ARROW );
    }

    stop_glerror();

    // Stash an object pointer for OSMessageBox()
    gWindowImplementation = this;

#if LL_X11
    mFlashing = false;
    initialiseX11Clipboard();
#endif // LL_X11

    mKeyVirtualKey = 0;
    mKeyModifiers = KMOD_NONE;
}

static SDL_Surface *Load_BMP_Resource(const char *basename)
{
    const int PATH_BUFFER_SIZE=1000;
    char path_buffer[PATH_BUFFER_SIZE]; /* Flawfinder: ignore */

    // Figure out where our BMP is living on the disk
    snprintf(path_buffer, PATH_BUFFER_SIZE-1, "%s%sres-sdl%s%s",
         gDirUtilp->getAppRODataDir().c_str(),
         gDirUtilp->getDirDelimiter().c_str(),
         gDirUtilp->getDirDelimiter().c_str(),
         basename);
    path_buffer[PATH_BUFFER_SIZE-1] = '\0';

    return SDL_LoadBMP(path_buffer);
}

#if LL_X11
// This is an XFree86/XOrg-specific hack for detecting the amount of Video RAM
// on this machine.  It works by searching /var/log/var/log/Xorg.?.log or
// /var/log/XFree86.?.log for a ': (VideoRAM ?|Memory): (%d+) kB' regex, where
// '?' is the X11 display number derived from $DISPLAY
static int x11_detect_VRAM_kb_fp(FILE *fp, const char *prefix_str)
{
    const int line_buf_size = 1000;
    char line_buf[line_buf_size];
    while (fgets(line_buf, line_buf_size, fp))
    {
        //LL_DEBUGS() << "XLOG: " << line_buf << LL_ENDL;

        // Why the ad-hoc parser instead of using a regex?  Our
        // favourite regex implementation - libboost_regex - is
        // quite a heavy and troublesome dependency for the client, so
        // it seems a shame to introduce it for such a simple task.
        // *FIXME: libboost_regex is a dependency now anyway, so we may
        // as well use it instead of this hand-rolled nonsense.
        const char *part1_template = prefix_str;
        const char part2_template[] = " kB";
        char *part1 = strstr(line_buf, part1_template);
        if (part1) // found start of matching line
        {
            part1 = &part1[strlen(part1_template)]; // -> after
            char *part2 = strstr(part1, part2_template);
            if (part2) // found end of matching line
            {
                // now everything between part1 and part2 is
                // supposed to be numeric, describing the
                // number of kB of Video RAM supported
                int rtn = 0;
                for (; part1 < part2; ++part1)
                {
                    if (*part1 < '0' || *part1 > '9')
                    {
                        // unexpected char, abort parse
                        rtn = 0;
                        break;
                    }
                    rtn *= 10;
                    rtn += (*part1) - '0';
                }
                if (rtn > 0)
                {
                    // got the kB number.  return it now.
                    return rtn;
                }
            }
        }
    }
    return 0; // 'could not detect'
}

static int x11_detect_VRAM_kb()
{
    std::string x_log_location("/var/log/");
    std::string fname;
    int rtn = 0; // 'could not detect'
    int display_num = 0;
    FILE *fp;
    char *display_env = getenv("DISPLAY"); // e.g. :0 or :0.0 or :1.0 etc
    // parse DISPLAY number so we can go grab the right log file
    if (display_env[0] == ':' &&
        display_env[1] >= '0' && display_env[1] <= '9')
    {
        display_num = display_env[1] - '0';
    }

    // *TODO: we could be smarter and see which of Xorg/XFree86 has the
    // freshest time-stamp.

    // Try Xorg log first
    fname = x_log_location;
    fname += "Xorg.";
    fname += ('0' + display_num);
    fname += ".log";
    fp = fopen(fname.c_str(), "r");
    if (fp)
    {
        LL_INFOS() << "Looking in " << fname
            << " for VRAM info..." << LL_ENDL;
        rtn = x11_detect_VRAM_kb_fp(fp, ": VideoRAM: ");
        fclose(fp);
        if (0 == rtn)
        {
            fp = fopen(fname.c_str(), "r");
            if (fp)
            {
                rtn = x11_detect_VRAM_kb_fp(fp, ": Video RAM: ");
                fclose(fp);
                if (0 == rtn)
                {
                    fp = fopen(fname.c_str(), "r");
                    if (fp)
                    {
                        rtn = x11_detect_VRAM_kb_fp(fp, ": Memory: ");
                        fclose(fp);
                    }
                }
            }
        }
    }
    else
    {
        LL_INFOS() << "Could not open " << fname
            << " - skipped." << LL_ENDL;
        // Try old XFree86 log otherwise
        fname = x_log_location;
        fname += "XFree86.";
        fname += ('0' + display_num);
        fname += ".log";
        fp = fopen(fname.c_str(), "r");
        if (fp)
        {
            LL_INFOS() << "Looking in " << fname
                << " for VRAM info..." << LL_ENDL;
            rtn = x11_detect_VRAM_kb_fp(fp, ": VideoRAM: ");
            fclose(fp);
            if (0 == rtn)
            {
                fp = fopen(fname.c_str(), "r");
                if (fp)
                {
                    rtn = x11_detect_VRAM_kb_fp(fp, ": Memory: ");
                    fclose(fp);
                }
            }
        }
        else
        {
            LL_INFOS() << "Could not open " << fname
                << " - skipped." << LL_ENDL;
        }
    }
    return rtn;
}
#endif // LL_X11

void LLWindowSDL::setTitle(const std::string &title)
{
    SDL_SetWindowTitle( mWindow, title.c_str() );
}

void LLWindowSDL::tryFindFullscreenSize( int &width, int &height )
{
    LL_INFOS() << "createContext: setting up fullscreen " << width << "x" << height << LL_ENDL;

    // If the requested width or height is 0, find the best default for the monitor.
    if((width == 0) || (height == 0))
    {
        // Scan through the list of modes, looking for one which has:
        //      height between 700 and 800
        //      aspect ratio closest to the user's original mode
        S32 resolutionCount = 0;
        LLWindowResolution *resolutionList = getSupportedResolutions(resolutionCount);

        if(resolutionList != NULL)
        {
            F32 closestAspect = 0;
            U32 closestHeight = 0;
            U32 closestWidth = 0;
            int i;

            LL_INFOS() << "createContext: searching for a display mode, original aspect is " << mOriginalAspectRatio << LL_ENDL;

            for(i=0; i < resolutionCount; i++)
            {
                F32 aspect = (F32)resolutionList[i].mWidth / (F32)resolutionList[i].mHeight;

                LL_INFOS() << "createContext: width " << resolutionList[i].mWidth << " height " << resolutionList[i].mHeight << " aspect " << aspect << LL_ENDL;

                if( (resolutionList[i].mHeight >= 700) && (resolutionList[i].mHeight <= 800) &&
                    (fabs(aspect - mOriginalAspectRatio) < fabs(closestAspect - mOriginalAspectRatio)))
                {
                    LL_INFOS() << " (new closest mode) " << LL_ENDL;

                    // This is the closest mode we've seen yet.
                    closestWidth = resolutionList[i].mWidth;
                    closestHeight = resolutionList[i].mHeight;
                    closestAspect = aspect;
                }
            }

            width = closestWidth;
            height = closestHeight;
        }
    }

    if((width == 0) || (height == 0))
    {
        // Mode search failed for some reason.  Use the old-school default.
        width = 1024;
        height = 768;
    }
}

bool LLWindowSDL::createContext(int x, int y, int width, int height, int bits, bool fullscreen, bool enable_vsync)
{
    //bool          glneedsinit = false;

    LL_INFOS() << "createContext, fullscreen=" << fullscreen <<
        " size=" << width << "x" << height << LL_ENDL;

    // captures don't survive contexts
    mGrabbyKeyFlags = 0;
    mReallyCapturedCount = 0;

    SDL_SetHint( SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0" );
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");

    // IME - International input compositing, i.e. for Japanese / Chinese text input
    // Request the IME interface to show over-the-top compositing while typing
    mIMEEnabled = gSavedSettings.getBOOL("SDL2IMEEnabled");

    if (mIMEEnabled)
    {
        SDL_SetHint( SDL_HINT_IME_INTERNAL_EDITING, "1");
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO ) < 0 )
    {
        LL_INFOS() << "sdl_init() failed! " << SDL_GetError() << LL_ENDL;
        setupFailure("sdl_init() failure,  window creation error", "error", OSMB_OK);
        return false;
    }
    
    // Enable Wayland mouse warp emulation for compatibility with older games
    SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_EMULATE_MOUSE_WARP, "1");
    LL_INFOS("SDL") << "Enabled Wayland mouse warp emulation hint" << LL_ENDL;
    
    // Disable warp motion events to prevent interference with relative mouse mode
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_WARP_MOTION, "0");
    LL_INFOS("SDL") << "Disabled relative warp motion events for better XWayland support" << LL_ENDL;

    SDL_version c_sdl_version;
    SDL_VERSION(&c_sdl_version);
    LL_INFOS() << "Compiled against SDL "
        << int(c_sdl_version.major) << "."
        << int(c_sdl_version.minor) << "."
        << int(c_sdl_version.patch) << LL_ENDL;
    SDL_version r_sdl_version;
    SDL_GetVersion(&r_sdl_version);
    LL_INFOS() << " Running against SDL "
        << int(r_sdl_version.major) << "."
        << int(r_sdl_version.minor) << "."
        << int(r_sdl_version.patch) << LL_ENDL;

    if (width == 0)
        width = 1024;
    if (height == 0)
        width = 768;

    mFullscreen = fullscreen;

    int sdlflags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;

    if( mFullscreen )
    {
        sdlflags |= SDL_WINDOW_FULLSCREEN;
        tryFindFullscreenSize( width, height );
    }

    mSDLFlags = sdlflags;

    GLint redBits{8}, greenBits{8}, blueBits{8}, alphaBits{8};

    GLint depthBits{(bits <= 16) ? 16 : 24}, stencilBits{8};

    if (getenv("LL_GL_NO_STENCIL"))
        stencilBits = 0;

    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, alphaBits);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE,   redBits);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, greenBits);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,  blueBits);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, depthBits );

    // We need stencil support for a few (minor) things.
    if (stencilBits)
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, stencilBits);

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    if (mFSAASamples > 0)
    {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, mFSAASamples);
    }

    // <FS:Zi> Make shared context work on Linux for multithreaded OpenGL
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    mWindow = SDL_CreateWindow( mWindowTitle.c_str(), SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height, mSDLFlags );

    if( mWindow )
    {
        mContext = SDL_GL_CreateContext( mWindow );

        if( mContext == 0 )
        {
            LL_WARNS() << "Cannot create GL context " << SDL_GetError() << LL_ENDL;
            close();
            setupFailure("GL Context creation error", "Error", OSMB_OK);
            return false;
        }

        if (SDL_GL_MakeCurrent(mWindow, mContext) != 0)
        {
            LL_WARNS() << "Failed to make context current. SDL: " << SDL_GetError() << LL_ENDL;
            close();
            setupFailure("GL Context failed to set current failure", "Error", OSMB_OK);
            return false;
        }

        // FIRE-32559: This *should* work, but for some reason aftrer login vsync always acts as if it's disabled, so
        // the flag will get set again later in void LLViewerWindow::setStartupComplete() -Zi
        toggleVSync(enable_vsync);

        mSurface = SDL_GetWindowSurface( mWindow );
        
        // Calculate DPI scaling factors
        updateDPIScaling();
        
        // Detect XWayland fractional scaling and setup coordinate compensation
        detectXWaylandScaling();
    }


    if( mFullscreen )
    {
        if (mSurface)
        {
            mFullscreen = true;
            mFullscreenWidth = mSurface->w;
            mFullscreenHeight = mSurface->h;
            mFullscreenRefresh = -1;

            LL_INFOS() << "Running at " << mFullscreenWidth
                << "x"   << mFullscreenHeight
                << " @ " << mFullscreenRefresh
                << LL_ENDL;
        }
        else
        {
            LL_WARNS() << "createContext: fullscreen creation failure. SDL: " << SDL_GetError() << LL_ENDL;
            // No fullscreen support
            mFullscreen = false;
            mFullscreenWidth   = -1;
            mFullscreenHeight  = -1;
            mFullscreenRefresh = -1;

            std::string error = llformat("Unable to run fullscreen at %d x %d.\nRunning in window.", width, height);
            OSMessageBox(error, "Error", OSMB_OK);
            return false;
        }
    }
    else
    {
        if (!mWindow)
        {
            LL_WARNS() << "createContext: window creation failure. SDL: " << SDL_GetError() << LL_ENDL;
            setupFailure("Window creation error", "Error", OSMB_OK);
            return false;
        }
    }

    // Set the application icon.
    SDL_Surface *bmpsurface;
    bmpsurface = Load_BMP_Resource("firestorm_icon.BMP");
    if (bmpsurface)
    {
        SDL_SetWindowIcon(mWindow, bmpsurface);
        SDL_FreeSurface(bmpsurface);
        bmpsurface = NULL;
    }

    // Detect video memory size.
# if LL_X11
    gGLManager.mVRAM = x11_detect_VRAM_kb() / 1024;
    if (gGLManager.mVRAM != 0)
    {
        LL_INFOS() << "X11 log-parser detected " << gGLManager.mVRAM << "MB VRAM." << LL_ENDL;
    } else
# endif // LL_X11
    {
        // fallback to letting SDL detect VRAM.
        // note: I've not seen SDL's detection ever actually find
        // VRAM != 0, but if SDL *does* detect it then that's a bonus.
        gGLManager.mVRAM = 0;
        if (gGLManager.mVRAM != 0)
        {
            LL_INFOS() << "SDL detected " << gGLManager.mVRAM << "MB VRAM." << LL_ENDL;
        }
    }
    // If VRAM is not detected, that is handled later

    // *TODO: Now would be an appropriate time to check for some
    // explicitly unsupported cards.
    //const char* RENDERER = (const char*) glGetString(GL_RENDERER);

    SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &redBits);
    SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &greenBits);
    SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &blueBits);
    SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &alphaBits);
    SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &depthBits);
    SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &stencilBits);

    LL_INFOS() << "GL buffer:" << LL_ENDL;
    LL_INFOS() << "  Red Bits " << S32(redBits) << LL_ENDL;
    LL_INFOS() << "  Green Bits " << S32(greenBits) << LL_ENDL;
    LL_INFOS() << "  Blue Bits " << S32(blueBits) << LL_ENDL;
    LL_INFOS() << "  Alpha Bits " << S32(alphaBits) << LL_ENDL;
    LL_INFOS() << "  Depth Bits " << S32(depthBits) << LL_ENDL;
    LL_INFOS() << "  Stencil Bits " << S32(stencilBits) << LL_ENDL;

    GLint colorBits = redBits + greenBits + blueBits + alphaBits;
    // fixme: actually, it's REALLY important for picking that we get at
    // least 8 bits each of red,green,blue.  Alpha we can be a bit more
    // relaxed about if we have to.
    if (colorBits < 32)
    {
        close();
        setupFailure(
            "Second Life requires True Color (32-bit) to run in a window.\n"
            "Please go to Control Panels -> Display -> Settings and\n"
            "set the screen to 32-bit color.\n"
            "Alternately, if you choose to run fullscreen, Second Life\n"
            "will automatically adjust the screen each time it runs.",
            "Error",
            OSMB_OK);
        return false;
    }

#if LL_X11
    /* Grab the window manager specific information */
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if ( SDL_GetWindowWMInfo(mWindow, &info) )
    {
        /* Save the information for later use */
        if ( info.subsystem == SDL_SYSWM_X11 )
        {
            mSDL_Display = info.info.x11.display;
            mSDL_XWindowID = info.info.x11.window;
            
            // Detect if we're running under XWayland compatibility layer
            const char* wayland_display = getenv("WAYLAND_DISPLAY");
            const char* session_type = getenv("XDG_SESSION_TYPE");
            
            if ((wayland_display && *wayland_display) || 
                (session_type && strcmp(session_type, "wayland") == 0))
            {
                LL_INFOS("Window") << "Running under XWayland compatibility layer" << LL_ENDL;
                mRunningUnderXWayland = true;
            }
            else
            {
                LL_INFOS("Window") << "Running on native X11" << LL_ENDL;
                mRunningUnderXWayland = false;
            }
            
            // Use warp-to-center approach for XWayland camera controls
            if (mRunningUnderXWayland)
            {
                // Use warp-to-center approach for better compatibility
                LL_INFOS("XWayland") << "Using warp-to-center approach for camera controls" << LL_ENDL;
            }
        }
        else
        {
            LL_WARNS() << "We're not running under X11?  Wild."
                << LL_ENDL;
        }
    }
    else
    {
        LL_WARNS() << "We're not running under any known WM.  Wild."
            << LL_ENDL;
    }
#endif // LL_X11

    // clear screen to black right at the start so it doesn't look like a crash
    glClearColor(0.0f, 0.0f, 0.0f ,1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(mWindow);

    // start text input immediately when IME is not enabled
    if (!mIMEEnabled)
    {
        SDL_StartTextInput();
    }

    //make sure multisampling is disabled by default
    glDisable(GL_MULTISAMPLE_ARB);

    // Don't need to get the current gamma, since there's a call that restores it to the system defaults.
    return true;
}


// changing fullscreen resolution, or switching between windowed and fullscreen mode.
bool LLWindowSDL::switchContext(bool fullscreen, const LLCoordScreen &size, bool enable_vsync, const LLCoordScreen * const posp)
{
    const bool needsRebuild = true;  // Just nuke the context and start over.
    bool result = true;

    LL_INFOS() << "switchContext, fullscreen=" << fullscreen << LL_ENDL;
    
    // XWayland-specific fullscreen handling
    if (mRunningUnderXWayland && fullscreen)
    {
        LL_INFOS("XWayland") << "Preparing fullscreen transition for XWayland" << LL_ENDL;
        
        // Force sync before fullscreen transition
        if (mSDL_Display)
        {
            XSync(mSDL_Display, False);
            usleep(10000);  // 10ms delay for compositor sync
        }
    }
    
    stop_glerror();
    if(needsRebuild)
    {
        destroyContext();
        result = createContext(0, 0, size.mX, size.mY, 0, fullscreen, enable_vsync);
        if (result)
        {
            gGLManager.initGL();

            //start with arrow cursor
            initCursors(mUseLegacyCursors); // <FS:LO> Legacy cursor setting from main program
            setCursor( UI_CURSOR_ARROW );
            
            // XWayland post-context setup
            if (mRunningUnderXWayland)
            {
                LL_DEBUGS("XWayland") << "Post-context XWayland setup" << LL_ENDL;
                
                if (fullscreen)
                {
                    // Ensure fullscreen state is properly established
                    SDL_SetWindowFullscreen(mWindow, SDL_WINDOW_FULLSCREEN);
                }
                
                // Force window to front and focus
                SDL_RaiseWindow(mWindow);
                SDL_SetWindowInputFocus(mWindow);
                
                // Additional sync for compositor
                if (mSDL_Display)
                {
                    XSync(mSDL_Display, False);
                }
            }
        }
    }

    stop_glerror();

    return result;
}

void LLWindowSDL::destroyContext()
{
    LL_INFOS() << "destroyContext begins" << LL_ENDL;

    SDL_StopTextInput();
#if LL_X11
    mSDL_Display = NULL;
    mSDL_XWindowID = None;
    Lock_Display = NULL;
    Unlock_Display = NULL;
#endif // LL_X11

    // Clean up remaining GL state before blowing away window
    LL_INFOS() << "shutdownGL begins" << LL_ENDL;
    gGLManager.shutdownGL();
    LL_INFOS() << "SDL_QuitSS/VID begins" << LL_ENDL;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);  // *FIX: this might be risky...

    mWindow = NULL;
}

LLWindowSDL::~LLWindowSDL()
{
    quitCursors();
    destroyContext();

    if(mSupportedResolutions != NULL)
    {
        delete []mSupportedResolutions;
    }

    gWindowImplementation = NULL;
}

void LLWindowSDL::initializeXWaylandDPIScaling()
{
    if (mDPIScalingInitialized || !mRunningUnderXWayland)
        return;
    
    mDPIScalingInitialized = true;
    
#if LL_X11
    if (!mSDL_Display)
        return;
        
    // Get screen dimensions in pixels and millimeters
    int screen = DefaultScreen(mSDL_Display);
    int screen_width_px = DisplayWidth(mSDL_Display, screen);
    int screen_height_px = DisplayHeight(mSDL_Display, screen);
    int screen_width_mm = DisplayWidthMM(mSDL_Display, screen);
    int screen_height_mm = DisplayHeightMM(mSDL_Display, screen);
    
    if (screen_width_mm > 0 && screen_height_mm > 0)
    {
        // Calculate actual DPI
        F32 dpi_x = (F32)screen_width_px * 25.4f / (F32)screen_width_mm;
        F32 dpi_y = (F32)screen_height_px * 25.4f / (F32)screen_height_mm;
        F32 avg_dpi = (dpi_x + dpi_y) / 2.0f;
        
        mDetectedDPI = (S32)avg_dpi;
        
        // Calculate scaling factor (96 DPI is the baseline)
        mXWaylandDPIScale = avg_dpi / 96.0f;
        
        // Clamp scaling to reasonable range
        mXWaylandDPIScale = llclamp(mXWaylandDPIScale, 0.5f, 3.0f);
        
        LL_INFOS("XWayland") << "Detected DPI: " << avg_dpi 
                            << " (x:" << dpi_x << " y:" << dpi_y << ")"
                            << ", scale factor: " << mXWaylandDPIScale << LL_ENDL;
    }
    else
    {
        LL_WARNS("XWayland") << "Unable to detect screen DPI, using defaults" << LL_ENDL;
    }
    
    // Also check for environment variables that might indicate HiDPI
    const char* gdk_scale = getenv("GDK_SCALE");
    const char* qt_scale = getenv("QT_SCALE_FACTOR");
    
    if (gdk_scale)
    {
        F32 env_scale = atof(gdk_scale);
        if (env_scale > 0.0f && env_scale != mXWaylandDPIScale)
        {
            LL_INFOS("XWayland") << "GDK_SCALE environment override: " << env_scale << LL_ENDL;
            mXWaylandDPIScale = env_scale;
        }
    }
    
    if (qt_scale)
    {
        F32 env_scale = atof(qt_scale);
        if (env_scale > 0.0f && env_scale != mXWaylandDPIScale)
        {
            LL_INFOS("XWayland") << "QT_SCALE_FACTOR environment override: " << env_scale << LL_ENDL;
            mXWaylandDPIScale = env_scale;
        }
    }
#endif // LL_X11
}

#if LL_X11
bool LLWindowSDL::validateGrabState()
{
    if (!mRunningUnderXWayland || !mSDL_Display)
        return true; // Not applicable or no display
        
    // Check if we can get focus information (basic validation)
    Window focused_window;
    int revert_to;
    XGetInputFocus(mSDL_Display, &focused_window, &revert_to);
    
    // Simple validation: if we think we're grabbing but window isn't focused, something's wrong
    bool window_has_focus = (focused_window == mSDL_XWindowID || focused_window == PointerRoot);
    bool state_claims_grab = mInGrabOperation;
    
    if (state_claims_grab && !window_has_focus && mHaveInputFocus)
    {
        LL_WARNS("XWayland") << "Grab state mismatch detected - we claim to be grabbing but window not focused" << LL_ENDL;
        forceGrabReset();
        return false;
    }
    
    // Check for stale grab operations (timeout)
    if (mInGrabOperation && (LLFrameTimer::getTotalTime() - mGrabOperationStartTime) > GRAB_TIMEOUT)
    {
        LL_WARNS("XWayland") << "Grab operation timeout (" << GRAB_TIMEOUT << "s) - forcing reset" << LL_ENDL;
        forceGrabReset();
        return false;
    }
    
    return true;
}

void LLWindowSDL::forceGrabReset()
{
    if (!mRunningUnderXWayland || !mSDL_Display)
        return;
        
    LL_INFOS("XWayland") << "Forcing grab state reset" << LL_ENDL;
    
    // Force ungrab everything
    maybe_lock_display();
    XUngrabPointer(mSDL_Display, CurrentTime);
    XUngrabKeyboard(mSDL_Display, CurrentTime);
    XSync(mSDL_Display, False);
    maybe_unlock_display();
    
    // Reset all grab-related state
    mInGrabOperation = false;
    mGrabbyKeyFlags = 0;
    mSkipNextWarpEvent = false;
    mReallyCapturedCount = 0;
    mGrabOperationStartTime = 0.0;
}
#endif // LL_X11

void LLWindowSDL::show()
{
    // *FIX: What to do with SDL?
}

void LLWindowSDL::hide()
{
    // *FIX: What to do with SDL?
}

//virtual
void LLWindowSDL::minimize()
{
    // *FIX: What to do with SDL?
}

//virtual
void LLWindowSDL::restore()
{
    // *FIX: What to do with SDL?
}


// close() destroys all OS-specific code associated with a window.
// Usually called from LLWindowManager::destroyWindow()
void LLWindowSDL::close()
{
    // Is window is already closed?
    //  if (!mWindow)
    //  {
    //      return;
    //  }

    // Make sure cursor is visible and we haven't mangled the clipping state.
    setMouseClipping(false);
    showCursor();

    destroyContext();
}

bool LLWindowSDL::isValid()
{
    return (mWindow != NULL);
}

bool LLWindowSDL::getVisible()
{
    bool result = false;

    // *FIX: This isn't really right...
    // Then what is?
    if (mWindow)
    {
        result = true;
    }

    return(result);
}

bool LLWindowSDL::getMinimized()
{
    bool result = false;

    if (mWindow && (1 == mIsMinimized))
    {
        result = true;
    }
    return(result);
}

bool LLWindowSDL::getMaximized()
{
    bool result = false;

    if (mWindow)
    {
        // TODO
    }

    return(result);
}

bool LLWindowSDL::maximize()
{
    // TODO
    return false;
}

bool LLWindowSDL::getFullscreen()
{
    return mFullscreen;
}

bool LLWindowSDL::getPosition(LLCoordScreen *position)
{
    int x;
    int y;

    SDL_GetWindowPosition(mWindow, &x, &y);

    position->mX = x;
    position->mY = y;

    return true;
}

bool LLWindowSDL::getSize(LLCoordScreen *size)
{
    if (mWindow && size)
    {
        // For screen coordinates, use actual drawable size (physical pixels)
        if (mDrawableWidth > 0 && mDrawableHeight > 0)
        {
            size->mX = mDrawableWidth;
            size->mY = mDrawableHeight;
        }
        else
        {
            // Fallback if DPI scaling not initialized
            SDL_GL_GetDrawableSize(mWindow, &size->mX, &size->mY);
        }
        return true;
    }

    return false;
}

bool LLWindowSDL::getSize(LLCoordWindow *size)
{
    if (mWindow)
    {
        int window_w, window_h;
        SDL_GetWindowSize(mWindow, &window_w, &window_h);
        size->mX = window_w;
        size->mY = window_h;
        return (true);
    }

    return (false);
}

bool LLWindowSDL::setPosition(const LLCoordScreen position)
{
    if (!mWindow)
        return false;
    
    // XWayland window positioning may be ignored by Wayland compositors
    if (mRunningUnderXWayland)
    {
        LL_INFOS("XWayland") << "Attempting window position: " << position.mX << ", " << position.mY 
                            << " (may be ignored by compositor)" << LL_ENDL;
                            
        // Try SDL positioning first
        SDL_SetWindowPosition(mWindow, position.mX, position.mY);
        
        // Also try X11 positioning as backup
        if (mSDL_Display && mSDL_XWindowID)
        {
            XMoveWindow(mSDL_Display, mSDL_XWindowID, position.mX, position.mY);
            XSync(mSDL_Display, False);
        }
        
        // Note: Wayland compositors may ignore positioning requests for security
        // This is expected behavior, not a bug
        return true;
    }
    
    // Standard X11 positioning
    SDL_SetWindowPosition(mWindow, position.mX, position.mY);
    return true;
}

template< typename T > bool setSizeImpl( const T& newSize, SDL_Window *pWin )
{
    if( !pWin )
        return false;

    auto nFlags = SDL_GetWindowFlags( pWin );

    if( nFlags & SDL_WINDOW_MAXIMIZED )
        SDL_RestoreWindow( pWin );


    SDL_SetWindowSize( pWin, newSize.mX, newSize.mY );
    SDL_Event event;
    event.type = SDL_WINDOWEVENT;
    event.window.event = SDL_WINDOWEVENT_RESIZED;
    event.window.windowID = SDL_GetWindowID( pWin );
    event.window.data1 = newSize.mX;
    event.window.data2 = newSize.mY;
    SDL_PushEvent( &event );

    return true;
}

bool LLWindowSDL::setSizeImpl(const LLCoordScreen size)
{
    return ::setSizeImpl( size, mWindow );
}

bool LLWindowSDL::setSizeImpl(const LLCoordWindow size)
{
    return ::setSizeImpl( size, mWindow );
}


void LLWindowSDL::swapBuffers()
{
    if (mWindow)
    {
        SDL_GL_SwapWindow( mWindow );
    }
}

U32 LLWindowSDL::getFSAASamples()
{
    return mFSAASamples;
}

void LLWindowSDL::setFSAASamples(const U32 samples)
{
    mFSAASamples = samples;
}

F32 LLWindowSDL::getGamma()
{
    return 1/mGamma;
}

bool LLWindowSDL::restoreGamma()
{
    //CGDisplayRestoreColorSyncSettings();
    // SDL_SetGamma(1.0f, 1.0f, 1.0f);
    return true;
}

bool LLWindowSDL::setGamma(const F32 gamma)
{
    mGamma = gamma;
    if (mGamma == 0) mGamma = 0.1f;
    mGamma = 1/mGamma;
    // SDL_SetGamma(mGamma, mGamma, mGamma);
    return true;
}

bool LLWindowSDL::isCursorHidden()
{
    return mCursorHidden;
}




// Constrains the mouse to the window.
void LLWindowSDL::setMouseClipping( bool b )
{
    if (b)
    {
        // Use window grab for proper desktop isolation, especially important for GNOME hot corners
        SDL_SetWindowGrab(mWindow, SDL_TRUE);
        SDL_ShowCursor(SDL_DISABLE);
        LL_DEBUGS("Mouse") << "Mouse clipping enabled with window grab (relative mode: " << (SDL_GetRelativeMouseMode() ? "yes" : "no") << ")" << LL_ENDL;
    }
    else
    {
        SDL_SetWindowGrab(mWindow, SDL_FALSE);
        SDL_ShowCursor(SDL_ENABLE);
        LL_DEBUGS("Mouse") << "Mouse clipping disabled" << LL_ENDL;
    }
}

// virtual
void LLWindowSDL::setMinSize(U32 min_width, U32 min_height, bool enforce_immediately)
{
    LLWindow::setMinSize(min_width, min_height, enforce_immediately);

#if LL_X11
    // Set the minimum size limits for X11 window
    // so the window manager doesn't allow resizing below those limits.
    XSizeHints* hints = XAllocSizeHints();
    hints->flags |= PMinSize;
    hints->min_width = mMinWindowWidth;
    hints->min_height = mMinWindowHeight;

    XSetWMNormalHints(mSDL_Display, mSDL_XWindowID, hints);

    XFree(hints);
#endif
}

bool LLWindowSDL::setCursorPosition(const LLCoordWindow position)
{
    bool result = true;
    
    // Skip cursor warping when in relative mouse mode (cursor is hidden anyway)
    if (SDL_GetRelativeMouseMode() || mRelativeModeActive)
    {
        return true;  // Pretend success - relative mode handles motion differently
    }

    // SDL_WarpMouseInWindow expects window coordinates, not screen coordinates
    // Apply DPI scaling to convert logical coordinates to physical window coordinates
    S32 scaled_x = (S32)(position.mX * mDPIScaleX);
    S32 scaled_y = (S32)(position.mY * mDPIScaleY);

    LL_DEBUGS("DPI") << "setCursorPosition logical: " << position.mX << ", " << position.mY 
                     << " -> scaled: " << scaled_x << ", " << scaled_y 
                     << " (scale: " << mDPIScaleX << "x" << mDPIScaleY << ")" << LL_ENDL;

    // XWayland requires hide-warp-show sequence for proper mouse warping
    // This is the critical fix that Blender also had to implement
    if (mRunningUnderXWayland)
    {
        // Save current cursor visibility state - check both permanent and temporary states
        bool cursor_was_visible = !mCursorHidden && !mXWaylandTempCursorHidden;
        
        // XWayland workaround: hide cursor before warping
        if (cursor_was_visible)
        {
            mXWaylandTempCursorHidden = true;
            SDL_ShowCursor(SDL_DISABLE);
        }
        
        SDL_FlushEvent(SDL_MOUSEMOTION);
        SDL_WarpMouseInWindow(mWindow, scaled_x, scaled_y);
        SDL_FlushEvent(SDL_MOUSEMOTION);
        
        // Restore cursor visibility if it was visible before
        if (cursor_was_visible)
        {
            SDL_ShowCursor(SDL_ENABLE);
            mXWaylandTempCursorHidden = false;
        }
        
        LL_DEBUGS("XWayland") << "Used hide-warp-show sequence for cursor position: " 
                             << scaled_x << ", " << scaled_y << LL_ENDL;
    }
    else
    {
        // Standard warping for native Wayland/X11
        SDL_FlushEvent(SDL_MOUSEMOTION);
        SDL_WarpMouseInWindow(mWindow, scaled_x, scaled_y);
        SDL_FlushEvent(SDL_MOUSEMOTION);
    }

    //LL_INFOS() << llformat("llcw %d,%d -> scr %d,%d", position.mX, position.mY, screen_pos.mX, screen_pos.mY) << LL_ENDL;

    return result;
}

bool LLWindowSDL::getCursorPosition(LLCoordWindow *position)
{
    if (!position)
        return false;

    int x, y;
    SDL_GetMouseState(&x, &y);

    // SDL_GetMouseState returns coordinates relative to the window in physical pixels
    // Convert to logical window coordinates (LLCoordWindow uses logical pixels)
    position->mX = (S32)(x / mDPIScaleX);
    position->mY = (S32)(y / mDPIScaleY);

    LL_DEBUGS("DPI") << "getCursorPosition SDL physical: " << x << ", " << y 
                     << " -> logical: " << position->mX << ", " << position->mY
                     << " (scale: " << mDPIScaleX << "x" << mDPIScaleY << ")" << LL_ENDL;

    return true;
}

void LLWindowSDL::saveCursorPositionForRelativeMode()
{
    if (!mRelativeModeActive && mRestoreCursorOnModeChange)
    {
        LLCoordWindow current_pos;
        if (getCursorPosition(&current_pos))
        {
            mPreRelativeModePos = current_pos;
            LL_DEBUGS("Mouse") << "Saved cursor position before relative mode: (" 
                              << mPreRelativeModePos.mX << ", " << mPreRelativeModePos.mY << ")" << LL_ENDL;
        }
    }
}

void LLWindowSDL::restoreCursorPositionFromRelativeMode()
{
    if (mRelativeModeActive && mRestoreCursorOnModeChange && 
        mPreRelativeModePos.mX >= 0 && mPreRelativeModePos.mY >= 0)
    {
        LL_DEBUGS("Mouse") << "Restoring cursor position after relative mode: (" 
                          << mPreRelativeModePos.mX << ", " << mPreRelativeModePos.mY << ")" << LL_ENDL;
        
        // Flush events before restoring to prevent accumulation
        SDL_FlushEvent(SDL_MOUSEMOTION);
        
        // Set cursor position without triggering relative mode checks
        LLCoordScreen screen_pos;
        if (convertCoords(mPreRelativeModePos, &screen_pos))
        {
            SDL_WarpMouseInWindow(mWindow, screen_pos.mX, screen_pos.mY);
            mPreRelativeModePos.set(-1, -1);  // Clear saved position
            
            // Flush events after warping to clear any generated events
            SDL_FlushEvent(SDL_MOUSEMOTION);
        }
    }
}

void LLWindowSDL::setRelativeModeState(bool active)
{
    // Validate window exists before performing operations
    if (!mWindow)
    {
        LL_WARNS("Mouse") << "Cannot set relative mode state: window not available" << LL_ENDL;
        return;
    }
    
    if (mRelativeModeActive != active)
    {
        if (active)
        {
            // Entering relative mode - save position and flush events
            saveCursorPositionForRelativeMode();
            
            // Flush any pending mouse motion events to prevent accumulation
            SDL_FlushEvent(SDL_MOUSEMOTION);
            
            // Clear accumulated deltas
            mRelativeDeltaX = 0;
            mRelativeDeltaY = 0;
        }
        else
        {
            // Exiting relative mode - flush events first
            SDL_FlushEvent(SDL_MOUSEMOTION);
            
            // Clear deltas before restoring position
            mRelativeDeltaX = 0;
            mRelativeDeltaY = 0;
            
            // Now restore cursor position
            restoreCursorPositionFromRelativeMode();
            
            // Flush events again after position restoration to clear any generated by warp
            SDL_FlushEvent(SDL_MOUSEMOTION);
        }
        
        mRelativeModeActive = active;
        LL_DEBUGS("Mouse") << "Relative mode state changed to: " << (active ? "active" : "inactive") << LL_ENDL;
    }
}

F32 LLWindowSDL::getNativeAspectRatio()
{
    // MBW -- there are a couple of bad assumptions here.  One is that the display list won't include
    //      ridiculous resolutions nobody would ever use.  The other is that the list is in order.

    // New assumptions:
    // - pixels are square (the only reasonable choice, really)
    // - The user runs their display at a native resolution, so the resolution of the display
    //    when the app is launched has an aspect ratio that matches the monitor.

    //RN: actually, the assumption that there are no ridiculous resolutions (above the display's native capabilities) has
    // been born out in my experience.
    // Pixels are often not square (just ask the people who run their LCDs at 1024x768 or 800x600 when running fullscreen, like me)
    // The ordering of display list is a blind assumption though, so we should check for max values
    // Things might be different on the Mac though, so I'll defer to MBW

    // The constructor for this class grabs the aspect ratio of the monitor before doing any resolution
    // switching, and stashes it in mOriginalAspectRatio.  Here, we just return it.

    if (mOverrideAspectRatio > 0.f)
    {
        return mOverrideAspectRatio;
    }

    return mOriginalAspectRatio;
}

F32 LLWindowSDL::getPixelAspectRatio()
{
    F32 pixel_aspect = 1.f;
    
    // Initialize DPI scaling detection if not done yet
    if (mRunningUnderXWayland && !mDPIScalingInitialized)
    {
        initializeXWaylandDPIScaling();
    }
    
    if (getFullscreen())
    {
        LLCoordScreen screen_size;
        if (getSize(&screen_size))
        {
            pixel_aspect = getNativeAspectRatio() * (F32)screen_size.mY / (F32)screen_size.mX;
        }
    }

    // Apply XWayland DPI scaling if needed
    if (mRunningUnderXWayland && mXWaylandDPIScale != 1.0f)
    {
        pixel_aspect *= mXWaylandDPIScale;
        LL_DEBUGS("XWayland") << "Applied DPI scaling: " << mXWaylandDPIScale << LL_ENDL;
    }

    return pixel_aspect;
}


// This is to support 'temporarily windowed' mode so that
// dialogs are still usable in fullscreen.
void LLWindowSDL::beforeDialog()
{
    bool running_x11 = false;
#if LL_X11
    running_x11 = (mSDL_XWindowID != None);
#endif //LL_X11

    LL_INFOS() << "LLWindowSDL::beforeDialog()" << LL_ENDL;

    if (SDLReallyCaptureInput(false)) // must ungrab input so popup works!
    {
        if (mFullscreen)
        {
            // need to temporarily go non-fullscreen; bless SDL
            // for providing a SDL_WM_ToggleFullScreen() - though
            // it only works in X11
            if (running_x11 && mWindow)
            {
                SDL_SetWindowFullscreen( mWindow, 0 );
            }
        }
    }

#if LL_X11
    if (mSDL_Display)
    {
        // Everything that we/SDL asked for should happen before we
        // potentially hand control over to GTK.
        maybe_lock_display();
        XSync(mSDL_Display, False);
        maybe_unlock_display();
    }
#endif // LL_X11

    maybe_lock_display();
}

void LLWindowSDL::afterDialog()
{
    bool running_x11 = false;
#if LL_X11
    running_x11 = (mSDL_XWindowID != None);
#endif //LL_X11

    LL_INFOS() << "LLWindowSDL::afterDialog()" << LL_ENDL;

    maybe_unlock_display();

    if (mFullscreen)
    {
        // need to restore fullscreen mode after dialog - only works
        // in X11
        if (running_x11 && mWindow)
        {
            SDL_SetWindowFullscreen( mWindow, 0 );
        }
    }
}


#if LL_X11
// set/reset the XWMHints flag for 'urgency' that usually makes the icon flash
void LLWindowSDL::x11_set_urgent(bool urgent)
{
    if (mSDL_Display && !mFullscreen)
    {
        XWMHints *wm_hints;

        LL_INFOS() << "X11 hint for urgency, " << urgent << LL_ENDL;

        maybe_lock_display();
        wm_hints = XGetWMHints(mSDL_Display, mSDL_XWindowID);
        if (!wm_hints)
            wm_hints = XAllocWMHints();

        if (urgent)
            wm_hints->flags |= XUrgencyHint;
        else
            wm_hints->flags &= ~XUrgencyHint;

        XSetWMHints(mSDL_Display, mSDL_XWindowID, wm_hints);
        XFree(wm_hints);
        XSync(mSDL_Display, False);
        maybe_unlock_display();
    }
}
#endif // LL_X11

void LLWindowSDL::flashIcon(F32 seconds)
{
    if (getMinimized()) // <FS:CR> Moved this here from llviewermessage.cpp
    {
#if !LL_X11
        LL_INFOS() << "Stub LLWindowSDL::flashIcon(" << seconds << ")" << LL_ENDL;
#else
        LL_INFOS() << "X11 LLWindowSDL::flashIcon(" << seconds << ")" << LL_ENDL;

        F32 remaining_time = mFlashTimer.getRemainingTimeF32();
        if (remaining_time < seconds)
            remaining_time = seconds;
        mFlashTimer.reset();
        mFlashTimer.setTimerExpirySec(remaining_time);

        x11_set_urgent(true);
        mFlashing = true;
#endif // LL_X11
    }
}

bool LLWindowSDL::isClipboardTextAvailable()
{
    if (!mSDL_Display)
        return false;
        
    // XWayland may have delayed clipboard synchronization
    if (mRunningUnderXWayland)
    {
        // Try multiple times with small delay for XWayland sync
        for (int attempts = 0; attempts < 3; attempts++)
        {
            if (XGetSelectionOwner(mSDL_Display, XA_CLIPBOARD) != None)
                return true;
                
            if (attempts < 2)
            {
                usleep(10000);  // 10ms delay for clipboard sync
                XSync(mSDL_Display, False);  // Force sync with XWayland
            }
        }
        return false;
    }
    
    return XGetSelectionOwner(mSDL_Display, XA_CLIPBOARD) != None;
}

bool LLWindowSDL::pasteTextFromClipboard(LLWString &dst)
{
    // XWayland clipboard synchronization with retry logic
    if (mRunningUnderXWayland)
    {
        // Try multiple times as XWayland clipboard sync can be delayed
        for (int attempts = 0; attempts < 3; attempts++)
        {
            if (getSelectionText(XA_CLIPBOARD, dst))
            {
                LL_DEBUGS("XWayland") << "Clipboard paste succeeded on attempt " << (attempts + 1) << LL_ENDL;
                return true;
            }
            
            if (attempts < 2)
            {
                LL_DEBUGS("XWayland") << "Clipboard paste failed, retrying..." << LL_ENDL;
                usleep(20000);  // 20ms delay for sync
                XSync(mSDL_Display, False);
            }
        }
        
        LL_WARNS("XWayland") << "Clipboard paste failed after retries" << LL_ENDL;
        return false;
    }
    
    return getSelectionText(XA_CLIPBOARD, dst);
}

bool LLWindowSDL::copyTextToClipboard(const LLWString &s)
{
    bool result = setSelectionText(XA_CLIPBOARD, s);
    
    // XWayland clipboard synchronization
    if (mRunningUnderXWayland && result)
    {
        // Force immediate synchronization with Wayland compositor
        XSync(mSDL_Display, False);
        usleep(5000);  // 5ms delay to ensure sync
        
        LL_DEBUGS("XWayland") << "Clipboard copy with XWayland sync" << LL_ENDL;
    }
    
    return result;
}

bool LLWindowSDL::isPrimaryTextAvailable()
{
    LLWString text;
    return getSelectionText(XA_PRIMARY, text) && !text.empty();
}

bool LLWindowSDL::pasteTextFromPrimary(LLWString &dst)
{
    return getSelectionText(XA_PRIMARY, dst);
}

bool LLWindowSDL::copyTextToPrimary(const LLWString &s)
{
    return setSelectionText(XA_PRIMARY, s);
}

LLWindow::LLWindowResolution* LLWindowSDL::getSupportedResolutions(S32 &num_resolutions)
{
    if (!mSupportedResolutions)
    {
        mSupportedResolutions = new LLWindowResolution[MAX_NUM_RESOLUTIONS];
        mNumSupportedResolutions = 0;

        // Get the current monitor index instead of hardcoding display 0
        S32 current_display = mWindow ? SDL_GetWindowDisplayIndex(mWindow) : 0;
        if (current_display < 0)
            current_display = 0;  // Fallback to primary display

        // For XWayland, also check all displays for compatibility
        S32 num_displays = SDL_GetNumVideoDisplays();
        if (mRunningUnderXWayland && num_displays > 1)
        {
            LL_DEBUGS("XWayland") << "Multi-monitor detection: " << num_displays << " displays found" << LL_ENDL;
            
            // In XWayland, enumerate resolutions from all monitors for better compatibility
            for (S32 display_idx = 0; display_idx < num_displays && mNumSupportedResolutions < MAX_NUM_RESOLUTIONS; ++display_idx)
            {
                int max_modes = SDL_GetNumDisplayModes(display_idx);
                max_modes = llclamp(max_modes, 0, MAX_NUM_RESOLUTIONS - mNumSupportedResolutions);
                
                for (int mode_idx = 0; mode_idx < max_modes; ++mode_idx)
                {
                    SDL_DisplayMode mode = { SDL_PIXELFORMAT_UNKNOWN, 0, 0, 0, 0 };
                    if (SDL_GetDisplayMode(display_idx, mode_idx, &mode) != 0)
                        continue;

                    int w = mode.w;
                    int h = mode.h;
                    
                    // Apply DPI scaling for XWayland if detected
                    if (mDPIScalingInitialized && mXWaylandDPIScale > 1.0f)
                    {
                        w = (int)(w / mXWaylandDPIScale);
                        h = (int)(h / mXWaylandDPIScale);
                    }
                    
                    if ((w >= 800) && (h >= 600))
                    {
                        // Check for duplicates across all displays
                        bool is_duplicate = false;
                        for (S32 existing = 0; existing < mNumSupportedResolutions; ++existing)
                        {
                            if (mSupportedResolutions[existing].mWidth == w &&
                                mSupportedResolutions[existing].mHeight == h)
                            {
                                is_duplicate = true;
                                break;
                            }
                        }
                        
                        if (!is_duplicate)
                        {
                            mSupportedResolutions[mNumSupportedResolutions].mWidth = w;
                            mSupportedResolutions[mNumSupportedResolutions].mHeight = h;
                            mNumSupportedResolutions++;
                        }
                    }
                }
            }
        }
        else
        {
            // Standard single-display resolution enumeration
            int max = SDL_GetNumDisplayModes(current_display);
            max = llclamp(max, 0, MAX_NUM_RESOLUTIONS);

            for (int i = 0; i < max; ++i)
            {
                SDL_DisplayMode mode = { SDL_PIXELFORMAT_UNKNOWN, 0, 0, 0, 0 };
                if (SDL_GetDisplayMode(current_display, i, &mode) != 0)
                    continue;

                int w = mode.w;
                int h = mode.h;
                if ((w >= 800) && (h >= 600))
                {
                    // make sure we don't add the same resolution multiple times!
                    if ((mNumSupportedResolutions == 0) ||
                        ((mSupportedResolutions[mNumSupportedResolutions-1].mWidth != w) &&
                         (mSupportedResolutions[mNumSupportedResolutions-1].mHeight != h)))
                    {
                        mSupportedResolutions[mNumSupportedResolutions].mWidth = w;
                        mSupportedResolutions[mNumSupportedResolutions].mHeight = h;
                        mNumSupportedResolutions++;
                    }
                }
            }
        }
        
        LL_DEBUGS("Window") << "Found " << mNumSupportedResolutions << " supported resolutions" << LL_ENDL;
    }

    num_resolutions = mNumSupportedResolutions;
    return mSupportedResolutions;
}

bool LLWindowSDL::convertCoords(LLCoordGL from, LLCoordWindow *to)
{
    if (!to)
        return false;

    // Convert from OpenGL coordinates to window coordinates
    // GL uses drawable size, Window uses logical size
    to->mX = (S32)(from.mX / mDPIScaleX);
    to->mY = (S32)((mDrawableHeight - from.mY - 1) / mDPIScaleY);

    return true;
}

bool LLWindowSDL::convertCoords(LLCoordWindow from, LLCoordGL* to)
{
    if (!to)
        return false;

    // Convert from window coordinates to OpenGL coordinates
    // Window uses logical size, GL uses drawable size
    to->mX = (S32)(from.mX * mDPIScaleX);
    to->mY = (S32)(mDrawableHeight - (from.mY * mDPIScaleY) - 1);

    return true;
}

bool LLWindowSDL::convertCoords(LLCoordScreen from, LLCoordWindow* to)
{
    if (!to)
        return false;

    // Convert from screen coordinates to window coordinates
    // Screen coordinates are typically in physical pixels, window in logical pixels
    if (mFullscreen) {
        // In fullscreen, coordinates are the same
        to->mX = from.mX;
        to->mY = from.mY;
    } else {
        // Apply DPI scaling conversion
        to->mX = (S32)(from.mX / mDPIScaleX);
        to->mY = (S32)(from.mY / mDPIScaleY);
    }
    return true;
}

bool LLWindowSDL::convertCoords(LLCoordWindow from, LLCoordScreen *to)
{
    if (!to)
        return false;

    // Convert from window coordinates to screen coordinates
    // Window coordinates are in logical pixels, screen in physical pixels
    if (mFullscreen) {
        // In fullscreen, coordinates are the same
        to->mX = from.mX;
        to->mY = from.mY;
    } else {
        // Apply DPI scaling conversion
        to->mX = (S32)(from.mX * mDPIScaleX);
        to->mY = (S32)(from.mY * mDPIScaleY);
    }
    return true;
}

bool LLWindowSDL::convertCoords(LLCoordScreen from, LLCoordGL *to)
{
    LLCoordWindow window_coord;

    return(convertCoords(from, &window_coord) && convertCoords(window_coord, to));
}

bool LLWindowSDL::convertCoords(LLCoordGL from, LLCoordScreen *to)
{
    LLCoordWindow window_coord;

    return(convertCoords(from, &window_coord) && convertCoords(window_coord, to));
}


void LLWindowSDL::updateDPIScaling()
{
    if (!mWindow)
    {
        mDPIScaleX = 1.0f;
        mDPIScaleY = 1.0f;
        mDrawableWidth = 0;
        mDrawableHeight = 0;
        return;
    }
    
    int window_w, window_h;
    int drawable_w, drawable_h;
    
    // Get logical window size
    SDL_GetWindowSize(mWindow, &window_w, &window_h);
    
    // Get actual drawable size (which accounts for DPI scaling)
    SDL_GL_GetDrawableSize(mWindow, &drawable_w, &drawable_h);
    
    // Calculate DPI scale factors
    mDPIScaleX = (window_w > 0) ? (float)drawable_w / window_w : 1.0f;
    mDPIScaleY = (window_h > 0) ? (float)drawable_h / window_h : 1.0f;
    
    mDrawableWidth = drawable_w;
    mDrawableHeight = drawable_h;
    
    LL_INFOS() << "DPI Scaling - Window: " << window_w << "x" << window_h 
               << ", Drawable: " << drawable_w << "x" << drawable_h
               << ", Scale: " << mDPIScaleX << "x" << mDPIScaleY << LL_ENDL;
    
    // Log XWayland fractional scaling status if active
    if (mXWaylandFractionalScaling)
    {
        LL_INFOS("XWaylandScaling") << "XWayland fractional scaling active - Wayland scale: " << mWaylandScaleFactor
                                   << ", Coordinate compensation: " << mCoordinateCompensationX << "x" << mCoordinateCompensationY << LL_ENDL;
    }
}

void LLWindowSDL::detectXWaylandScaling()
{
    // Reset XWayland fractional scaling detection
    mXWaylandFractionalScaling = false;
    mWaylandScaleFactor = 1.0f;
    mCoordinateCompensationX = 1.0f;
    mCoordinateCompensationY = 1.0f;
    mXWaylandCompensationEnabled = true; // Default to enabled, can be toggled for testing
    
    // Auto-test mode for coordinate debugging - checks environment variable
    const char* test_mode_env = std::getenv("FIRESTORM_COORDINATE_TEST_MODE");
    if (test_mode_env)
    {
        int test_mode = atoi(test_mode_env);
        LL_INFOS("XWaylandTesting") << "Environment variable FIRESTORM_COORDINATE_TEST_MODE=" << test_mode << " detected" << LL_ENDL;
        
        // We'll apply the test mode after detection is complete
    }

    // Check if we're running under XWayland
    const char* session_type = std::getenv("XDG_SESSION_TYPE");
    const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
    
    if (!session_type || strcmp(session_type, "wayland") != 0 || !wayland_display)
    {
        LL_INFOS("XWaylandScaling") << "Not running under XWayland - no compensation needed" << LL_ENDL;
        return;
    }
    
    LL_INFOS("XWaylandScaling") << "XWayland session detected, checking for fractional scaling" << LL_ENDL;
    
    // Try to detect GNOME Mutter display config using gdbus
    // This command gets the current display configuration including scale factor
    FILE* fp = popen("gdbus call --session --dest org.gnome.Mutter.DisplayConfig --object-path /org/gnome/Mutter/DisplayConfig --method org.gnome.Mutter.DisplayConfig.GetCurrentState 2>/dev/null", "r");
    
    if (fp)
    {
        char buffer[4096];
        std::string output;
        
        while (fgets(buffer, sizeof(buffer), fp))
        {
            output += buffer;
        }
        pclose(fp);
        
        // Parse the output to extract the current scale factor
        // Look for patterns like "1.7518248558044434" in the output
        std::size_t scale_pos = output.find("'is-current': <true>");
        if (scale_pos != std::string::npos)
        {
            // Search backwards for the scale factor before 'is-current': <true>
            std::size_t search_start = std::max(0, (int)scale_pos - 200);
            std::string search_area = output.substr(search_start, scale_pos - search_start);
            
            // Look for floating point numbers that could be scale factors
            // Typical fractional scale factors: 1.25, 1.5, 1.75, 2.0, etc.
            size_t pos = search_area.rfind(", ");
            if (pos != std::string::npos)
            {
                pos += 2; // Skip ", "
                size_t end = search_area.find(",", pos);
                if (end == std::string::npos) end = search_area.find(" ", pos);
                if (end != std::string::npos)
                {
                    std::string scale_str = search_area.substr(pos, end - pos);
                    try
                    {
                        float scale = std::stof(scale_str);
                        if (scale >= 1.1f && scale <= 4.0f) // Reasonable scale factor range
                        {
                            mWaylandScaleFactor = scale;
                            
                            // If scale factor is not 1.0, we need fractional scaling compensation
                            if (abs(scale - 1.0f) > 0.01f)
                            {
                                mXWaylandFractionalScaling = true;
                                
                                // XWayland coordinate compensation
                                // Physical coordinates come in at native resolution but need adjustment
                                mCoordinateCompensationX = 1.0f / scale;
                                mCoordinateCompensationY = 1.0f / scale;
                                
                                LL_INFOS("XWaylandScaling") << "XWayland fractional scaling detected: " << scale 
                                                           << ", compensation: " << mCoordinateCompensationX << LL_ENDL;
                            }
                        }
                    }
                    catch (const std::exception& e)
                    {
                        LL_WARNS("XWaylandScaling") << "Failed to parse scale factor: " << scale_str << LL_ENDL;
                    }
                }
            }
        }
    }
    
    // Fallback: Try to detect scaling through xrandr if gdbus failed
    if (!mXWaylandFractionalScaling)
    {
        fp = popen("xrandr --listmonitors 2>/dev/null", "r");
        if (fp)
        {
            char buffer[256];
            if (fgets(buffer, sizeof(buffer), fp)) // Skip header
            {
                if (fgets(buffer, sizeof(buffer), fp)) // Read monitor line
                {
                    // Parse format like " 0: +*DP-3 4384/600x2466/340+0+0  DP-3"
                    int width_px, width_mm, height_px, height_mm;
                    if (sscanf(buffer, "%*d: %*s %d/%dx%d/%d", &width_px, &width_mm, &height_px, &height_mm) == 4)
                    {
                        // Calculate DPI and infer scaling
                        float dpi_x = (float)width_px * 25.4f / width_mm;
                        float dpi_y = (float)height_px * 25.4f / height_mm;
                        float avg_dpi = (dpi_x + dpi_y) / 2.0f;
                        
                        // Standard DPI is 96, so scaling factor approximation
                        float inferred_scale = avg_dpi / 96.0f;
                        
                        // Check if this looks like fractional scaling (1.25, 1.5, 1.75, 2.0, etc.)
                        if (inferred_scale >= 1.1f && inferred_scale <= 4.0f)
                        {
                            mWaylandScaleFactor = inferred_scale;
                            mXWaylandFractionalScaling = true;
                            mCoordinateCompensationX = 1.0f / inferred_scale;
                            mCoordinateCompensationY = 1.0f / inferred_scale;
                            
                            LL_INFOS("XWaylandScaling") << "XWayland scaling inferred from DPI: " << inferred_scale 
                                                       << " (DPI: " << avg_dpi << ", compensation: " << mCoordinateCompensationX << ")" << LL_ENDL;
                        }
                    }
                }
            }
            pclose(fp);
        }
    }
    
    if (!mXWaylandFractionalScaling)
    {
        LL_INFOS("XWaylandScaling") << "No XWayland fractional scaling detected - using standard coordinate handling" << LL_ENDL;
    }
    
    // Apply test mode if specified via environment variable
    const char* test_mode_env = std::getenv("FIRESTORM_COORDINATE_TEST_MODE");
    if (test_mode_env && mXWaylandFractionalScaling)
    {
        int test_mode = atoi(test_mode_env);
        LL_INFOS("XWaylandTesting") << "Applying coordinate test mode " << test_mode << " from environment variable" << LL_ENDL;
        testCoordinateCompensation(test_mode);
    }
}

void LLWindowSDL::logCoordinateDebugInfo(const char* event_name, float original_x, float original_y, float compensated_x, float compensated_y)
{
    if (mXWaylandFractionalScaling)
    {
        LL_DEBUGS("XWaylandScaling") << event_name << " coordinate compensation: (" 
                                    << original_x << "," << original_y 
                                    << ") -> (" << compensated_x << "," << compensated_y << ")"
                                    << " [Scale factor: " << mWaylandScaleFactor 
                                    << ", Compensation: " << mCoordinateCompensationX << "x" << mCoordinateCompensationY << "]" << LL_ENDL;
    }
}

void LLWindowSDL::logDetailedCoordinateFlow(const char* stage, float x, float y, const char* description)
{
    LL_INFOS("CoordinateFlow") << "STAGE: " << stage 
                              << " | COORDS: (" << x << "," << y << ")"
                              << " | DESC: " << description
                              << " | DPI Scale: (" << mDPIScaleX << "," << mDPIScaleY << ")"
                              << " | XWayland: " << (mXWaylandFractionalScaling ? "ON" : "OFF")
                              << " | Compensation: " << (mXWaylandCompensationEnabled ? "ENABLED" : "DISABLED")
                              << LL_ENDL;
}

void LLWindowSDL::setXWaylandCompensationEnabled(bool enabled)
{
    mXWaylandCompensationEnabled = enabled;
    LL_INFOS("XWaylandTesting") << "XWayland compensation " << (enabled ? "ENABLED" : "DISABLED") << " for testing" << LL_ENDL;
}

void LLWindowSDL::testCoordinateCompensation(int mode)
{
    if (!mXWaylandFractionalScaling)
    {
        LL_INFOS("XWaylandTesting") << "Cannot test compensation - XWayland fractional scaling not detected" << LL_ENDL;
        return;
    }
    
    float original_compensation = 1.0f / mWaylandScaleFactor;
    
    switch (mode)
    {
        case 0: // Disabled
            mXWaylandCompensationEnabled = false;
            LL_INFOS("XWaylandTesting") << "Testing mode 0: Compensation DISABLED" << LL_ENDL;
            break;
            
        case 1: // Normal (default)
            mXWaylandCompensationEnabled = true;
            mCoordinateCompensationX = original_compensation;
            mCoordinateCompensationY = original_compensation;
            LL_INFOS("XWaylandTesting") << "Testing mode 1: NORMAL compensation (" << original_compensation << "x)" << LL_ENDL;
            break;
            
        case 2: // Inverted (multiply instead of divide)
            mXWaylandCompensationEnabled = true;
            mCoordinateCompensationX = mWaylandScaleFactor;
            mCoordinateCompensationY = mWaylandScaleFactor;
            LL_INFOS("XWaylandTesting") << "Testing mode 2: INVERTED compensation (" << mWaylandScaleFactor << "x)" << LL_ENDL;
            break;
            
        case 3: // Double compensation (for testing double-scaling theory)
            mXWaylandCompensationEnabled = true;
            mCoordinateCompensationX = original_compensation * original_compensation;
            mCoordinateCompensationY = original_compensation * original_compensation;
            LL_INFOS("XWaylandTesting") << "Testing mode 3: DOUBLE compensation (" << (original_compensation * original_compensation) << "x)" << LL_ENDL;
            break;
            
        default:
            LL_INFOS("XWaylandTesting") << "Invalid test mode " << mode << ". Use 0=disabled, 1=normal, 2=inverted, 3=double" << LL_ENDL;
            return;
    }
    
    LL_INFOS("XWaylandTesting") << "Click on a UI element to see coordinate transformation with mode " << mode << LL_ENDL;
}


void LLWindowSDL::setupFailure(const std::string& text, const std::string& caption, U32 type)
{
    destroyContext();

    OSMessageBox(text, caption, type);
}

bool LLWindowSDL::SDLReallyCaptureInput(bool capture)
{
    // note: this used to be safe to call nestedly, but in the
    // end that's not really a wise usage pattern, so don't.

    if (capture)
        mReallyCapturedCount = 1;
    else
        mReallyCapturedCount = 0;

    bool wantGrab;
    if (mReallyCapturedCount <= 0) // uncapture
    {
        wantGrab = false;
    } else // capture
    {
        wantGrab = true;
    }

    if (mReallyCapturedCount < 0) // yuck, imbalance.
    {
        mReallyCapturedCount = 0;
        LL_WARNS() << "ReallyCapture count was < 0" << LL_ENDL;
    }

    bool newGrab = wantGrab;

#if LL_X11
    if (!mFullscreen) /* only bother if we're windowed anyway */
    {
        if (mSDL_Display)
        {
            {
                /* we dirtily mix raw X11 with SDL so that our pointer
                   isn't (as often) constrained to the limits of the
                   window while grabbed, which feels nicer and
                   hopefully eliminates some reported 'sticky pointer'
                   problems.  We use raw X11 instead of
                   SDL_WM_GrabInput() because the latter constrains
                   the pointer to the window and also steals all
                   *keyboard* input from the window manager, which was
                   frustrating users. */
                int result;
                if (wantGrab == true)
                {
                    // Don't attempt grab if we don't have focus
                    if (!mHaveInputFocus)
                    {
                        LL_DEBUGS("XWayland") << "Skipping grab - window doesn't have focus" << LL_ENDL;
                        newGrab = false;
                    }
                    else
                    {
                        // Standard grabbing for XWayland
                        if (mRunningUnderXWayland)
                        {
                            newGrab = true;
                            LL_INFOS("XWayland") << "Enabled grabbing for input capture" << LL_ENDL;
                        }
                        
                        // Use standard capture approach
                        {
                            // Save cursor position before grabbing for XWayland ungrab restoration
                            // Only save position if we're not already in a grab operation
                            if (mRunningUnderXWayland && !mInGrabOperation)
                            {
                                getCursorPosition(&mSavedCursorPos);
                                mGrabOperationStartTime = LLFrameTimer::getTotalTime();
                                LL_INFOS("XWayland") << "Saving cursor position before grab: " 
                                                     << mSavedCursorPos.mX << ", " << mSavedCursorPos.mY << LL_ENDL;
                            }

                            // Use modern SDL_CaptureMouse instead of raw X11 grabs
                            int result = SDL_CaptureMouse(SDL_TRUE);
                            if (0 == result)
                            {
                                newGrab = true;
                                if (mRunningUnderXWayland)
                                    mInGrabOperation = true;
                            }
                            else
                            {
                                newGrab = false;
                                if (mRunningUnderXWayland)
                                    mInGrabOperation = false;
                                LL_WARNS("Mouse") << "SDL_CaptureMouse failed: " << SDL_GetError() << LL_ENDL;
                            }
                        }
                    }
                }
                else
                {
                    newGrab = false;
                    
                    // Clean up grab state
                    SDL_CaptureMouse(SDL_FALSE);

                    // XWayland grab state cleanup and cursor restoration after ungrab
                    if (mRunningUnderXWayland && mInGrabOperation)
                    {
                        mInGrabOperation = false;
                        mLastUngrabTime = LLFrameTimer::getTotalTime();
                        
                        // Restore cursor to original position for Alt+drag operations
                        // This is standard 3D application behavior
                        if (mSavedCursorPos.mX >= 0 && mSavedCursorPos.mY >= 0)
                        {
                            LL_INFOS("XWayland") << "Restoring cursor to saved position: " 
                                                 << mSavedCursorPos.mX << ", " << mSavedCursorPos.mY << LL_ENDL;
                            
                            // Set skip flag to prevent the warp from triggering motion events
                            mSkipNextWarpEvent = true;
                            
                            // Use X11 directly for XWayland cursor restoration
                            maybe_lock_display();
                            
                            // Get window position for global coordinate calculation
                            int win_x, win_y;
                            SDL_GetWindowPosition(mWindow, &win_x, &win_y);
                            
                            // Convert window coordinates to global screen coordinates
                            int global_x = win_x + mSavedCursorPos.mX;
                            int global_y = win_y + mSavedCursorPos.mY;
                            
                            LL_INFOS("XWayland") << "Warping to global position: " 
                                                 << global_x << ", " << global_y 
                                                 << " (window at " << win_x << ", " << win_y << ")" << LL_ENDL;
                            
                            // Use XWarpPointer for more reliable cursor positioning under XWayland
                            XWarpPointer(mSDL_Display, None, mSDL_XWindowID, 
                                         0, 0, 0, 0, 
                                         mSavedCursorPos.mX, mSavedCursorPos.mY);
                            
                            // Ensure the warp completes
                            XSync(mSDL_Display, False);
                            maybe_unlock_display();
                            
                            // Small delay for XWayland to process
                            usleep(5000); // 5ms
                            
                            // Clear saved position
                            mSavedCursorPos.mX = -1;
                            mSavedCursorPos.mY = -1;
                        }
                        else
                        {
                            LL_WARNS("XWayland") << "Invalid saved cursor position, skipping restoration" << LL_ENDL;
                        }
                    }
                }
            }
        }
    }
#endif // LL_X11
    // return boolean success for whether we ended up in the desired state
    return capture == newGrab;
}

U32 LLWindowSDL::SDLCheckGrabbyKeys(U32 keysym, bool gain)
{
    /* part of the fix for SL-13243: Some popular window managers like
       to totally eat alt-drag for the purposes of moving windows.  We
       spoil their day by acquiring the exclusive X11 mouse lock for as
       long as ALT is held down, so the window manager can't easily
       see what's happening.  Tested successfully with Metacity.
       And... do the same with CTRL, for other darn WMs.  We don't
       care about other metakeys as SL doesn't use them with dragging
       (for now). */

    /* We maintain a bitmap of critical keys which are up and down
       instead of simply key-counting, because SDL sometimes reports
       misbalanced keyup/keydown event pairs to us for whatever reason. */

    U32 mask = 0;
    switch (keysym)
    {
    case SDLK_LALT:
        mask = 1U << 0; break;
    case SDLK_RALT:
        mask = 1U << 1; break;
    case SDLK_LCTRL:
        mask = 1U << 2; break;
    case SDLK_RCTRL:
        mask = 1U << 3; break;
    default:
        break;
    }

    if (gain)
        mGrabbyKeyFlags |= mask;
    else
        mGrabbyKeyFlags &= ~mask;

    //LL_INFOS() << "mGrabbyKeyFlags=" << mGrabbyKeyFlags << LL_ENDL;

    /* 0 means we don't need to mousegrab, otherwise grab. */
    return mGrabbyKeyFlags;
}


void check_vm_bloat()
{
#if LL_LINUX
    // watch our own VM and RSS sizes, warn if we bloated rapidly
    static const std::string STATS_FILE = "/proc/self/stat";
    FILE *fp = fopen(STATS_FILE.c_str(), "r");
    if (fp)
    {
        static long long last_vm_size = 0;
        static long long last_rss_size = 0;
        const long long significant_vm_difference = 250 * 1024*1024;
        const long long significant_rss_difference = 50 * 1024*1024;
        long long this_vm_size = 0;
        long long this_rss_size = 0;

        ssize_t res;
        size_t dummy;
        char *ptr = NULL;
        for (int i=0; i<22; ++i) // parse past the values we don't want
        {
            res = getdelim(&ptr, &dummy, ' ', fp);
            if (-1 == res)
            {
                LL_WARNS() << "Unable to parse " << STATS_FILE << LL_ENDL;
                goto finally;
            }
            free(ptr);
            ptr = NULL;
        }
        // 23rd space-delimited entry is vsize
        res = getdelim(&ptr, &dummy, ' ', fp);
        llassert(ptr);
        if (-1 == res)
        {
            LL_WARNS() << "Unable to parse " << STATS_FILE << LL_ENDL;
            goto finally;
        }
        this_vm_size = atoll(ptr);
        free(ptr);
        ptr = NULL;
        // 24th space-delimited entry is RSS
        res = getdelim(&ptr, &dummy, ' ', fp);
        llassert(ptr);
        if (-1 == res)
        {
            LL_WARNS() << "Unable to parse " << STATS_FILE << LL_ENDL;
            goto finally;
        }
        this_rss_size = getpagesize() * atoll(ptr);
        free(ptr);
        ptr = NULL;

        LL_INFOS() << "VM SIZE IS NOW " << (this_vm_size/(1024*1024)) << " MB, RSS SIZE IS NOW " << (this_rss_size/(1024*1024)) << " MB" << LL_ENDL;

        if (llabs(last_vm_size - this_vm_size) >
            significant_vm_difference)
        {
            if (this_vm_size > last_vm_size)
            {
                LL_WARNS() << "VM size grew by " << (this_vm_size - last_vm_size)/(1024*1024) << " MB in last frame" << LL_ENDL;
            }
            else
            {
                LL_INFOS() << "VM size shrank by " << (last_vm_size - this_vm_size)/(1024*1024) << " MB in last frame" << LL_ENDL;
            }
        }

        if (llabs(last_rss_size - this_rss_size) >
            significant_rss_difference)
        {
            if (this_rss_size > last_rss_size)
            {
                LL_WARNS() << "RSS size grew by " << (this_rss_size - last_rss_size)/(1024*1024) << " MB in last frame" << LL_ENDL;
            }
            else
            {
                LL_INFOS() << "RSS size shrank by " << (last_rss_size - this_rss_size)/(1024*1024) << " MB in last frame" << LL_ENDL;
            }
        }

        last_rss_size = this_rss_size;
        last_vm_size = this_vm_size;

finally:
        if (NULL != ptr)
        {
            free(ptr);
            ptr = NULL;
        }
        fclose(fp);
    }
#endif // LL_LINUX
}


// virtual
void LLWindowSDL::processMiscNativeEvents()
{
#if LL_GLIB
    // XWayland performance optimization - reduce GLib pumping frequency
    F32 pump_time_limit = mRunningUnderXWayland ? 
        (1.0f / 30.0f) :  // 30 FPS for XWayland (less overhead)
        (1.0f / 15.0f);   // Standard 15 FPS for native X11
        
    // Pump until we've nothing left to do or passed time limit pumping for this frame.
    static LLTimer pump_timer;
    pump_timer.reset();
    pump_timer.setTimerExpirySec(pump_time_limit);
    do
    {
        g_main_context_iteration(g_main_context_default(), false);
    } while( g_main_context_pending(g_main_context_default()) && !pump_timer.hasExpired());
    
    // XWayland may need additional X11 event processing for sync
    if (mRunningUnderXWayland && mSDL_Display)
    {
        // Validate grab state periodically
        static LLTimer validation_timer;
        if (validation_timer.getElapsedTimeF32() > 1.0f) // Every second
        {
#if LL_X11
            validateGrabState();
#endif
            validation_timer.reset();
        }
        
        // Process any pending X11 events that might be buffered
        XSync(mSDL_Display, False);
        
        // Process any SDL events that might be queued
        SDL_PumpEvents();
    }
#endif

    // hack - doesn't belong here - but this is just for debugging
    if (getenv("LL_DEBUG_BLOAT"))
    {
        check_vm_bloat();
    }
}

void LLWindowSDL::gatherInput()
{
    const Uint32 CLICK_THRESHOLD = 300;  // milliseconds
    static int leftClick = 0;
    static int rightClick = 0;
    static Uint32 lastLeftDown = 0;
    static Uint32 lastRightDown = 0;
    static U64 previousTextinputTime = 0;
    SDL_Event event;

    // mask to apply to the keyup/keydown modifiers to handle AltGr keys correctly
    static U32 altGrMask = 0x00;

    // Handle all outstanding SDL events
    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
            case SDL_SYSWMEVENT:
            {
                XEvent e = event.syswm.msg->msg.x11.event;
                if (e.type == KeyPress || e.type == KeyRelease)
                {
                    // XLookupKeysym doesn't work here because of the weird way the "index" is
                    // tied to the e->state and we don't get the necessary information at this
                    // point, so we use the more expensive XLookupString which apparently knows
                    // all of the secrets inside XKeyEvent. -Zi

                    KeySym ks;
                    static char str[256+1];
                    XLookupString((XKeyEvent *) &e, str, 256, &ks, nullptr);

                    if (ks == XK_ISO_Level3_Shift)
                    {
                        altGrMask = KMOD_RALT;
                    }
                    else if (ks == XK_Alt_R)
                    {
                        altGrMask = 0x00;
                    }
                }
                break;
            }

            case SDL_MOUSEWHEEL:
                if( event.wheel.y != 0 )
                {
                    S32 scroll_delta = -event.wheel.y;
                    
                    // XWayland debug logging if needed
                    if (mRunningUnderXWayland)
                    {
                        LL_DEBUGS("XWayland") << "Mouse wheel: " << event.wheel.y 
                                             << " -> " << scroll_delta << LL_ENDL;
                    }
                    
                    mCallbacks->handleScrollWheel(this, scroll_delta);
                }
                // Also handle horizontal scrolling if supported
                if (event.wheel.x != 0)
                {
                    LL_DEBUGS("XWayland") << "Horizontal wheel scroll: " << event.wheel.x << LL_ENDL;
                    // Could be mapped to horizontal camera movement if desired
                }
                break;

            case SDL_MOUSEMOTION:
            {
                LLCoordWindow winCoord;
                
                // Handle relative mouse mode for camera controls
                if (SDL_GetRelativeMouseMode())
                {
                    // Accumulate relative motion across multiple events per frame
                    // This prevents jittery movement by preserving all mouse movements
                    mRelativeDeltaX += event.motion.xrel;
                    mRelativeDeltaY += -event.motion.yrel;  // Invert Y axis for correct mouselook
                    
                    // Get window center for coordinates
                    int w, h;
                    SDL_GetWindowSize(mWindow, &w, &h);
                    winCoord.mX = w / 2;
                    winCoord.mY = h / 2;
                    
                    LL_DEBUGS("Mouse") << "Relative mode: xrel=" << event.motion.xrel 
                                      << " yrel=" << event.motion.yrel 
                                      << " delta=(" << mRelativeDeltaX << "," << mRelativeDeltaY << ")" << LL_ENDL;
                }
                else
                {
                    // Standard absolute coordinate handling
                    // SDL events provide physical pixel coordinates - convert to logical coordinates
                    // Apply XWayland fractional scaling compensation if needed
                    float compensated_x = event.motion.x;
                    float compensated_y = event.motion.y;
                    
                    if (mXWaylandFractionalScaling)
                    {
                        compensated_x *= mCoordinateCompensationX;
                        compensated_y *= mCoordinateCompensationY;
                        
                        logCoordinateDebugInfo("MOUSEMOTION", event.motion.x, event.motion.y, compensated_x, compensated_y);
                    }
                    
                    winCoord.mX = (S32)(compensated_x / mDPIScaleX);
                    winCoord.mY = (S32)(compensated_y / mDPIScaleY);
                }
                
                // Validate coordinates for multi-monitor setups (skip when in relative mode)
                if (mRunningUnderXWayland && !SDL_GetRelativeMouseMode())
                {
                    // Get display bounds
                    SDL_Rect bounds;
                    int display_index = SDL_GetWindowDisplayIndex(mWindow);
                    if (display_index >= 0)
                    {
                        SDL_GetDisplayBounds(display_index, &bounds);
                        
                        // Check if coordinates are within reasonable bounds (with some tolerance)
                        if (winCoord.mX < bounds.x - 100 || winCoord.mX > bounds.x + bounds.w + 100 ||
                            winCoord.mY < bounds.y - 100 || winCoord.mY > bounds.y + bounds.h + 100)
                        {
                            LL_WARNS("XWayland") << "Suspicious coordinate jump detected: " 
                                                 << winCoord.mX << "," << winCoord.mY
                                                 << " (bounds: " << bounds.x << "," << bounds.y 
                                                 << " " << bounds.w << "x" << bounds.h << ")" << LL_ENDL;
                            // Use last known good position if available
                            if (mLastValidMousePos.mX != 0 || mLastValidMousePos.mY != 0)
                            {
                                winCoord = mLastValidMousePos;
                            }
                        }
                        else
                        {
                            mLastValidMousePos = winCoord;
                        }
                    }
                    else
                    {
                        // Fallback: just update last valid position if display index failed
                        mLastValidMousePos = winCoord;
                    }
                }
                
                LLCoordGL openGlCoord;
                convertCoords(winCoord, &openGlCoord);
                MASK mask = gKeyboard->currentMask(true);
                mCallbacks->handleMouseMove(this, openGlCoord, mask);
                break;
            }

            case SDL_TEXTINPUT:
            {
                auto string = utf8str_to_utf16str( event.text.text );
                mKeyModifiers = gKeyboard->currentMask( false );
                if (altGrMask)
                {
                    mKeyModifiers &= ~MASK_ALT;
                }
                mInputType = "textinput";
                for( auto key: string )
                {
                    mKeyVirtualKey = key;

                    // filter ctrl and left-alt keypresses from line inputs so we don't end up with e.g.
                    // "h" in teleport history filter input after pressing  alt+h to call up the floater
                    if (mKeyModifiers & (MASK_CONTROL | MASK_ALT))
                        gKeyboard->handleKeyDown(mKeyVirtualKey, mKeyModifiers);
                    else
                        handleUnicodeUTF16(key, mKeyModifiers);
                }
                previousTextinputTime = LLFrameTimer::getTotalTime();
                break;
            }

            case SDL_KEYDOWN:
                mKeyVirtualKey = event.key.keysym.sym;
                mKeyModifiers = event.key.keysym.mod & (~altGrMask);
                mInputType = "keydown";

                // treat all possible Enter/Return keys the same
                if (mKeyVirtualKey == SDLK_RETURN2 || mKeyVirtualKey == SDLK_KP_ENTER)
                {
                    mKeyVirtualKey = SDLK_RETURN;
                }

                // XWayland keyboard handling improvements
                if (mRunningUnderXWayland)
                {
                    // Handle compose key sequences that might be different in XWayland
                    // Note: SDLK_COMPOSE may not be available in all SDL2 versions
                    if ((mKeyModifiers & KMOD_RALT)) // Right Alt as compose key
                    {
                        LL_DEBUGS("XWayland") << "Compose key sequence detected" << LL_ENDL;
                    }
                    
                    // Debug international keyboard layouts
                    if (event.key.keysym.scancode != event.key.keysym.sym)
                    {
                        LL_DEBUGS("XWayland") << "Key mapping: scancode=" << event.key.keysym.scancode 
                                             << " sym=" << event.key.keysym.sym << LL_ENDL;
                    }
                }

                if (mKeyVirtualKey == SDLK_RETURN && mIMEEnabled)
                {
                    // block spurious enter key events that break up IME entered lines in teh wrong places
                    U64 eventTimeDiff = LLFrameTimer::getTotalTime() - previousTextinputTime;
                    previousTextinputTime = 0;

                    if (eventTimeDiff < 20000)
                    {
                        LL_INFOS() << "SDL_KEYDOWN(SDLK_RETURN) event came too fast after SDL_TEXTINPUT, blocked - Time: " << eventTimeDiff << LL_ENDL;
                        break;
                    }
                }

                gKeyboard->handleKeyDown(mKeyVirtualKey, mKeyModifiers );

                // <FS:ND> Slightly hacky :| To make the viewer honor enter (eg to accept form input) we've to not only send handleKeyDown but also send a
                // invoke handleUnicodeUTF16 in case the user hits return.
                // Note that we cannot blindly use handleUnicodeUTF16 for each SDL_KEYDOWN. Doing so will create bogus keyboard input (like % for cursor left).
                if( mKeyVirtualKey == SDLK_RETURN )
                {
                    // fix return key not working when capslock, scrolllock or numlock are enabled
                    mKeyModifiers &= (~(KMOD_NUM | KMOD_CAPS | KMOD_MODE | KMOD_SCROLL));
                    handleUnicodeUTF16( mKeyVirtualKey, mKeyModifiers );
                }

                // part of the fix for SL-13243
                if (SDLCheckGrabbyKeys(event.key.keysym.sym, true) != 0)
                    SDLReallyCaptureInput(true);

                break;

            case SDL_KEYUP:
                mKeyVirtualKey = event.key.keysym.sym;
                mKeyModifiers = event.key.keysym.mod & (~altGrMask);
                mInputType = "keyup";

                // treat all possible Enter/Return keys the same
                if (mKeyVirtualKey == SDLK_RETURN2 || mKeyVirtualKey == SDLK_KP_ENTER)
                {
                    mKeyVirtualKey = SDLK_RETURN;
                }

                if (SDLCheckGrabbyKeys(mKeyVirtualKey, false) == 0)
                    SDLReallyCaptureInput(false); // part of the fix for SL-13243

                gKeyboard->handleKeyUp(mKeyVirtualKey,mKeyModifiers);
                break;

            case SDL_MOUSEBUTTONDOWN:
            {
                bool isDoubleClick = false;
                
                // Stage 1: Raw SDL coordinates
                float raw_x = event.button.x;
                float raw_y = event.button.y;
                logDetailedCoordinateFlow("1-RAW_SDL", raw_x, raw_y, "Raw SDL event coordinates");
                
                // Stage 2: XWayland fractional scaling compensation (if enabled and detected)
                float compensated_x = raw_x;
                float compensated_y = raw_y;
                
                if (mXWaylandFractionalScaling && mXWaylandCompensationEnabled)
                {
                    compensated_x *= mCoordinateCompensationX;
                    compensated_y *= mCoordinateCompensationY;
                    
                    logCoordinateDebugInfo("MOUSEBUTTONDOWN", raw_x, raw_y, compensated_x, compensated_y);
                    logDetailedCoordinateFlow("2-XWAYLAND_COMPENSATED", compensated_x, compensated_y, "After XWayland compensation");
                }
                else
                {
                    logDetailedCoordinateFlow("2-XWAYLAND_SKIPPED", compensated_x, compensated_y, 
                        mXWaylandFractionalScaling ? "Compensation disabled for testing" : "No XWayland scaling detected");
                }
                
                // Stage 3: DPI scaling conversion
                float dpi_adjusted_x = compensated_x / mDPIScaleX;
                float dpi_adjusted_y = compensated_y / mDPIScaleY;
                logDetailedCoordinateFlow("3-DPI_ADJUSTED", dpi_adjusted_x, dpi_adjusted_y, "After DPI scaling division");
                
                LLCoordWindow winCoord((S32)dpi_adjusted_x, (S32)dpi_adjusted_y);
                logDetailedCoordinateFlow("4-WINDOW_COORD", winCoord.mX, winCoord.mY, "LLCoordWindow (integer)");
                
                LLCoordGL openGlCoord;
                convertCoords(winCoord, &openGlCoord);
                logDetailedCoordinateFlow("5-OPENGL_COORD", openGlCoord.mX, openGlCoord.mY, "Final LLCoordGL sent to viewer");
                
                MASK mask = gKeyboard->currentMask(true);

                if (event.button.button == SDL_BUTTON_LEFT)   // SDL doesn't manage double clicking...
                {
                    Uint32 now = SDL_GetTicks();
                    if ((now - lastLeftDown) > CLICK_THRESHOLD)
                        leftClick = 1;
                    else
                    {
                        if (++leftClick >= 2)
                        {
                            leftClick = 0;
                            isDoubleClick = true;
                        }
                    }
                    lastLeftDown = now;
                }
                else if (event.button.button == SDL_BUTTON_RIGHT)
                {
                    Uint32 now = SDL_GetTicks();
                    if ((now - lastRightDown) > CLICK_THRESHOLD)
                        rightClick = 1;
                    else
                    {
                        if (++rightClick >= 2)
                        {
                            rightClick = 0;
                            isDoubleClick = true;
                        }
                    }
                    lastRightDown = now;
                }

                if (event.button.button == SDL_BUTTON_LEFT)  // left
                {
                    if (isDoubleClick)
                        mCallbacks->handleDoubleClick(this, openGlCoord, mask);
                    else
                        mCallbacks->handleMouseDown(this, openGlCoord, mask);
                }

                else if (event.button.button == SDL_BUTTON_RIGHT)  // right
                {
                    mCallbacks->handleRightMouseDown(this, openGlCoord, mask);
                }

                else if (event.button.button == SDL_BUTTON_MIDDLE)  // middle
                {
                    mCallbacks->handleMiddleMouseDown(this, openGlCoord, mask);
                }
                else if (event.button.button == 4)  // mousewheel up...thanks to X11 for making SDL consider these "buttons".
                    mCallbacks->handleScrollWheel(this, -1);
                else if (event.button.button == 5)  // mousewheel down...thanks to X11 for making SDL consider these "buttons".
                    mCallbacks->handleScrollWheel(this, 1);

                break;
            }

            case SDL_MOUSEBUTTONUP:
            {
                // SDL events provide physical pixel coordinates - convert to logical window coordinates
                // Apply XWayland fractional scaling compensation if needed
                float compensated_x = event.button.x;
                float compensated_y = event.button.y;
                
                if (mXWaylandFractionalScaling)
                {
                    compensated_x *= mCoordinateCompensationX;
                    compensated_y *= mCoordinateCompensationY;
                    
                    logCoordinateDebugInfo("MOUSEBUTTONUP", event.button.x, event.button.y, compensated_x, compensated_y);
                }
                
                LLCoordWindow winCoord(
                    (S32)(compensated_x / mDPIScaleX),
                    (S32)(compensated_y / mDPIScaleY)
                );
                
                // XWayland cursor position validation - only for extreme edge cases
                // Simplified to avoid interfering with normal Alt-drag camera operations
                if (mRunningUnderXWayland)
                {
                    F64 time_since_ungrab = LLFrameTimer::getTotalTime() - mLastUngrabTime;
                    
                    // Only fix coordinates if they are clearly invalid (exactly at 0,0 corner)
                    // and we just had an ungrab operation
                    if (time_since_ungrab < 0.05 && // Within 50ms of ungrab (tighter window)
                        winCoord.mX == 0 && winCoord.mY == 0 && // Only fix exact corner coordinates
                        mGrabbyKeyFlags == 0) // And no keys are currently pressed
                    {
                        LL_DEBUGS("XWayland") << "Detected corner jump coordinates: " 
                                             << winCoord.mX << ", " << winCoord.mY 
                                             << ", using saved position" << LL_ENDL;
                        winCoord = mSavedCursorPos;
                    }
                }
                
                LLCoordGL openGlCoord;
                convertCoords(winCoord, &openGlCoord);
                MASK mask = gKeyboard->currentMask(true);

                if (event.button.button == SDL_BUTTON_LEFT)  // left
                    mCallbacks->handleMouseUp(this, openGlCoord, mask);
                else if (event.button.button == SDL_BUTTON_RIGHT)  // right
                    mCallbacks->handleRightMouseUp(this, openGlCoord, mask);
                else if (event.button.button == SDL_BUTTON_MIDDLE)  // middle
                    mCallbacks->handleMiddleMouseUp(this, openGlCoord, mask);
                // don't handle mousewheel here...

                break;
            }

            case SDL_WINDOWEVENT:  // *FIX: handle this?
            {
                if( event.window.event == SDL_WINDOWEVENT_RESIZED
                    /* || event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED*/ ) // <FS:ND> SDL_WINDOWEVENT_SIZE_CHANGED is followed by SDL_WINDOWEVENT_RESIZED, so handling one shall be enough
                {
                    LL_INFOS() << "Handling a resize event: " << event.window.data1 << "x" << event.window.data2 << LL_ENDL;

                    S32 width = llmax(event.window.data1, (S32)mMinWindowWidth);
                    S32 height = llmax(event.window.data2, (S32)mMinWindowHeight);
                    mSurface = SDL_GetWindowSurface( mWindow );
                    
                    // Update DPI scaling on resize
                    updateDPIScaling();
                    
                    // Re-detect XWayland fractional scaling in case display configuration changed
                    detectXWaylandScaling();

                    // *FIX: I'm not sure this is necessary!
                    // <FS:ND> I think is is not
                    // SDL_SetWindowSize(mWindow, width, height);
                    //

                    mCallbacks->handleResize(this, width, height);
                }
                else if( event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ) // <FS:ND> What about SDL_WINDOWEVENT_ENTER (mouse focus)
                {
                    // We have to do our own state massaging because SDL
                    // can send us two unfocus events in a row for example,
                    // which confuses the focus code [SL-24071].
                    mHaveInputFocus = true;
                    
                    // XWayland focus handling improvements
                    if (mRunningUnderXWayland)
                    {
                        LL_DEBUGS("Window") << "XWayland focus regained - validating state" << LL_ENDL;
                        
                        // Validate and clean grab state
#if LL_X11
                        validateGrabState();
#endif
                        
                        // Reset motion tracking
                        mLastValidMousePos = LLCoordWindow(0, 0);
                        getCursorPosition(&mLastValidMousePos);
                        
                        SDL_RaiseWindow(mWindow);
                        SDL_SetWindowInputFocus(mWindow);
                        
                        // Flush all pending events
                        if (mSDL_Display)
                        {
                            maybe_lock_display();
                            XSync(mSDL_Display, False);
                            maybe_unlock_display();
                        }
                    }

                    // GPU State Recovery after focus regain
                    // Alt-tab and focus changes can corrupt VSync state and frame pacing timing
                    int pre_focus_interval = SDL_GL_GetSwapInterval();
                    LL_INFOS("WindowFocus") << "Window focus regained - performing GPU state recovery. "
                                           << "Current swap interval: " << pre_focus_interval << LL_ENDL;
                    
                    // 1. Verify and recover VSync state (SDL2 may have reset swap interval)
                    bool expected_vsync_state = mVSyncEnabled;
                    if (!verifyVSyncState(expected_vsync_state))
                    {
                        LL_WARNS("WindowFocus") << "VSync state corrupted after focus change - forcing recovery" << LL_ENDL;
                        forceVSyncRecovery();
                    }
                    
                    // Log final state after recovery
                    int post_recovery_interval = SDL_GL_GetSwapInterval();
                    if (post_recovery_interval != pre_focus_interval)
                    {
                        LL_INFOS("WindowFocus") << "Swap interval changed during recovery: " 
                                               << pre_focus_interval << " → " << post_recovery_interval << LL_ENDL;
                    }
                    
                    // 2. Reset frame pacing system (timing data is stale after focus loss)
                    if (mFramePacingEnabled)
                    {
                        F64 current_time = LLTimer::getElapsedSeconds();
                        mLastFrameTime = current_time;
                        mFrameTimeAccumulator = 0.0;
                        mConsecutiveFastFrames = 0;
                        mConsecutiveSlowFrames = 0;
                        mLastPacingAdjustment = current_time;
                        LL_DEBUGS("WindowFocus") << "Frame pacing timing reset after focus regain" << LL_ENDL;
                    }
                    
                    // 3. Update multi-monitor VSync configuration (window may have moved)
                    if (mMultiMonitorVSyncEnabled)
                    {
                        updateMonitorRefreshRates();
                        synchronizeMultiMonitorVSync();
                        LL_DEBUGS("WindowFocus") << "Multi-monitor VSync configuration updated" << LL_ENDL;
                    }
                    
                    // 4. Verify OpenGL context is still valid and states are correct
                    GLenum gl_error = glGetError();
                    if (gl_error != GL_NO_ERROR)
                    {
                        LL_WARNS("WindowFocus") << "OpenGL error detected after focus regain: 0x" 
                                               << std::hex << gl_error << std::dec << LL_ENDL;
                        // Clear any accumulated errors
                        while (glGetError() != GL_NO_ERROR) { /* clear error queue */ }
                    }

                    mCallbacks->handleFocus(this);
                }
                else if( event.window.event == SDL_WINDOWEVENT_FOCUS_LOST ) // <FS:ND> What about SDL_WINDOWEVENT_LEAVE (mouse focus)
                {
                    // We have to do our own state massaging because SDL
                    // can send us two unfocus events in a row for example,
                    // which confuses the focus code [SL-24071].
                    mHaveInputFocus = false;

                    // GPU Power Saving during focus loss
                    // Prepare for potential GPU state changes by SDL2/drivers
                    LL_DEBUGS("WindowFocus") << "Window focus lost - preparing for potential GPU state changes" << LL_ENDL;
                    
                    // Note: We don't disable VSync or frame pacing here as that could cause
                    // issues when focus is regained. Instead, we just log that focus was lost
                    // and let the focus regain handler restore proper state.
                    
                    // Disable relative mouse mode on focus loss
                    if (SDL_GetRelativeMouseMode())
                    {
                        SDL_SetRelativeMouseMode(SDL_FALSE);
                        LL_INFOS("Window") << "Disabled relative mouse mode on focus loss" << LL_ENDL;
                    }
                    
                    // XWayland improved focus handling
                    if (mRunningUnderXWayland)
                    {
                        // Under XWayland, distinguish between actual focus loss and spurious events
                        // Only force ungrab for legitimate focus loss (Alt-Tab, minimize, etc.)
                        bool legitimate_focus_loss = true;
                        
                        // If we're in the middle of grab operations, this might be a spurious event
                        // caused by the grab itself - validate it
                        if (mInGrabOperation)
                        {
                            LL_DEBUGS("XWayland") << "Focus lost during grab operation - validating legitimacy" << LL_ENDL;
                            
                            // Check if window is actually minimized or deactivated
                            SDL_Window* focused_window = SDL_GetKeyboardFocus();
                            if (focused_window == mWindow)
                            {
                                // We still have keyboard focus, this is likely spurious
                                LL_DEBUGS("XWayland") << "Keyboard focus still present - ignoring spurious focus lost" << LL_ENDL;
                                legitimate_focus_loss = false;
                            }
                        }
                        
                        if (legitimate_focus_loss)
                        {
                            LL_INFOS("XWayland") << "Legitimate focus lost - resetting grab state" << LL_ENDL;
                            
                            // Clean up relative mouse mode if active
                            if (SDL_GetRelativeMouseMode())
                            {
                                SDL_SetRelativeMouseMode(SDL_FALSE);
                                LL_INFOS("XWayland") << "Disabled relative mouse mode on focus loss" << LL_ENDL;
                            }
                            
                            // Force ungrab if we have any active grab operations
                            if (mInGrabOperation || mGrabbyKeyFlags != 0)
                            {
#if LL_X11
                                maybe_lock_display();
                                XUngrabPointer(mSDL_Display, CurrentTime);
                                XUngrabKeyboard(mSDL_Display, CurrentTime);
                                XSync(mSDL_Display, False);
                                maybe_unlock_display();
#endif
                                
                                // Reset all grab-related state
                                mInGrabOperation = false;
                                mGrabbyKeyFlags = 0;
                                mSkipNextWarpEvent = false;
                                mReallyCapturedCount = 0;
                            }
                            
                            // Only call handleFocusLost for legitimate focus loss
                            mCallbacks->handleFocusLost(this);
                        }
                        else
                        {
                            // Restore focus state for spurious events
                            mHaveInputFocus = true;
                        }
                    }
                    else
                    {
                        // Non-XWayland - use original behavior with GPU state awareness
                        mCallbacks->handleFocusLost(this);
                    }
                }
                else if( event.window.event == SDL_WINDOWEVENT_LEAVE )
                {
                    // Handle mouse leave events - this is different from focus loss
                    LL_DEBUGS("Window") << "Mouse left window" << LL_ENDL;
                    
                    // Under XWayland, mouse leave during grab operations is expected and normal
                    // Don't treat it as focus loss unless we're not actively grabbing
                    if (mRunningUnderXWayland && mInGrabOperation)
                    {
                        LL_DEBUGS("XWayland") << "Mouse leave during grab operation - this is normal" << LL_ENDL;
                    }
                }
                else if( event.window.event == SDL_WINDOWEVENT_ENTER )
                {
                    // Handle mouse enter events
                    LL_DEBUGS("Window") << "Mouse entered window" << LL_ENDL;
                }
                else if( event.window.event == SDL_WINDOWEVENT_MINIMIZED ||
                         event.window.event == SDL_WINDOWEVENT_MAXIMIZED ||
                         event.window.event == SDL_WINDOWEVENT_RESTORED ||
                         event.window.event == SDL_WINDOWEVENT_EXPOSED ||
                         event.window.event == SDL_WINDOWEVENT_SHOWN )
                {
                    mIsMinimized = (event.window.event == SDL_WINDOWEVENT_MINIMIZED);

                    // GPU State Recovery after window restore/show
                    // Minimize/restore can also corrupt GPU state similar to focus changes
                    if (!mIsMinimized && (event.window.event == SDL_WINDOWEVENT_RESTORED || 
                                          event.window.event == SDL_WINDOWEVENT_SHOWN))
                    {
                        int restore_interval = SDL_GL_GetSwapInterval();
                        LL_INFOS("WindowFocus") << "Window restored/shown - verifying GPU state. "
                                               << "Swap interval: " << restore_interval << LL_ENDL;
                        
                        // Verify VSync state after window restore
                        bool expected_vsync_state = mVSyncEnabled;
                        if (!verifyVSyncState(expected_vsync_state))
                        {
                            LL_WARNS("WindowFocus") << "VSync state corrupted after restore - forcing recovery" << LL_ENDL;
                            forceVSyncRecovery();
                        }
                        
                        // Reset frame pacing timing after restore
                        if (mFramePacingEnabled)
                        {
                            mLastFrameTime = LLTimer::getElapsedSeconds();
                            mFrameTimeAccumulator = 0.0;
                        }
                    }

                    mCallbacks->handleActivate(this, !mIsMinimized);
                    LL_INFOS() << "SDL deiconification state switched to " << mIsMinimized << LL_ENDL;
                }

                break;
            }
            case SDL_QUIT:
                if(mCallbacks->handleCloseRequest(this))
                {
                    // Get the app to initiate cleanup.
                    mCallbacks->handleQuit(this);
                    // The app is responsible for calling destroyWindow when done with GL
                }
                break;
            default:
                //LL_INFOS() << "Unhandled SDL event type " << event.type << LL_ENDL;
                break;
        }
    }

    updateCursor();

#if LL_X11
    // This is a good time to stop flashing the icon if our mFlashTimer has
    // expired.
    if (mFlashing && mFlashTimer.hasExpired())
    {
        x11_set_urgent(false);
        mFlashing = false;
    }
#endif // LL_X11
}

static SDL_Cursor *makeSDLCursorFromBMP(const char *filename, int hotx, int hoty)
{
    SDL_Cursor *sdlcursor = NULL;
    SDL_Surface *bmpsurface;

    // Load cursor pixel data from BMP file
    bmpsurface = Load_BMP_Resource(filename);
    if (bmpsurface && bmpsurface->w%8==0)
    {
        SDL_Surface *cursurface;
        LL_DEBUGS() << "Loaded cursor file " << filename << " "
                    << bmpsurface->w << "x" << bmpsurface->h << LL_ENDL;
        cursurface = SDL_CreateRGBSurface (SDL_SWSURFACE,
                                           bmpsurface->w,
                                           bmpsurface->h,
                                           32,
                                           SDL_SwapLE32(0xFFU),
                                           SDL_SwapLE32(0xFF00U),
                                           SDL_SwapLE32(0xFF0000U),
                                           SDL_SwapLE32(0xFF000000U));
        SDL_FillRect(cursurface, NULL, SDL_SwapLE32(0x00000000U));

        // Blit the cursor pixel data onto a 32-bit RGBA surface so we
        // only have to cope with processing one type of pixel format.
        if (0 == SDL_BlitSurface(bmpsurface, NULL,
                                 cursurface, NULL))
        {
            // n.b. we already checked that width is a multiple of 8.
            const int bitmap_bytes = (cursurface->w * cursurface->h) / 8;
            unsigned char *cursor_data = new unsigned char[bitmap_bytes];
            unsigned char *cursor_mask = new unsigned char[bitmap_bytes];
            memset(cursor_data, 0, bitmap_bytes);
            memset(cursor_mask, 0, bitmap_bytes);
            int i,j;
            // Walk the RGBA cursor pixel data, extracting both data and
            // mask to build SDL-friendly cursor bitmaps from.  The mask
            // is inferred by color-keying against 200,200,200
            for (i=0; i<cursurface->h; ++i) {
                for (j=0; j<cursurface->w; ++j) {
                    U8 *pixelp =
                        ((U8*)cursurface->pixels)
                        + cursurface->pitch * i
                        + j*cursurface->format->BytesPerPixel;
                    U8 srcred = pixelp[0];
                    U8 srcgreen = pixelp[1];
                    U8 srcblue = pixelp[2];
                    bool mask_bit = (srcred != 200)
                        || (srcgreen != 200)
                        || (srcblue != 200);
                    bool data_bit = mask_bit && (srcgreen <= 80);//not 0x80
                    unsigned char bit_offset = (cursurface->w/8) * i
                        + j/8;
                    cursor_data[bit_offset] |= (data_bit) << (7 - (j&7));
                    cursor_mask[bit_offset] |= (mask_bit) << (7 - (j&7));
                }
            }
            sdlcursor = SDL_CreateCursor((Uint8*)cursor_data,
                                         (Uint8*)cursor_mask,
                                         cursurface->w, cursurface->h,
                                         hotx, hoty);
            delete[] cursor_data;
            delete[] cursor_mask;
        } else {
            LL_WARNS() << "CURSOR BLIT FAILURE, cursurface: " << cursurface << LL_ENDL;
        }
        SDL_FreeSurface(cursurface);
        SDL_FreeSurface(bmpsurface);
    } else {
        LL_WARNS() << "CURSOR LOAD FAILURE " << filename << LL_ENDL;
    }

    return sdlcursor;
}

void LLWindowSDL::updateCursor()
{
    if (ATIbug) {
        // cursor-updating is very flaky when this bug is
        // present; do nothing.
        return;
    }

    if (mCurrentCursor != mNextCursor)
    {
        if (mNextCursor < UI_CURSOR_COUNT)
        {
            SDL_Cursor *sdlcursor = mSDLCursors[mNextCursor];
            // Try to default to the arrow for any cursors that
            // did not load correctly.
            if (!sdlcursor && mSDLCursors[UI_CURSOR_ARROW])
                sdlcursor = mSDLCursors[UI_CURSOR_ARROW];
            if (sdlcursor)
                SDL_SetCursor(sdlcursor);
        } else {
            LL_WARNS() << "Tried to set invalid cursor number " << mNextCursor << LL_ENDL;
        }
        mCurrentCursor = mNextCursor;
    }
}

//void LLWindowSDL::initCursors()
void LLWindowSDL::initCursors(bool useLegacyCursors) // <FS:LO> Legacy cursor setting from main program
{
    int i;
    // Blank the cursor pointer array for those we may miss.
    for (i=0; i<UI_CURSOR_COUNT; ++i)
    {
        mSDLCursors[i] = NULL;
    }
    // Pre-make an SDL cursor for each of the known cursor types.
    // We hardcode the hotspots - to avoid that we'd have to write
    // a .cur file loader.
    // NOTE: SDL doesn't load RLE-compressed BMP files.
    mSDLCursors[UI_CURSOR_ARROW] = makeSDLCursorFromBMP("llarrow.BMP",0,0);
    mSDLCursors[UI_CURSOR_WAIT] = makeSDLCursorFromBMP("wait.BMP",12,15);
    mSDLCursors[UI_CURSOR_HAND] = makeSDLCursorFromBMP("hand.BMP",7,10);
    mSDLCursors[UI_CURSOR_IBEAM] = makeSDLCursorFromBMP("ibeam.BMP",15,16);
    mSDLCursors[UI_CURSOR_CROSS] = makeSDLCursorFromBMP("cross.BMP",16,14);
    mSDLCursors[UI_CURSOR_SIZENWSE] = makeSDLCursorFromBMP("sizenwse.BMP",14,17);
    mSDLCursors[UI_CURSOR_SIZENESW] = makeSDLCursorFromBMP("sizenesw.BMP",17,17);
    mSDLCursors[UI_CURSOR_SIZEWE] = makeSDLCursorFromBMP("sizewe.BMP",16,14);
    mSDLCursors[UI_CURSOR_SIZENS] = makeSDLCursorFromBMP("sizens.BMP",17,16);
    mSDLCursors[UI_CURSOR_SIZEALL] = makeSDLCursorFromBMP("sizeall.BMP", 17, 17);
    mSDLCursors[UI_CURSOR_NO] = makeSDLCursorFromBMP("llno.BMP",8,8);
    mSDLCursors[UI_CURSOR_WORKING] = makeSDLCursorFromBMP("working.BMP",12,15);
    mSDLCursors[UI_CURSOR_TOOLGRAB] = makeSDLCursorFromBMP("lltoolgrab.BMP",2,13);
    mSDLCursors[UI_CURSOR_TOOLLAND] = makeSDLCursorFromBMP("lltoolland.BMP",1,6);
    mSDLCursors[UI_CURSOR_TOOLFOCUS] = makeSDLCursorFromBMP("lltoolfocus.BMP",8,5);
    mSDLCursors[UI_CURSOR_TOOLCREATE] = makeSDLCursorFromBMP("lltoolcreate.BMP",7,7);
    mSDLCursors[UI_CURSOR_ARROWDRAG] = makeSDLCursorFromBMP("arrowdrag.BMP",0,0);
    mSDLCursors[UI_CURSOR_ARROWCOPY] = makeSDLCursorFromBMP("arrowcop.BMP",0,0);
    mSDLCursors[UI_CURSOR_ARROWDRAGMULTI] = makeSDLCursorFromBMP("llarrowdragmulti.BMP",0,0);
    mSDLCursors[UI_CURSOR_ARROWCOPYMULTI] = makeSDLCursorFromBMP("arrowcopmulti.BMP",0,0);
    mSDLCursors[UI_CURSOR_NOLOCKED] = makeSDLCursorFromBMP("llnolocked.BMP",8,8);
    mSDLCursors[UI_CURSOR_ARROWLOCKED] = makeSDLCursorFromBMP("llarrowlocked.BMP",0,0);
    mSDLCursors[UI_CURSOR_GRABLOCKED] = makeSDLCursorFromBMP("llgrablocked.BMP",2,13);
    mSDLCursors[UI_CURSOR_TOOLTRANSLATE] = makeSDLCursorFromBMP("lltooltranslate.BMP",0,0);
    mSDLCursors[UI_CURSOR_TOOLROTATE] = makeSDLCursorFromBMP("lltoolrotate.BMP",0,0);
    mSDLCursors[UI_CURSOR_TOOLSCALE] = makeSDLCursorFromBMP("lltoolscale.BMP",0,0);
    mSDLCursors[UI_CURSOR_TOOLCAMERA] = makeSDLCursorFromBMP("lltoolcamera.BMP",7,5);
    mSDLCursors[UI_CURSOR_TOOLPAN] = makeSDLCursorFromBMP("lltoolpan.BMP",7,5);
    mSDLCursors[UI_CURSOR_TOOLZOOMIN] = makeSDLCursorFromBMP("lltoolzoomin.BMP",7,5);
    mSDLCursors[UI_CURSOR_TOOLZOOMOUT] = makeSDLCursorFromBMP("lltoolzoomout.BMP", 7, 5);
    mSDLCursors[UI_CURSOR_TOOLPICKOBJECT3] = makeSDLCursorFromBMP("toolpickobject3.BMP",0,0);
    mSDLCursors[UI_CURSOR_TOOLPLAY] = makeSDLCursorFromBMP("toolplay.BMP",0,0);
    mSDLCursors[UI_CURSOR_TOOLPAUSE] = makeSDLCursorFromBMP("toolpause.BMP",0,0);
    mSDLCursors[UI_CURSOR_TOOLMEDIAOPEN] = makeSDLCursorFromBMP("toolmediaopen.BMP",0,0);
    mSDLCursors[UI_CURSOR_PIPETTE] = makeSDLCursorFromBMP("lltoolpipette.BMP",2,28);
    /* <FS:LO> Legacy cursor setting from main program
    mSDLCursors[UI_CURSOR_TOOLSIT] = makeSDLCursorFromBMP("toolsit.BMP",20,15);
    mSDLCursors[UI_CURSOR_TOOLBUY] = makeSDLCursorFromBMP("toolbuy.BMP",20,15);
    mSDLCursors[UI_CURSOR_TOOLOPEN] = makeSDLCursorFromBMP("toolopen.BMP",20,15);*/
    if (useLegacyCursors) {
        mSDLCursors[UI_CURSOR_TOOLSIT] = makeSDLCursorFromBMP("toolsit-legacy.BMP",0,0);
        mSDLCursors[UI_CURSOR_TOOLBUY] = makeSDLCursorFromBMP("toolbuy-legacy.BMP",0,0);
        mSDLCursors[UI_CURSOR_TOOLOPEN] = makeSDLCursorFromBMP("toolopen-legacy.BMP",0,0);
        mSDLCursors[UI_CURSOR_TOOLPAY] = makeSDLCursorFromBMP("toolpay-legacy.BMP",0,0);
    }
    else
    {
        mSDLCursors[UI_CURSOR_TOOLSIT] = makeSDLCursorFromBMP("toolsit.BMP",20,15);
        mSDLCursors[UI_CURSOR_TOOLBUY] = makeSDLCursorFromBMP("toolbuy.BMP",20,15);
        mSDLCursors[UI_CURSOR_TOOLOPEN] = makeSDLCursorFromBMP("toolopen.BMP",20,15);
        mSDLCursors[UI_CURSOR_TOOLPAY] = makeSDLCursorFromBMP("toolbuy.BMP",20,15);
    }
    // </FS:LO>
    mSDLCursors[UI_CURSOR_TOOLPATHFINDING] = makeSDLCursorFromBMP("lltoolpathfinding.BMP", 16, 16);
    mSDLCursors[UI_CURSOR_TOOLPATHFINDING_PATH_START] = makeSDLCursorFromBMP("lltoolpathfindingpathstart.BMP", 16, 16);
    mSDLCursors[UI_CURSOR_TOOLPATHFINDING_PATH_START_ADD] = makeSDLCursorFromBMP("lltoolpathfindingpathstartadd.BMP", 16, 16);
    mSDLCursors[UI_CURSOR_TOOLPATHFINDING_PATH_END] = makeSDLCursorFromBMP("lltoolpathfindingpathend.BMP", 16, 16);
    mSDLCursors[UI_CURSOR_TOOLPATHFINDING_PATH_END_ADD] = makeSDLCursorFromBMP("lltoolpathfindingpathendadd.BMP", 16, 16);
    mSDLCursors[UI_CURSOR_TOOLNO] = makeSDLCursorFromBMP("llno.BMP",8,8);

    if (getenv("LL_ATI_MOUSE_CURSOR_BUG") != NULL && !gGLManager.mIsNVIDIA) {
        LL_INFOS() << "Disabling cursor updating due to LL_ATI_MOUSE_CURSOR_BUG (ATI/AMD GPU detected)" << LL_ENDL;
        ATIbug = true;
    }
}

void LLWindowSDL::quitCursors()
{
    int i;
    if (mWindow)
    {
        for (i=0; i<UI_CURSOR_COUNT; ++i)
        {
            if (mSDLCursors[i])
            {
                SDL_FreeCursor(mSDLCursors[i]);
                mSDLCursors[i] = NULL;
            }
        }
    } else {
        // SDL doesn't refcount cursors, so if the window has
        // already been destroyed then the cursors have gone with it.
        LL_INFOS() << "Skipping quitCursors: mWindow already gone." << LL_ENDL;
        for (i=0; i<UI_CURSOR_COUNT; ++i)
            mSDLCursors[i] = NULL;
    }
}

void LLWindowSDL::captureMouse()
{
    // SDL already enforces the semantics that captureMouse is
    // used for, i.e. that we continue to get mouse events as long
    // as a button is down regardless of whether we left the
    // window, and in a less obnoxious way than SDL_WM_GrabInput
    // which would confine the cursor to the window too.

    LL_DEBUGS() << "LLWindowSDL::captureMouse" << LL_ENDL;
}

void LLWindowSDL::releaseMouse()
{
    // see LWindowSDL::captureMouse()

    LL_DEBUGS() << "LLWindowSDL::releaseMouse" << LL_ENDL;
}

void LLWindowSDL::hideCursor()
{
    if(!mCursorHidden)
    {
        // LL_INFOS() << "hideCursor: hiding" << LL_ENDL;
        mCursorHidden = true;
        mHideCursorPermanent = true;
        SDL_ShowCursor(0);
    }
    else
    {
        // LL_INFOS() << "hideCursor: already hidden" << LL_ENDL;
    }
}

void LLWindowSDL::showCursor()
{
    if(mCursorHidden)
    {
        // LL_INFOS() << "showCursor: showing" << LL_ENDL;
        mCursorHidden = false;
        mHideCursorPermanent = false;
        SDL_ShowCursor(1);
    }
    else
    {
        // LL_INFOS() << "showCursor: already visible" << LL_ENDL;
    }
}

void LLWindowSDL::showCursorFromMouseMove()
{
    if (!mHideCursorPermanent)
    {
        showCursor();
    }
}

void LLWindowSDL::hideCursorUntilMouseMove()
{
    if (!mHideCursorPermanent)
    {
        hideCursor();
        mHideCursorPermanent = false;
    }
}

//
// LLSplashScreenSDL - I don't think we'll bother to implement this; it's
// fairly obsolete at this point.
//
LLSplashScreenSDL::LLSplashScreenSDL()
{
}

LLSplashScreenSDL::~LLSplashScreenSDL()
{
}

void LLSplashScreenSDL::showImpl()
{
}

void LLSplashScreenSDL::updateImpl(const std::string& mesg)
{
}

void LLSplashScreenSDL::hideImpl()
{
}


S32 OSMessageBoxSDL(const std::string& text, const std::string& caption, U32 type)
{
    SDL_MessageBoxData oData = { SDL_MESSAGEBOX_INFORMATION, nullptr, caption.c_str(), text.c_str(), 0, nullptr, nullptr };
    SDL_MessageBoxButtonData btnOk[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, OSBTN_OK, "OK" }};
    SDL_MessageBoxButtonData btnOkCancel [] =  {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, OSBTN_OK, "OK" }, {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, OSBTN_CANCEL, "Cancel"} };
    SDL_MessageBoxButtonData btnYesNo[] = { {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, OSBTN_YES, "Yes" }, {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, OSBTN_NO, "No"} };

    switch (type)
    {
        default:
        case OSMB_OK:
            oData.flags = SDL_MESSAGEBOX_WARNING;
            oData.buttons = btnOk;
            oData.numbuttons = 1;
            break;
        case OSMB_OKCANCEL:
            oData.flags = SDL_MESSAGEBOX_INFORMATION;
            oData.buttons = btnOkCancel;
            oData.numbuttons = 2;
            break;
        case OSMB_YESNO:
            oData.flags = SDL_MESSAGEBOX_INFORMATION;
            oData.buttons = btnYesNo;
            oData.numbuttons = 2;
            break;
    }

    int btn{0};
    if( 0 == SDL_ShowMessageBox( &oData, &btn ) )
        return btn;
    return OSBTN_CANCEL;
}

bool LLWindowSDL::dialogColorPicker( F32 *r, F32 *g, F32 *b)
{
    return (false);
}

/*
        Make the raw keyboard data available - used to poke through to LLQtWebKit so
        that Qt/Webkit has access to the virtual keycodes etc. that it needs
*/
LLSD LLWindowSDL::getNativeKeyData()
{
    LLSD result = LLSD::emptyMap();

    U32 modifiers = 0; // pretend-native modifiers... oh what a tangled web we weave!

    // we go through so many levels of device abstraction that I can't really guess
    // what a plugin under GDK under Qt under SL under SDL under X11 considers
    // a 'native' modifier mask.  this has been sort of reverse-engineered... they *appear*
    // to match GDK consts, but that may be co-incidence.
    modifiers |= (mKeyModifiers & KMOD_LSHIFT) ? 0x0001 : 0;
    modifiers |= (mKeyModifiers & KMOD_RSHIFT) ? 0x0001 : 0;// munge these into the same shift
    modifiers |= (mKeyModifiers & KMOD_CAPS)   ? 0x0002 : 0;
    modifiers |= (mKeyModifiers & KMOD_LCTRL)  ? 0x0004 : 0;
    modifiers |= (mKeyModifiers & KMOD_RCTRL)  ? 0x0004 : 0;// munge these into the same ctrl
    modifiers |= (mKeyModifiers & KMOD_LALT)   ? 0x0008 : 0;// untested
    modifiers |= (mKeyModifiers & KMOD_RALT)   ? 0x0008 : 0;// untested
    // *todo: test ALTs - I don't have a case for testing these.  Do you?
    // *todo: NUM? - I don't care enough right now (and it's not a GDK modifier).

    result["virtual_key"] = (S32)mKeyVirtualKey;
    result["virtual_key_win"] = (S32)LLKeyboardSDL::mapSDL2toWin( mKeyVirtualKey );
    result["modifiers"] = (S32)modifiers;
    result["input_type"] = mInputType;
    return result;
}

#if LL_LINUX || LL_SOLARIS
// extracted from spawnWebBrowser for clarity and to eliminate
//  compiler confusion regarding close(int fd) vs. LLWindow::close()
void exec_cmd(const std::string& cmd, const std::string& arg)
{
    char* const argv[] = {(char*)cmd.c_str(), (char*)arg.c_str(), NULL};
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0)
    { // child
        // disconnect from stdin/stdout/stderr, or child will
        // keep our output pipe undesirably alive if it outlives us.
        // close(0);
        // close(1);
        // close(2);
        // <FS:TS> Reopen stdin, stdout, and stderr to /dev/null.
        //         It's good practice to always have those file
        //         descriptors open to something, lest the exec'd
        //         program actually try to use them.
        FILE *result;
        result = freopen("/dev/null","r",stdin);
        if (result == NULL)
        {
                LL_WARNS() << "Error reopening stdin for web browser: "
                        << strerror(errno) << LL_ENDL;
                }
        result = freopen("/dev/null","w",stdout);
        if (result == NULL)
        {
                LL_WARNS() << "Error reopening stdout for web browser: "
                        << strerror(errno) << LL_ENDL;
                }
        result = freopen("/dev/null","w",stderr);
        if (result == NULL)
        {
                LL_WARNS() << "Error reopening stderr for web browser: "
                        << strerror(errno) << LL_ENDL;
                }
        // end ourself by running the command
        execv(cmd.c_str(), argv);   /* Flawfinder: ignore */
        // if execv returns at all, there was a problem.
        LL_WARNS() << "execv failure when trying to start " << cmd << LL_ENDL;
        _exit(1); // _exit because we don't want atexit() clean-up!
    } else {
        if (pid > 0)
        {
            // parent - wait for child to die
            int childExitStatus;
            waitpid(pid, &childExitStatus, 0);
        } else {
            LL_WARNS() << "fork failure." << LL_ENDL;
        }
    }
}
#endif

// Open a URL with the user's default web browser.
// Must begin with protocol identifier.
void LLWindowSDL::spawnWebBrowser(const std::string& escaped_url, bool async)
{
    bool found = false;
    S32 i;
    for (i = 0; i < gURLProtocolWhitelistCount; i++)
    {
        if (escaped_url.find(gURLProtocolWhitelist[i]) != std::string::npos)
        {
            found = true;
            break;
        }
    }

    if (!found)
    {
        LL_WARNS() << "spawn_web_browser called for url with protocol not on whitelist: " << escaped_url << LL_ENDL;
        return;
    }

    LL_INFOS() << "spawn_web_browser: " << escaped_url << LL_ENDL;

#if LL_LINUX
# if LL_X11
    if (mSDL_Display)
    {
        maybe_lock_display();
        // Just in case - before forking.
        XSync(mSDL_Display, False);
        maybe_unlock_display();
    }
# endif // LL_X11

    std::string cmd, arg;
    cmd  = gDirUtilp->getAppRODataDir();
    cmd += gDirUtilp->getDirDelimiter();
    cmd += "etc";
    cmd += gDirUtilp->getDirDelimiter();
    cmd += "launch_url.sh";
    arg = escaped_url;
    exec_cmd(cmd, arg);
#endif // LL_LINUX

    LL_INFOS() << "spawn_web_browser returning." << LL_ENDL;
}

void LLWindowSDL::openFile(const std::string& file_name)
{
    spawnWebBrowser("file://"+file_name,true);
}

void *LLWindowSDL::getPlatformWindow()
{
    return NULL;
}

void LLWindowSDL::bringToFront()
{
    // This is currently used when we are 'launched' to a specific
    // map position externally.
    LL_INFOS() << "bringToFront" << LL_ENDL;
#if LL_X11
    if (mSDL_Display && !mFullscreen)
    {
        maybe_lock_display();
        XRaiseWindow(mSDL_Display, mSDL_XWindowID);
        XSync(mSDL_Display, False);
        maybe_unlock_display();
    }
#endif // LL_X11
}

//static
std::vector<std::string> LLWindowSDL::getDynamicFallbackFontList()
{
    // Use libfontconfig to find us a nice ordered list of fallback fonts
    // specific to this system.
    std::string final_fallback("/usr/share/fonts/truetype/kochi/kochi-gothic.ttf");
    const int max_font_count_cutoff = 40; // fonts are expensive in the current system, don't enumerate an arbitrary number of them
    // Our 'ideal' font properties which define the sorting results.
    // slant=0 means Roman, index=0 means the first face in a font file
    // (the one we actually use), weight=80 means medium weight,
    // spacing=0 means proportional spacing.
    std::string sort_order("slant=0:index=0:weight=80:spacing=0");
    // elide_unicode_coverage removes fonts from the list whose unicode
    // range is covered by fonts earlier in the list.  This usually
    // removes ~90% of the fonts as redundant (which is great because
    // the font list can be huge), but might unnecessarily reduce the
    // renderable range if for some reason our FreeType actually fails
    // to use some of the fonts we want it to.
    const bool elide_unicode_coverage = true;
    std::vector<std::string> rtns;
    FcFontSet *fs = NULL;
    FcPattern *sortpat = NULL;

    LL_INFOS() << "Getting system font list from FontConfig..." << LL_ENDL;

    // If the user has a system-wide language preference, then favor
    // fonts from that language group.  This doesn't affect the types
    // of languages that can be displayed, but ensures that their
    // preferred language is rendered from a single consistent font where
    // possible.
    FL_Locale *locale = NULL;
    FL_Success success = FL_FindLocale(&locale, FL_MESSAGES);
    if (success != 0)
    {
        if (success >= 2 && locale->lang) // confident!
        {
            LL_INFOS("AppInit") << "Language " << locale->lang << LL_ENDL;
            LL_INFOS("AppInit") << "Location " << locale->country << LL_ENDL;
            LL_INFOS("AppInit") << "Variant " << locale->variant << LL_ENDL;

            LL_INFOS() << "Preferring fonts of language: "
                << locale->lang
                << LL_ENDL;
            sort_order = "lang=" + std::string(locale->lang) + ":"
                + sort_order;
        }
    }
    FL_FreeLocale(&locale);

    if (!FcInit())
    {
        LL_WARNS() << "FontConfig failed to initialize." << LL_ENDL;
        rtns.push_back(final_fallback);
        return rtns;
    }

    sortpat = FcNameParse((FcChar8*) sort_order.c_str());
    if (sortpat)
    {
        // Sort the list of system fonts from most-to-least-desirable.
        FcResult result;
        fs = FcFontSort(NULL, sortpat, elide_unicode_coverage,
                NULL, &result);
        FcPatternDestroy(sortpat);
    }

    int found_font_count = 0;
    if (fs)
    {
        // Get the full pathnames to the fonts, where available,
        // which is what we really want.
        found_font_count = fs->nfont;
        for (int i=0; i<fs->nfont; ++i)
        {
            FcChar8 *filename;
            if (FcResultMatch == FcPatternGetString(fs->fonts[i],
                                FC_FILE, 0,
                                &filename)
                && filename)
            {
                rtns.push_back(std::string((const char*)filename));
                if (rtns.size() >= max_font_count_cutoff)
                    break; // hit limit
            }
        }
        FcFontSetDestroy (fs);
    }

    LL_DEBUGS() << "Using font list: " << LL_ENDL;
    for (std::vector<std::string>::iterator it = rtns.begin();
         it != rtns.end();
         ++it)
    {
        LL_DEBUGS() << "  file: " << *it << LL_ENDL;
    }
    LL_INFOS() << "Using " << rtns.size() << "/" << found_font_count << " system fonts." << LL_ENDL;

    rtns.push_back(final_fallback);
    return rtns;
}

// <FS:Zi> Make shared context work on Linux for multithreaded OpenGL
class sharedContext
{
    public:
        SDL_GLContext mContext;
};

void* LLWindowSDL::createSharedContext()
{
    sharedContext* sc = new sharedContext();
    sc->mContext = SDL_GL_CreateContext(mWindow);
    if (sc->mContext)
    {
        SDL_GL_SetSwapInterval(0);
        SDL_GL_MakeCurrent(mWindow, mContext);

        LLCoordScreen size;
        if (getSize(&size))
        {
            // tickle window size to fix font going blocky on login screen since SDL 2.24.0
            size.mX--;
            setSize(size);
            size.mX++;
            setSize(size);
        }

        LL_DEBUGS() << "Creating shared OpenGL context successful!" << LL_ENDL;

        return (void*)sc;
    }

    LL_WARNS() << "Creating shared OpenGL context failed!" << LL_ENDL;

    return nullptr;
}

void LLWindowSDL::makeContextCurrent(void* context)
{
    LL_PROFILER_GPU_CONTEXT;
    SDL_GL_MakeCurrent(mWindow, ((sharedContext*)context)->mContext);
}

void LLWindowSDL::destroySharedContext(void* context)
{
    sharedContext* sc = (sharedContext*)context;

    SDL_GL_DeleteContext(sc->mContext);

    delete sc;
}

void LLWindowSDL::toggleVSync(bool enable_vsync)
{
    mVSyncEnabled = enable_vsync;
    
    if (enable_vsync)
    {
        // Nvidia on Linux often has issues with adaptive vsync (-1)
        // Try regular vsync first for Nvidia, adaptive for others
        bool try_adaptive_first = !gGLManager.mIsNVIDIA;
        bool vsync_success = false;
        
        if (try_adaptive_first)
        {
            // Try adaptive vsync first (-1) for non-Nvidia GPUs
            if (SDL_GL_SetSwapInterval(-1) != -1)
            {
                mCurrentSwapInterval = -1;
                LL_DEBUGS() << "Adaptive vsync enabled" << LL_ENDL;
                vsync_success = true;
            }
            else
            {
                LL_INFOS() << "Failed to enable adaptive vsync, trying regular vsync" << LL_ENDL;
            }
        }
        
        if (!vsync_success)
        {
            // Try regular vsync (1)
            if (SDL_GL_SetSwapInterval(1) != -1)
            {
                mCurrentSwapInterval = 1;
                LL_DEBUGS() << "Vsync enabled" << LL_ENDL;
                vsync_success = true;
            }
            else if (try_adaptive_first)
            {
                LL_WARNS() << "Failed to enable vsync" << LL_ENDL;
            }
        }
        
        // If regular vsync worked and this is Nvidia, optionally try adaptive as fallback
        if (vsync_success && !try_adaptive_first)
        {
            // For Nvidia, try adaptive as secondary option if regular worked
            if (SDL_GL_SetSwapInterval(-1) != -1)
            {
                mCurrentSwapInterval = -1;
                LL_DEBUGS() << "Nvidia adaptive vsync enabled as secondary option" << LL_ENDL;
            }
            else
            {
                // Fall back to regular vsync for Nvidia
                SDL_GL_SetSwapInterval(1);
                mCurrentSwapInterval = 1;
                LL_DEBUGS() << "Nvidia using regular vsync (adaptive not supported)" << LL_ENDL;
            }
        }
        
        if (!vsync_success)
        {
            mCurrentSwapInterval = 0;
            mVSyncEnabled = false;
            LL_WARNS() << "Failed to enable any form of vsync" << LL_ENDL;
            
            // Try environment variable fallback for Nvidia
            if (gGLManager.mIsNVIDIA)
            {
                #ifdef LL_LINUX
                setenv("__GL_SYNC_TO_VBLANK", "1", 1);
                LL_INFOS() << "Set __GL_SYNC_TO_VBLANK=1 for Nvidia fallback" << LL_ENDL;
                #endif
            }
        }
    }
    else
    {
        SDL_GL_SetSwapInterval(0);
        mCurrentSwapInterval = 0;
        LL_DEBUGS() << "Vsync disabled" << LL_ENDL;
        
        // Clear Nvidia environment variable if set
        if (gGLManager.mIsNVIDIA)
        {
            #ifdef LL_LINUX
            unsetenv("__GL_SYNC_TO_VBLANK");
            #endif
        }
    }
    
    mLastVerifiedSwapInterval = mCurrentSwapInterval;
    mLastVSyncVerifyTime = LLTimer::getElapsedSeconds();
}

bool LLWindowSDL::verifyVSyncState(bool expected_state)
{
    // Check if enough time has passed since last verification (avoid spam)
    F64 current_time = LLTimer::getElapsedSeconds();
    if (current_time - mLastVSyncVerifyTime < 1.0) // Check at most once per second
    {
        return mVSyncEnabled == expected_state;
    }
    
    // Get current swap interval from SDL
    int actual_interval = SDL_GL_GetSwapInterval();
    
    // Verify state matches expectation with EXACT interval checking
    bool state_matches = false;
    bool interval_corrupted = false;
    
    if (expected_state)
    {
        // VSync should be enabled - interval should be 1 or -1 (adaptive), not corrupted values like 2,4,8
        state_matches = (actual_interval == 1 || actual_interval == -1);
        
        // Detect corrupted intervals that cause low FPS (e.g., 8 causes 15fps on 120Hz monitors)
        if (actual_interval != 0 && actual_interval != 1 && actual_interval != -1)
        {
            interval_corrupted = true;
            LL_WARNS() << "VSync interval corrupted! Expected 1 or -1, got " << actual_interval 
                       << " (causes " << (120.0f / actual_interval) << " FPS on 120Hz monitor)" << LL_ENDL;
        }
    }
    else
    {
        // VSync should be disabled (interval should be zero)
        state_matches = (actual_interval == 0);
    }
    
    mLastVSyncVerifyTime = current_time;
    
    if (!state_matches || interval_corrupted)
    {
        LL_WARNS() << "VSync verification failed! Expected state: " << expected_state 
                   << ", Actual interval: " << actual_interval 
                   << ", Stored interval: " << mCurrentSwapInterval << LL_ENDL;
        
        // Attempt recovery
        forceVSyncRecovery();
        
        // Re-check after recovery
        int recovered_interval = SDL_GL_GetSwapInterval();
        LL_INFOS() << "VSync recovery result: interval changed from " << actual_interval 
                   << " to " << recovered_interval << LL_ENDL;
        
        state_matches = expected_state ? (recovered_interval == 1 || recovered_interval == -1) 
                                       : (recovered_interval == 0);
    }
    else if (actual_interval != mLastVerifiedSwapInterval)
    {
        LL_INFOS() << "VSync interval changed from " << mLastVerifiedSwapInterval 
                   << " to " << actual_interval << LL_ENDL;
        mLastVerifiedSwapInterval = actual_interval;
    }
    
    return state_matches;
}

void LLWindowSDL::forceVSyncRecovery()
{
    int corrupted_interval = SDL_GL_GetSwapInterval();
    LL_INFOS() << "Forcing VSync recovery - corrupted interval: " << corrupted_interval 
               << ", target state: " << mVSyncEnabled << LL_ENDL;
    
    // Store current state and force re-application
    bool current_state = mVSyncEnabled;
    
    // Disable first to completely reset state
    mVSyncEnabled = false;
    SDL_GL_SetSwapInterval(0);
    
    // Verify disable worked
    int disabled_interval = SDL_GL_GetSwapInterval();
    if (disabled_interval != 0)
    {
        LL_WARNS() << "Failed to disable VSync, interval still: " << disabled_interval << LL_ENDL;
    }
    
    // Small delay to ensure state change is processed
    #ifdef LL_LINUX
    usleep(2000); // 2ms delay for stability
    #endif
    
    // Re-enable if it was supposed to be enabled with retry logic
    if (current_state)
    {
        const int MAX_RETRIES = 3;
        bool recovery_successful = false;
        
        for (int retry = 0; retry < MAX_RETRIES && !recovery_successful; retry++)
        {
            if (retry > 0)
            {
                LL_INFOS() << "VSync recovery attempt " << (retry + 1) << " of " << MAX_RETRIES << LL_ENDL;
                #ifdef LL_LINUX
                usleep(1000); // Additional delay between retries
                #endif
            }
            
            toggleVSync(true);
            
            // Verify the recovery worked
            int new_interval = SDL_GL_GetSwapInterval();
            if (new_interval == 1 || new_interval == -1) // Valid VSync intervals
            {
                recovery_successful = true;
                LL_INFOS() << "VSync recovery successful! Interval: " << new_interval 
                           << " (was corrupted at " << corrupted_interval << ")" << LL_ENDL;
                mCurrentSwapInterval = new_interval;
            }
            else
            {
                LL_WARNS() << "VSync recovery attempt " << (retry + 1) << " failed. Got interval: " 
                           << new_interval << " (expected 1 or -1)" << LL_ENDL;
            }
        }
        
        if (!recovery_successful)
        {
            LL_WARNS() << "VSync recovery FAILED after " << MAX_RETRIES << " attempts. "
                       << "Performance may be degraded. Consider disabling VSync." << LL_ENDL;
        }
    }
}

bool LLWindowSDL::isVSyncSupported()
{
    // Test if VSync is supported by attempting to enable it
    int original_interval = SDL_GL_GetSwapInterval();
    
    // Try to set vsync to 1
    int result = SDL_GL_SetSwapInterval(1);
    
    // Restore original interval
    SDL_GL_SetSwapInterval(original_interval);
    
    return (result != -1);
}

int LLWindowSDL::getCurrentSwapInterval()
{
    return SDL_GL_GetSwapInterval();
}

// </FS:Zi>

void LLWindowSDL::enableIME(bool b)
{
    mIMEEnabled = b;

    if (mIMEEnabled)
    {
        SDL_SetHint( SDL_HINT_IME_INTERNAL_EDITING, "1");
        SDL_StopTextInput();
    }
    else
    {
        SDL_SetHint( SDL_HINT_IME_INTERNAL_EDITING, "0");
        SDL_StartTextInput();
    }
}

// IME - International input compositing, i.e. for Japanese / Chinese text input
// Put the IME window at the right place (near current text input).
// Point coordinates should be the top of the current text line.
void LLWindowSDL::setLanguageTextInput(const LLCoordGL& position)
{
    if (!mIMEEnabled)
    {
        return;
    }

    LLCoordWindow win_pos;
    convertCoords( position, &win_pos );

    SDL_Rect r;
    r.x = win_pos.mX;
    r.y = win_pos.mY;
    r.w = 500;
    r.h = 16;

    SDL_SetTextInputRect(&r);
}

// IME - International input compositing, i.e. for Japanese / Chinese text input
void LLWindowSDL::allowLanguageTextInput(LLPreeditor *preeditor, bool b)
{
    if (!mIMEEnabled)
    {
        return;
    }

    if (preeditor != mPreeditor && !b)
    {
        // This condition may occur with a call to
        // setEnabled(bool) from LLTextEditor or LLLineEditor
        // when the control is not focused.
        // We need to silently ignore the case so that
        // the language input status of the focused control
        // is not disturbed.
        return;
    }

    // Take care of old and new preeditors.
    if (preeditor != mPreeditor || !b)
    {
        mPreeditor = (b ? preeditor : nullptr);
    }

    if (b)
    {
        SDL_StartTextInput();
    }
    else
    {
        SDL_StopTextInput();
    }
}

// ============================================================================
// Advanced Frame Pacing System Implementation
// ============================================================================

void LLWindowSDL::initializeFramePacing()
{
    mFramePacingEnabled = true;  // Enable by default, can be controlled by settings
    mTargetFrameTime = 1.0 / 60.0; // Default 60 FPS target (16.67ms)
    mLastFrameTime = LLTimer::getElapsedSeconds();
    mFrameTimeAccumulator = 0.0;
    mAverageFrameTime = mTargetFrameTime;
    mFrameVariance = 0.0;
    mFrameCount = 0;
    mConsecutiveFastFrames = 0;
    mConsecutiveSlowFrames = 0;
    mLastPacingAdjustment = LLTimer::getElapsedSeconds();
    mAdaptivePacingEnabled = true;
    mMinFrameTime = 1.0 / 240.0; // Cap at 240 FPS max (4.17ms)
    mMaxFrameTime = 1.0 / 30.0;  // Cap at 30 FPS min (33.33ms)
    
    LL_INFOS("Window") << "Frame Pacing System initialized - Target: " 
                       << (int)(1.0 / mTargetFrameTime) << " FPS" << LL_ENDL;
}

void LLWindowSDL::updateFramePacing()
{
    if (!mFramePacingEnabled)
        return;
        
    F64 current_time = LLTimer::getElapsedSeconds();
    F64 frame_time = current_time - mLastFrameTime;
    
    recordFrameTime(frame_time);
    
    // Adaptive frame time adjustment every 60 frames or every 2 seconds
    if (mFrameCount % 60 == 0 || (current_time - mLastPacingAdjustment) > 2.0)
    {
        adjustTargetFrameTime();
        mLastPacingAdjustment = current_time;
    }
    
    // Handle multi-monitor VSync synchronization
    synchronizeMultiMonitorVSync();
}

void LLWindowSDL::applyFramePacing()
{
    if (!mFramePacingEnabled)
        return;
        
    F64 current_time = LLTimer::getElapsedSeconds();
    
    // Validate time progression to avoid infinite loops or negative time
    if (current_time <= mLastFrameTime)
    {
        LL_WARNS("FramePacing") << "Time regression detected: current=" << current_time 
                               << " last=" << mLastFrameTime << ", disabling frame pacing" << LL_ENDL;
        mFramePacingEnabled = false;
        return;
    }
    
    F64 elapsed_frame_time = current_time - mLastFrameTime;
    
    // Sanity check for reasonable frame times (avoid extremely long or short frames)
    if (elapsed_frame_time > 1.0) // More than 1 second per frame
    {
        LL_WARNS("FramePacing") << "Extremely long frame time detected: " << elapsed_frame_time 
                               << " seconds, resetting" << LL_ENDL;
        mLastFrameTime = current_time;
        return;
    }
    
    // Calculate how much time we need to wait to meet target frame rate
    F64 remaining_time = mTargetFrameTime - elapsed_frame_time;
    
    // Only sleep if we have significant time remaining (>1ms) to avoid busy waiting
    if (remaining_time > 0.001)
    {
        // Use high precision sleep
        #ifdef LL_LINUX
        // Linux: use nanosleep for better precision
        struct timespec sleep_time;
        sleep_time.tv_sec = 0;
        sleep_time.tv_nsec = (long)(remaining_time * 1000000000.0);
        nanosleep(&sleep_time, nullptr);
        #else
        // Other platforms: use SDL_Delay (millisecond precision)
        SDL_Delay((Uint32)(remaining_time * 1000.0));
        #endif
    }
    
    mLastFrameTime = LLTimer::getElapsedSeconds();
}

bool LLWindowSDL::shouldSkipFrame()
{
    if (!mFramePacingEnabled)
        return false;
        
    F64 current_time = LLTimer::getElapsedSeconds();
    F64 elapsed_frame_time = current_time - mLastFrameTime;
    
    // Skip frame if we're running significantly faster than target
    // This helps reduce unnecessary GPU load
    if (elapsed_frame_time < (mTargetFrameTime * 0.5))
    {
        return true;
    }
    
    return false;
}

void LLWindowSDL::recordFrameTime(F64 frame_time)
{
    mFrameCount++;
    
    // Update running average using exponential moving average
    F64 alpha = 0.1; // Smoothing factor
    mAverageFrameTime = alpha * frame_time + (1.0 - alpha) * mAverageFrameTime;
    
    // Calculate variance for stability measurement
    F64 diff = frame_time - mAverageFrameTime;
    mFrameVariance = alpha * (diff * diff) + (1.0 - alpha) * mFrameVariance;
    
    // Track consecutive fast/slow frames for adaptive adjustment
    if (frame_time < mTargetFrameTime * 0.9)
    {
        mConsecutiveFastFrames++;
        mConsecutiveSlowFrames = 0;
    }
    else if (frame_time > mTargetFrameTime * 1.1)
    {
        mConsecutiveSlowFrames++;
        mConsecutiveFastFrames = 0;
    }
    else
    {
        mConsecutiveFastFrames = 0;
        mConsecutiveSlowFrames = 0;
    }
}

void LLWindowSDL::adjustTargetFrameTime()
{
    if (!mAdaptivePacingEnabled)
        return;
        
    F64 stability_threshold = 0.001; // 1ms variance threshold
    
    // If we're consistently running fast and the system is stable
    if (mConsecutiveFastFrames > 30 && mFrameVariance < stability_threshold)
    {
        // Increase target frame rate slightly (reduce frame time)
        mTargetFrameTime *= 0.95;
        mTargetFrameTime = llmax(mTargetFrameTime, mMinFrameTime);
        
        LL_DEBUGS("Window") << "Frame pacing: Increased target FPS to " 
                           << (int)(1.0 / mTargetFrameTime) << LL_ENDL;
    }
    // If we're consistently running slow
    else if (mConsecutiveSlowFrames > 10)
    {
        // Decrease target frame rate (increase frame time)
        mTargetFrameTime *= 1.05;
        mTargetFrameTime = llmin(mTargetFrameTime, mMaxFrameTime);
        
        LL_DEBUGS("Window") << "Frame pacing: Decreased target FPS to " 
                           << (int)(1.0 / mTargetFrameTime) << LL_ENDL;
    }
    
    // Reset counters after adjustment
    mConsecutiveFastFrames = 0;
    mConsecutiveSlowFrames = 0;
}

// Multi-Monitor VSync Support Implementation
void LLWindowSDL::initializeMultiMonitorVSync()
{
    LL_INFOS("MultiMonitor") << "Initializing multi-monitor VSync support" << LL_ENDL;
    
    mMultiMonitorVSyncEnabled = false;
    mPrimaryMonitorIndex = 0;
    mCurrentMonitorIndex = -1;
    mMultiMonitorSyncTarget = 1.0/60.0; // Default to 60Hz
    mCrossMonitorSyncDetected = false;
    
    // Detect monitor configuration
    detectMonitorConfiguration();
    
    // Enable multi-monitor VSync if multiple monitors detected
    if (mMonitorRefreshRates.size() > 1)
    {
        mMultiMonitorVSyncEnabled = true;
        mMultiMonitorSyncTarget = calculateOptimalSyncTarget();
        
        LL_INFOS("MultiMonitor") << "Multi-monitor VSync enabled with " << mMonitorRefreshRates.size() 
                                << " monitors, sync target: " << (int)(1.0/mMultiMonitorSyncTarget) << " Hz" << LL_ENDL;
    }
    else
    {
        LL_DEBUGS("MultiMonitor") << "Single monitor detected, using standard VSync" << LL_ENDL;
    }
}

void LLWindowSDL::detectMonitorConfiguration()
{
    mMonitorRefreshRates.clear();
    
    int num_displays = SDL_GetNumVideoDisplays();
    if (num_displays < 1)
    {
        LL_WARNS("MultiMonitor") << "Failed to detect video displays" << LL_ENDL;
        return;
    }
    
    LL_INFOS("MultiMonitor") << "Detected " << num_displays << " display(s)" << LL_ENDL;
    
    for (int i = 0; i < num_displays; i++)
    {
        SDL_DisplayMode current_mode;
        if (SDL_GetCurrentDisplayMode(i, &current_mode) == 0)
        {
            mMonitorRefreshRates.push_back(current_mode.refresh_rate);
            
            LL_DEBUGS("MultiMonitor") << "Display " << i << ": " 
                                     << current_mode.w << "x" << current_mode.h 
                                     << " @ " << current_mode.refresh_rate << "Hz" << LL_ENDL;
        }
        else
        {
            // Default to 60Hz if we can't get refresh rate
            mMonitorRefreshRates.push_back(60);
            LL_WARNS("MultiMonitor") << "Failed to get display mode for display " << i << ", assuming 60Hz" << LL_ENDL;
        }
    }
}

void LLWindowSDL::updateMonitorRefreshRates()
{
    // Re-detect monitor configuration (useful for dynamic monitor changes)
    detectMonitorConfiguration();
    
    // Recalculate optimal sync target
    if (mMultiMonitorVSyncEnabled)
    {
        F64 new_sync_target = calculateOptimalSyncTarget();
        if (fabs(new_sync_target - mMultiMonitorSyncTarget) > 0.001) // Significant change
        {
            mMultiMonitorSyncTarget = new_sync_target;
            LL_INFOS("MultiMonitor") << "Updated multi-monitor sync target to " 
                                    << (int)(1.0/mMultiMonitorSyncTarget) << " Hz" << LL_ENDL;
        }
    }
}

void LLWindowSDL::synchronizeMultiMonitorVSync()
{
    if (!mMultiMonitorVSyncEnabled)
        return;
        
    // Get current monitor
    S32 current_monitor = getCurrentMonitor();
    
    // Check if we've moved to a different monitor
    if (current_monitor != mCurrentMonitorIndex)
    {
        if (mCurrentMonitorIndex >= 0)
        {
            mCrossMonitorSyncDetected = true;
            LL_DEBUGS("MultiMonitor") << "Window moved from monitor " << mCurrentMonitorIndex 
                                     << " to monitor " << current_monitor << LL_ENDL;
        }
        
        mCurrentMonitorIndex = current_monitor;
        
        // Update frame pacing target if we moved to a different monitor
        if (current_monitor >= 0 && current_monitor < (S32)mMonitorRefreshRates.size())
        {
            F64 new_target = 1.0 / mMonitorRefreshRates[current_monitor];
            if (fabs(new_target - mTargetFrameTime) > 0.001)
            {
                mTargetFrameTime = new_target;
                LL_INFOS("MultiMonitor") << "Adjusted frame timing for monitor " << current_monitor 
                                        << " (" << mMonitorRefreshRates[current_monitor] << " Hz)" << LL_ENDL;
            }
        }
    }
    
    // Apply cross-monitor synchronization logic
    if (mCrossMonitorSyncDetected)
    {
        // Use the optimal sync target that works across all monitors
        mTargetFrameTime = mMultiMonitorSyncTarget;
        mCrossMonitorSyncDetected = false; // Reset flag
        
        LL_DEBUGS("MultiMonitor") << "Applied cross-monitor synchronization" << LL_ENDL;
    }
}

S32 LLWindowSDL::getCurrentMonitor()
{
    if (!mWindow)
        return -1;
        
    // Get window display index
    int display_index = SDL_GetWindowDisplayIndex(mWindow);
    if (display_index < 0)
    {
        LL_WARNS("MultiMonitor") << "Failed to get window display index: " << SDL_GetError() << LL_ENDL;
        return -1;
    }
    
    return display_index;
}

F64 LLWindowSDL::calculateOptimalSyncTarget()
{
    if (mMonitorRefreshRates.empty())
        return 1.0/60.0; // Default 60Hz
        
    // Find the Greatest Common Divisor (GCD) of all refresh rates
    // This ensures smooth operation across all monitors
    
    S32 gcd_rate = mMonitorRefreshRates[0];
    for (size_t i = 1; i < mMonitorRefreshRates.size(); i++)
    {
        S32 a = gcd_rate;
        S32 b = mMonitorRefreshRates[i];
        
        // Calculate GCD using Euclidean algorithm
        while (b != 0)
        {
            S32 temp = b;
            b = a % b;
            a = temp;
        }
        gcd_rate = a;
    }
    
    // Use the GCD as the optimal sync rate
    // This ensures all monitors can display frames without tearing
    if (gcd_rate <= 0)
        gcd_rate = 30; // Fallback to 30Hz minimum
        
    // Don't go below 30Hz for usability
    if (gcd_rate < 30)
        gcd_rate = 30;
        
    F64 sync_target = 1.0 / gcd_rate;
    
    LL_DEBUGS("MultiMonitor") << "Calculated optimal sync target: " << gcd_rate << " Hz" << LL_ENDL;
    
    return sync_target;
}

#endif // LL_SDL
