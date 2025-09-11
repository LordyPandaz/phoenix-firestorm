# XWayland Coordinate Debugging Guide

## Overview
Added comprehensive coordinate debugging system to trace mouse events through the entire transformation pipeline and test different compensation strategies.

## What Was Added

### 1. Enhanced Debug Logging
- **CoordinateFlow** log category: Detailed coordinate tracing through 5 stages
- **XWaylandTesting** log category: Testing mode changes and status
- **XWaylandScaling** log category: Existing compensation logging (enhanced)

### 2. Runtime Testing Methods
- `testCoordinateCompensation(int mode)` - Test different compensation strategies
- `setXWaylandCompensationEnabled(bool enabled)` - Toggle compensation on/off

### 3. Testing Modes
- **Mode 0**: Compensation DISABLED - Test if offset goes away
- **Mode 1**: NORMAL compensation (current: ÷1.75) - Baseline behavior  
- **Mode 2**: INVERTED compensation (×1.75) - Test if direction is wrong
- **Mode 3**: DOUBLE compensation (÷1.75²) - Test double scaling theory

## Coordinate Transformation Stages

Each mouse click is logged through these stages:

1. **1-RAW_SDL**: Raw coordinates from SDL event
2. **2-XWAYLAND_COMPENSATED**: After XWayland compensation (if enabled)
3. **3-DPI_ADJUSTED**: After DPI scaling division
4. **4-WINDOW_COORD**: LLCoordWindow (integer conversion)
5. **5-OPENGL_COORD**: Final LLCoordGL sent to viewer

## How to Debug

### Step 1: Set Test Mode via Environment Variable
Set the environment variable to automatically test different modes:
```bash
# Test Mode 0 (compensation disabled)
export FIRESTORM_COORDINATE_TEST_MODE=0
./firestorm

# Test Mode 1 (normal compensation)
export FIRESTORM_COORDINATE_TEST_MODE=1
./firestorm

# Test Mode 2 (inverted compensation)
export FIRESTORM_COORDINATE_TEST_MODE=2
./firestorm

# Test Mode 3 (double compensation)
export FIRESTORM_COORDINATE_TEST_MODE=3
./firestorm

# Default behavior (no environment variable)
unset FIRESTORM_COORDINATE_TEST_MODE
./firestorm
```

### Step 2: Enable Logging (Optional)
For detailed coordinate tracing, you can also enable log categories:
- Open Firestorm
- Go to Help -> About Firestorm -> Copy to Clipboard (has log location)
- Or check `~/.firestorm_x64/logs/` for log files
- Look for messages with `CoordinateFlow`, `XWaylandTesting`, `XWaylandScaling`

### Step 3: Test Each Mode
1. Start Firestorm with a test mode
2. Try clicking on UI elements (buttons, menus, panels)
3. Note which mode allows accurate clicking
4. Compare different modes to find the one that works

### Step 4: Identify the Fix
- **Mode 0 works** → XWayland compensation should be disabled for UI events
- **Mode 2 works** → XWayland compensation direction is wrong (should multiply, not divide)
- **Mode 3 works** → Double scaling issue confirmed
- **None work** → Problem is elsewhere in the coordinate pipeline

## What to Look For

### Expected Patterns
- **Raw SDL coordinates**: Should match your physical click position
- **After XWayland compensation**: Coordinates should be smaller (÷1.75)
- **After DPI scaling**: May be further adjusted
- **Final coordinates**: What the UI system sees for hit testing

### Diagnostic Questions
1. **Does Mode 0 fix the offset?** → Compensation shouldn't be applied to UI events
2. **Does Mode 2 fix the offset?** → Compensation direction is wrong  
3. **Are coordinates getting too small?** → Look for values decreasing through stages
4. **Do pie menu coordinates behave differently?** → Different code path for world vs UI

## Example Log Output
```
CoordinateFlow: STAGE: 1-RAW_SDL | COORDS: (350,200) | DESC: Raw SDL event coordinates
CoordinateFlow: STAGE: 2-XWAYLAND_COMPENSATED | COORDS: (200,114) | DESC: After XWayland compensation  
CoordinateFlow: STAGE: 3-DPI_ADJUSTED | COORDS: (200,114) | DESC: After DPI scaling division
CoordinateFlow: STAGE: 4-WINDOW_COORD | COORDS: (200,114) | DESC: LLCoordWindow (integer)
CoordinateFlow: STAGE: 5-OPENGL_COORD | COORDS: (200,886) | DESC: Final LLCoordGL sent to viewer
XWaylandTesting: Testing mode 0: Compensation DISABLED
```

## Next Steps

Based on test results:
1. If Mode 0 fixes it → Remove XWayland compensation for UI events
2. If Mode 2 fixes it → Invert compensation direction  
3. If no mode works → Issue is in later coordinate transformation stages
4. Compare working pie menu coordinates with broken UI coordinates

## Files Modified
- `indra/llwindow/llwindowsdl2.h`: Added debug method declarations
- `indra/llwindow/llwindowsdl2.cpp`: Added logging and testing methods

## Usage Example
```cpp
// To test in running viewer:
LLWindowSDL* window = (LLWindowSDL*)gViewerWindow->getWindow();
window->testCoordinateCompensation(0); // Test disabled mode
// Click UI element and observe logs
window->testCoordinateCompensation(2); // Test inverted mode  
// Click UI element and observe logs
```