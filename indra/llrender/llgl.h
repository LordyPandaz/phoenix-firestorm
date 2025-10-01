/**
 * @file llgl.h
 * @brief LLGL definition
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

#ifndef LL_LLGL_H
#define LL_LLGL_H

// This file contains various stuff for handling gl extensions and other gl related stuff.

#include <string>
#include <boost/unordered_map.hpp>
#include <list>
#include <mutex>
#include <atomic>

#include "llerror.h"
#include "v4color.h"
#include "llstring.h"
#include "stdtypes.h"
#include "v4math.h"
#include "llplane.h"
#include "llgltypes.h"
#include "llinstancetracker.h"

#include "llglheaders.h"
#include "glm/mat4x4.hpp"

extern bool gDebugGL;
extern bool gDebugSession;
extern bool gDebugGLSession;
extern llofstream gFailLog;

#define LL_GL_ERRS LL_ERRS("RenderState")

void ll_init_fail_log(std::string filename);

void ll_fail(std::string msg);

void ll_close_fail_log();

class LLSD;

// Manage GL extensions...
class LLGLManager
{
public:
    LLGLManager();

    bool initGL();
    void shutdownGL();

#if LL_WINDOWS
    void initWGL(); // Initializes stupid WGL extensions
#endif

    std::string getRawGLString(); // For sending to simulator

    bool mInited;
    bool mIsDisabled;

    // OpenGL limits
    S32 mMaxSamples;
    S32 mNumTextureImageUnits;
    S32 mMaxSampleMaskWords;
    S32 mMaxColorTextureSamples;
    S32 mMaxDepthTextureSamples;
    S32 mMaxIntegerSamples;
    S32 mGLMaxVertexRange;
    S32 mGLMaxIndexRange;
    S32 mGLMaxTextureSize;
    F32 mMaxAnisotropy = 0.f;
    S32 mMaxUniformBlockSize = 0;
    S32 mMaxVaryingVectors = 0;

    // GPU Performance Monitoring
    U32 mGPUFrameCount = 0;
    F64 mGPUFrameTime = 0.0;
    U32 mGLErrorCount = 0;
    F64 mLastFrameTime = 0.0;

    // GL 4.x capabilities
    bool mHasCubeMapArray = false;
    bool mHasDebugOutput = false;
    bool mHasTransformFeedback = false;
    bool mHasAnisotropic = false;

    // Vendor-specific extensions
    bool mHasAMDAssociations = false;
    bool mHasNVXGpuMemoryInfo = false;
    bool mHasATIMemInfo = false;
    
    // Enhanced texture compression support
    bool mHasS3TC = false;      // S3TC/DXT compression
    bool mHasRGTC = false;      // RGTC compression (better for normal maps)
    bool mHasBPTC = false;      // BPTC compression (high quality, modern)
    bool mHasASTC = false;      // ASTC compression (mobile/modern)

    bool mIsAMD;
    bool mIsNVIDIA;
    bool mIsIntel;
    bool mIsApple = false;

    // hints to the render pipe
    U32 mDownScaleMethod = 0; // see settings.xml RenderDownScaleMethod

#if LL_DARWIN
    // Needed to distinguish problem cards on older Macs that break with Materials
    bool mIsMobileGF;
#endif

    // Whether this version of GL is good enough for SL to use
    bool mHasRequirements;

    S32 mDriverVersionMajor;
    S32 mDriverVersionMinor;
    S32 mDriverVersionRelease;
    F32 mGLVersion; // e.g = 1.4
    S32 mGLSLVersionMajor;
    S32 mGLSLVersionMinor;
    std::string mDriverVersionVendorString;
    std::string mGLVersionString;

    U32 mVRAM; // VRAM in MB
    S32 mVRAMDetected; // <FS:Beq/> The amount detected/reported by the OS/Drivers. If different to mVRAM there is an override in place.

    // Runtime VRAM monitoring (atomic for thread safety)
    std::atomic<U32> mCurrentVRAM{0}; // Currently available VRAM in MB (atomic: accessed from render thread)
    U32 mLastEvictionCount = 0; // Last known eviction count for pressure detection (protected by mutex)
    std::atomic<U32> mTextureEvictionCount{0}; // Total texture evictions since startup (atomic: read from multiple threads)
    std::atomic<F64> mLastEvictionCheck{0.0}; // Last time we checked for evictions (atomic: thread-safe timestamp)

    // GPU Memory Pressure Management
    std::atomic<bool> mMemoryPressureDetected{false}; // Atomic: read from multiple threads
    std::atomic<F64> mLastMemoryPressureCheck{0.0}; // Atomic: thread-safe timestamp
    U32 mMemoryPressureLevel = 0; // 0=None, 1=Low, 2=Medium, 3=High, 4=Critical (protected by mutex)
    U32 mTextureMemoryBudget = 0; // Dynamic texture memory budget in MB (protected by mutex)
    U32 mOriginalTextureMemoryBudget = 0; // Original budget before emergency reductions (protected by mutex)
    F64 mLastBudgetReductionTime = 0.0; // Time of last budget reduction (protected by mutex)
    std::atomic<U32> mTextureMemoryUsed{0}; // Current texture memory usage in MB (atomic for thread safety)
    F64 mMemoryPressureThreshold = 0.85; // Trigger pressure handling at 85% usage (const-like, no protection needed)
    mutable std::recursive_mutex mMemoryPressureMutex; // Thread safety for memory tracking (recursive to prevent deadlock)

    // Context loss detection
    std::atomic<bool> mContextLost{false}; // Atomic: can be checked from multiple threads
    std::string getGLInfoString();
    void printGLInfoString();
    void getGLInfo(LLSD& info);

    void asLLSD(LLSD& info);
    
    // VRAM monitoring functions
    bool checkVRAMPressure();
    void updateVRAMStats();
    U32 getCurrentVRAM();
    U32 getEvictionCount();
    
    // Shader Cache System
    bool mShaderCacheEnabled = true;
    std::string mShaderCacheDir;
    U32 mShaderCacheHits = 0;
    U32 mShaderCacheMisses = 0;
    U32 mShaderCacheSize = 0; // Size in KB
    U32 mMaxShaderCacheSize = 256 * 1024; // Max 256MB cache
    F64 mLastShaderCacheCleanup = 0.0;
    
    void initializeShaderCache();
    bool loadShaderFromCache(const std::string& hash, std::string& vertex_source, std::string& fragment_source);
    void saveShaderToCache(const std::string& hash, const std::string& vertex_source, const std::string& fragment_source);
    std::string generateShaderHash(const std::string& vertex_source, const std::string& fragment_source);
    void cleanupShaderCache();
    void validateShaderCache();
    std::string getShaderCachePath(const std::string& hash);
    void updateShaderCacheStats();
    
    // GSP Firmware Detection (Nvidia)
    bool mGSPFirmwareDetected = false;
    bool mGSPFirmwareEnabled = false;
    std::string mGSPFirmwareVersion;
    
    void detectGSPFirmware();
    bool isGSPFirmwarePresent();
    std::string getGSPFirmwareInfo();
    
    // GPU Memory Pressure Management
    void initializeMemoryPressureSystem();
    void updateMemoryPressure();
    bool isMemoryPressureDetected() const { return mMemoryPressureDetected; }
    U32 getMemoryPressureLevel() const {
        std::lock_guard<std::recursive_mutex> lock(mMemoryPressureMutex);
        return mMemoryPressureLevel;
    }
    void handleMemoryPressure(U32 level);
    bool requestTextureMemory(U32 size_mb);
    void releaseTextureMemory(U32 size_mb);
    void emergencyMemoryCleanup();
    
    // Context loss detection
    bool checkContextLoss();
    void handleContextLoss();

    // In ALL CAPS
    std::string mGLVendor;
    std::string mGLVendorShort;

    // In ALL CAPS
    std::string mGLRenderer;

private:
    void initExtensions();
    void initGLStates();
};

extern LLGLManager gGLManager;

class LLQuaternion;
class LLMatrix4;

void rotate_quat(LLQuaternion& rotation);

void flush_glerror(); // Flush GL errors when we know we're handling them correctly.

void log_glerror();
void assert_glerror();

void clear_glerror();

// Enhanced GPU error handling and recovery
bool validate_gl_state(); // Validate current OpenGL state consistency
bool recover_from_gl_error(GLenum error); // Attempt to recover from specific GL errors  
void log_gpu_diagnostics(); // Log comprehensive GPU diagnostic information
bool check_framebuffer_status_safe(); // Safe framebuffer completeness check with recovery

// GPU Performance Monitoring
void update_gpu_performance_stats(); // Update frame time and error statistics
void reset_gpu_performance_stats(); // Reset performance counters
void log_gpu_performance_summary(); // Log performance summary

# define stop_glerror() assert_glerror()
# define llglassertok() assert_glerror()

// stop_glerror is still needed on OS X but has performance implications
// use macro below to conditionally add stop_glerror to non-release builds
// on OS X
#if LL_DARWIN && !LL_RELEASE_FOR_DOWNLOAD
#define STOP_GLERROR stop_glerror()
#else
#define STOP_GLERROR
#endif

#define llglassertok_always() assert_glerror()

////////////////////////
//
// Note: U32's are GLEnum's...
//

// This is a class for GL state management

/*
    GL STATE MANAGEMENT DESCRIPTION

    LLGLState and its two subclasses, LLGLEnable and LLGLDisable, manage the current
    enable/disable states of the GL to prevent redundant setting of state within a
    render path or the accidental corruption of what state the next path expects.

    Essentially, wherever you would call glEnable set a state and then
    subsequently reset it by calling glDisable (or vice versa), make an instance of
    LLGLEnable with the state you want to set, and assume it will be restored to its
    original state when that instance of LLGLEnable is destroyed.  It is good practice
    to exploit stack frame controls for optimal setting/unsetting and readability of
    code.  In llglstates.h, there are a collection of helper classes that define groups
    of enables/disables that can cause multiple states to be set with the creation of
    one instance.

    Sample usage:

    //disable lighting for rendering hud objects
    //INCORRECT USAGE
    LLGLEnable blend(GL_BLEND);
    renderHUD();
    LLGLDisable blend(GL_BLEND);

    //CORRECT USAGE
    {
        LLGLEnable blend(GL_BLEND);
        renderHUD();
    }

    If a state is to be set on a conditional, the following mechanism
    is useful:

    {
        LLGLEnable blend(blend_hud ? GL_GL_BLEND: 0);
        renderHUD();
    }

    A LLGLState initialized with a parameter of 0 does nothing.

    LLGLState works by maintaining a map of the current GL states, and ignoring redundant
    enables/disables.  If a redundant call is attempted, it becomes a noop, otherwise,
    it is set in the constructor and reset in the destructor.

    For debugging GL state corruption, running with debug enabled will trigger asserts
    if the existing GL state does not match the expected GL state.

*/

