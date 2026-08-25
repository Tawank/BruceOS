// JetConfig.hpp — Bruce's frontend configuration for the Jet renderer
// (components/jet). See components/jet/src/JetConfig.example.hpp for what
// every option does; only the choices specific to this app are commented
// below. Kept deliberately conservative for a first 3D example: flat-shaded,
// untextured, unlit, single full-resolution buffer straight into
// display__draw_rgb_bitmap(). Raise the bar once this baseline is proven on
// real hardware, not before.

// Bruce's display panels top out around 320x240 on the boards this targets
// today; per JetConfig.example.hpp's guidance, low-res targets don't benefit
// from extra fixed-point headroom.
#define JET32_WORLD_SCALE 4

#define RENDER_TILE_BUFFER 0
#define TILE_WIDTH 32
#define TILE_HEIGHT 32

#define FAST_Z 1
#define LAZY_Z 0

// No alpha/transparency in this demo (Primitives::createDebugCube is fully
// opaque), so the dither/fade machinery is dead weight -- leave it off.
#define SCREEN_DOOR_ALPHA 0
#define SKIP_ZERO_AREA_TRIANGLES 1
#define NOISE_ALPHA 0

// A depth buffer would cost screenWidth*screenHeight*2 more bytes on top of
// the colour framebuffer already allocated in main.cpp; SORT_TRIANGLES
// (painter's algorithm) is enough for one convex cube and much cheaper.
#define Z_BUFFERING 0
#define SORT_TRIANGLES 1
#define SORT_SCENE_OBJECTS 0
#define SORT_SCENE_REVERSE 0

// Requires SCREEN_DOOR_ALPHA; off along with it.
#define DEPTH_ALPHA_BLEND 0

#define TEXTURE_MAPPING 0
#define PERSPECTIVE_CORRECT_TEXTURES 0
#define BILINEAR_FILTER 0

// Primitives::createDebugCube colours each face directly (see main.cpp) --
// no lighting pass needed for a demo whose whole point is "a rotating cube
// with a visible orientation", and skipping LIGHTING keeps every vertex a
// flat 12 bytes instead of paying for a normal + Lambert term never used.
#define LIGHTING 0
#define Z_BRIGHTNESS 0

#define FLOAT_CAMERA_ANGLES 1
#define FLOAT_SIN_CACHE_SCALE 10
#define FLOAT_TAN_CACHE_SCALE 1

// One full-resolution buffer straight into display__draw_rgb_bitmap(); no
// pixel-doubling or interlacing logic in this app to match HALF_WIDTH/FIELD
// buffer layouts.
#define HALF_WIDTH_BUFFERS 0
#define FIELD_BUFFERS 0
#define SSR_FIELD_REFLECT 0

#define POSTFX_CRT 0
#define POSTFX_CELLSHADING 0
#define POSTFX_ANTIALIASING 0
#define POSTFX_BLOOM 0
#define POSTFX_MOTION_BLUR 0
#define POSTFX_CHROMATIC 0
#define POSTFX_PIXELATE 0

#define CRT_SCANLINE_INTENSITY 48
#define MOTION_BLUR_STRENGTH 50
#define CHROMATIC_OFFSET 2
#define PIXELATE_SIZE 4
#define CELLSHADING_CELL_BITS 4

#define DEBUG_OVERDRAW 0

#define zBrightFar (1600 * JET32_WORLD_SCALE)
#define zBrightNear (200 * JET32_WORLD_SCALE)
#define zBrightScale 48

#define depthFogFar (8192 * JET32_WORLD_SCALE)
#define depthFogNear (6144 * JET32_WORLD_SCALE)

#define CHECKERBOARD_MODE 0
#define CHECKERBOARD_RECONSTRUCTION 0

// No screen-space picking (touch/cursor hit-testing) in this demo.
#define MAX_PICK_QUERIES 0

// Detected via ESP_PLATFORM (injected by the ESP-IDF CMake toolchain) so a
// desktop build of Jet elsewhere never pulls in esp_attr.h/IRAM_ATTR.
#if defined(ESP_PLATFORM)
#define ESP32
#endif
