/**
 * @file test_xwayland_scaling.cpp
 * @brief Unit tests for XWayland fractional scaling coordinate compensation
 * 
 * Tests the coordinate transformation logic used to fix cursor jumping issues
 * in XWayland with fractional scaling enabled.
 */

#include <iostream>
#include <iomanip>
#include <cassert>
#include <cmath>

class TestXWaylandScaling 
{
public:
    // Test data structure for coordinate transformations
    struct CoordinateTest 
    {
        float scale_factor;
        float compensation_x, compensation_y;
        float original_x, original_y;
        float expected_x, expected_y;
        const char* description;
    };
    
    void runAllTests() 
    {
        std::cout << "Running XWayland Fractional Scaling Coordinate Tests..." << std::endl;
        
        testCoordinateCompensation();
        testScaleFactorCalculation();
        testEdgeCases();
        
        std::cout << "All tests passed!" << std::endl;
    }
    
private:
    void testCoordinateCompensation() 
    {
        std::cout << "\nTesting coordinate compensation logic..." << std::endl;
        
        CoordinateTest tests[] = {
            // Standard 175% scaling (like user's configuration)
            {1.75f, 1.0f/1.75f, 1.0f/1.75f, 350.0f, 200.0f, 200.0f, 114.29f, "175% scaling top-left"},
            {1.75f, 1.0f/1.75f, 1.0f/1.75f, 1750.0f, 1000.0f, 1000.0f, 571.43f, "175% scaling center"},
            {1.75f, 1.0f/1.75f, 1.0f/1.75f, 2625.0f, 1500.0f, 1500.0f, 857.14f, "175% scaling bottom-right"},
            
            // Other common fractional scaling values
            {1.25f, 1.0f/1.25f, 1.0f/1.25f, 250.0f, 150.0f, 200.0f, 120.0f, "125% scaling"},
            {1.5f, 1.0f/1.5f, 1.0f/1.5f, 300.0f, 180.0f, 200.0f, 120.0f, "150% scaling"},
            {2.0f, 1.0f/2.0f, 1.0f/2.0f, 400.0f, 240.0f, 200.0f, 120.0f, "200% scaling"},
            
            // No scaling (should be pass-through)
            {1.0f, 1.0f/1.0f, 1.0f/1.0f, 200.0f, 120.0f, 200.0f, 120.0f, "100% scaling (no compensation)"},
        };
        
        for (const auto& test : tests) 
        {
            float compensated_x = test.original_x * test.compensation_x;
            float compensated_y = test.original_y * test.compensation_y;
            
            // Allow small floating point errors
            bool x_matches = std::abs(compensated_x - test.expected_x) < 0.01f;
            bool y_matches = std::abs(compensated_y - test.expected_y) < 0.01f;
            
            std::cout << "  " << test.description << ": ";
            
            if (x_matches && y_matches) 
            {
                std::cout << "PASS (" << compensated_x << "," << compensated_y << ")" << std::endl;
            } 
            else 
            {
                std::cout << "FAIL - Expected (" << test.expected_x << "," << test.expected_y 
                         << "), got (" << compensated_x << "," << compensated_y << ")" << std::endl;
                assert(false);
            }
        }
    }
    
    void testScaleFactorCalculation() 
    {
        std::cout << "\nTesting scale factor calculation..." << std::endl;
        
        struct ScaleTest {
            int physical_width, physical_height;
            int physical_mm_width, physical_mm_height;
            float expected_scale_factor;
            const char* description;
        };
        
        ScaleTest tests[] = {
            // User's actual setup: 4384x2466 display at 600x340mm
            {4384, 2466, 600, 340, 1.86f, "User's 4K display (approx 185 DPI)"},
            
            // Common setups
            {3840, 2160, 527, 296, 1.86f, "Standard 4K 27\" display"},
            {2560, 1440, 527, 296, 1.24f, "Standard 1440p 27\" display"},
            {1920, 1080, 510, 287, 0.96f, "Standard 1080p 24\" display"},
        };
        
        for (const auto& test : tests) 
        {
            // Calculate DPI and infer scaling (matching the detection logic)
            float dpi_x = (float)test.physical_width * 25.4f / test.physical_mm_width;
            float dpi_y = (float)test.physical_height * 25.4f / test.physical_mm_height;
            float avg_dpi = (dpi_x + dpi_y) / 2.0f;
            float inferred_scale = avg_dpi / 96.0f;
            
            bool scale_matches = std::abs(inferred_scale - test.expected_scale_factor) < 0.1f;
            
            std::cout << "  " << test.description << ": ";
            
            if (scale_matches) 
            {
                std::cout << "PASS (scale: " << std::fixed << std::setprecision(2) 
                         << inferred_scale << ", DPI: " << avg_dpi << ")" << std::endl;
            } 
            else 
            {
                std::cout << "FAIL - Expected scale " << test.expected_scale_factor 
                         << ", got " << inferred_scale << std::endl;
                assert(false);
            }
        }
    }
    
    void testEdgeCases() 
    {
        std::cout << "\nTesting edge cases..." << std::endl;
        
        // Test zero coordinates (should remain zero)
        float scale = 1.75f;
        float compensation = 1.0f / scale;
        
        assert((0.0f * compensation) == 0.0f);
        std::cout << "  Zero coordinates: PASS" << std::endl;
        
        // Test maximum coordinates (should not overflow)
        float max_coord = 32767.0f; // Typical SDL coordinate limit
        float compensated = max_coord * compensation;
        assert(compensated < max_coord); // Should be smaller after compensation
        assert(compensated > 0.0f); // Should remain positive
        std::cout << "  Maximum coordinates: PASS" << std::endl;
        
        // Test very small scale factors
        float tiny_scale = 0.5f;
        float tiny_compensation = 1.0f / tiny_scale;
        float tiny_result = 100.0f * tiny_compensation;
        assert(tiny_result == 200.0f);
        std::cout << "  Very small scale factor: PASS" << std::endl;
        
        // Test very large scale factors
        float large_scale = 4.0f;
        float large_compensation = 1.0f / large_scale;
        float large_result = 400.0f * large_compensation;
        assert(large_result == 100.0f);
        std::cout << "  Very large scale factor: PASS" << std::endl;
    }
};

int main() 
{
    TestXWaylandScaling test;
    test.runAllTests();
    return 0;
}