#include "boost/function.hpp"

class LLGLState
{
public:
    static void initClass();
    static void restoreGL();

    static void resetTextureStates();
    static void dumpStates();

    // make sure GL blend function, GL states, and GL color mask match
    // what we expect
    //  writeAlpha - whether or not writing to alpha channel is expected
    static void checkStates(GLboolean writeAlpha = GL_TRUE);

protected:
    static boost::unordered_map<LLGLenum, LLGLboolean> sStateMap;

public:
    enum { CURRENT_STATE = -2, DISABLED_STATE = 0, ENABLED_STATE = 1 };
    LLGLState(LLGLenum state, S32 enabled = CURRENT_STATE);
    ~LLGLState();
    void setEnabled(S32 enabled);
    void enable() { setEnabled(ENABLED_STATE); }
    void disable() { setEnabled(DISABLED_STATE); }
protected:
    LLGLenum mState;
    bool mWasEnabled;
    bool mIsEnabled;
};

// New LLGLState class wrappers that don't depend on actual GL flags.
class LLGLEnableBlending : public LLGLState
{
public:
    LLGLEnableBlending(bool enable);
};

class LLGLEnableAlphaReject : public LLGLState
{
public:
    LLGLEnableAlphaReject(bool enable);
};

// Enable with functor
class LLGLEnableFunc : LLGLState
{
public:
    LLGLEnableFunc(LLGLenum state, bool enable, boost::function<void()> func)
        : LLGLState(state, enable)
    {
        if (enable)
        {
            func();
        }
    }
};

/// TODO: Being deprecated.
class LLGLEnable : public LLGLState
{
public:
    LLGLEnable(LLGLenum state) : LLGLState(state, ENABLED_STATE) {}
};

/// TODO: Being deprecated.
class LLGLDisable : public LLGLState
{
public:
    LLGLDisable(LLGLenum state) : LLGLState(state, DISABLED_STATE) {}
};

/*
  Store and modify projection matrix to create an oblique
  projection that clips to the specified plane.  Oblique
  projections alter values in the depth buffer, so this
  class should not be used mid-renderpass.

  Restores projection matrix on destruction.
  GL_MODELVIEW_MATRIX is active whenever program execution
  leaves this class.
  Does not stack.
  Caches inverse of projection matrix used in gGLObliqueProjectionInverse
*/
class LLGLUserClipPlane
{
public:

    LLGLUserClipPlane(const LLPlane& plane, const glm::mat4& modelview, const glm::mat4& projection, bool apply = true);
    ~LLGLUserClipPlane();

    void setPlane(F32 a, F32 b, F32 c, F32 d);
    void disable();

private:
    bool mApply;

    glm::mat4 mProjection;
    glm::mat4 mModelview;
};

/*
  Modify and load projection matrix to push depth values to far clip plane.

  Restores projection matrix on destruction.
  Saves/restores matrix mode around projection manipulation.
  Does not stack.
*/
class LLGLSquashToFarClip
{
public:
    LLGLSquashToFarClip();
    LLGLSquashToFarClip(const glm::mat4& projection, U32 layer = 0);

    void setProjectionMatrix(glm::mat4 projection, U32 layer);

    ~LLGLSquashToFarClip();
};

/*
    Interface for objects that need periodic GL updates applied to them.
    Used to synchronize GL updates with GL thread.
*/
class LLGLUpdate
{
public:

    static std::list<LLGLUpdate*> sGLQ;

    bool mInQ;
    LLGLUpdate()
        : mInQ(false)
    {
    }
    virtual ~LLGLUpdate()
    {
        if (mInQ)
        {
            std::list<LLGLUpdate*>::iterator iter = std::find(sGLQ.begin(), sGLQ.end(), this);
            if (iter != sGLQ.end())
            {
                sGLQ.erase(iter);
            }
        }
    }
    virtual void updateGL() = 0;
};

const U32 FENCE_WAIT_TIME_NANOSECONDS = 1000;  //1 ms

class LLGLFence
{
public:
    virtual ~LLGLFence()
    {
    }

    virtual void placeFence() = 0;
    virtual bool isCompleted() = 0;
    virtual void wait() = 0;
};

class LLGLSyncFence : public LLGLFence
{
public:
    GLsync mSync;

    LLGLSyncFence();
    virtual ~LLGLSyncFence();

    void placeFence();
    bool isCompleted();
    void wait();
};

extern LLMatrix4 gGLObliqueProjectionInverse;

#include "llglstates.h"

void init_glstates();

void parse_gl_version( S32* major, S32* minor, S32* release, std::string* vendor_specific, std::string* version_string );

extern bool gClothRipple;
extern bool gHeadlessClient;
extern bool gNonInteractive;
extern bool gGLActive;

// Deal with changing glext.h definitions for newer SDK versions, specifically
// with MAC OSX 10.5 -> 10.6


#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT GL_DEPTH_ATTACHMENT_EXT
#endif

#ifndef GL_STENCIL_ATTACHMENT
#define GL_STENCIL_ATTACHMENT GL_STENCIL_ATTACHMENT_EXT
#endif

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER GL_FRAMEBUFFER_EXT
#define GL_DRAW_FRAMEBUFFER GL_DRAW_FRAMEBUFFER_EXT
#define GL_READ_FRAMEBUFFER GL_READ_FRAMEBUFFER_EXT
#define GL_FRAMEBUFFER_COMPLETE GL_FRAMEBUFFER_COMPLETE_EXT
#define GL_FRAMEBUFFER_UNSUPPORTED GL_FRAMEBUFFER_UNSUPPORTED_EXT
#define GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT_EXT
#define GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT_EXT
#define glGenFramebuffers glGenFramebuffersEXT
#define glBindFramebuffer glBindFramebufferEXT
#define glCheckFramebufferStatus glCheckFramebufferStatusEXT
#define glBlitFramebuffer glBlitFramebufferEXT
#define glDeleteFramebuffers glDeleteFramebuffersEXT
#define glFramebufferRenderbuffer glFramebufferRenderbufferEXT
#define glFramebufferTexture2D glFramebufferTexture2DEXT
#endif

#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER GL_RENDERBUFFER_EXT
#define glGenRenderbuffers glGenRenderbuffersEXT
#define glBindRenderbuffer glBindRenderbufferEXT
#define glRenderbufferStorage glRenderbufferStorageEXT
#define glRenderbufferStorageMultisample glRenderbufferStorageMultisampleEXT
#define glDeleteRenderbuffers glDeleteRenderbuffersEXT
#endif

#ifndef GL_COLOR_ATTACHMENT
#define GL_COLOR_ATTACHMENT GL_COLOR_ATTACHMENT_EXT
#endif

#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 GL_COLOR_ATTACHMENT0_EXT
#endif

#ifndef GL_COLOR_ATTACHMENT1
#define GL_COLOR_ATTACHMENT1 GL_COLOR_ATTACHMENT1_EXT
#endif

#ifndef GL_COLOR_ATTACHMENT2
#define GL_COLOR_ATTACHMENT2 GL_COLOR_ATTACHMENT2_EXT
#endif

#ifndef GL_COLOR_ATTACHMENT3
#define GL_COLOR_ATTACHMENT3 GL_COLOR_ATTACHMENT3_EXT
#endif


#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 GL_DEPTH24_STENCIL8_EXT
#endif

#endif // LL_LLGL_H
