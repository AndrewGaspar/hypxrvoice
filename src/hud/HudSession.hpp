#pragma once

// Platform macros must precede EGL/GLES, which must precede openxr_platform.h — same
// include contract as hypxrpaper's Session.hpp / HypXRland's XRSession.cpp.
#define XR_USE_PLATFORM_EGL
#define XR_USE_GRAPHICS_API_OPENGL_ES

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <string>

class CEgl;

// The hypxrvoice HUD overlay session. A single VIEW-space (head-locked)
// XrCompositionLayerQuad rendered above HypXRland's monitors as a second overlay
// session (XrSessionCreateInfoOverlayEXTX, distinct sessionLayersPlacement). Content
// is a premultiplied-alpha RGBA texture uploaded ONLY when it changes (hypxrpaper's
// upload-once idle-monitor trick); fade/opacity is a free per-frame scalar via
// XR_KHR_composition_layer_color_scale_bias — no texture re-upload for fades.
//
// Single-threaded, EGL context held current for the whole session (Monado's GL-fence
// contract by construction, HypXRland commit 95c541a8). Content arrives as
// newline-delimited HudMsg JSON on stdin; EOF => clean exit.
class CHudSession {
  public:
    struct SParams {
        float   posX = 0.f, posY = -0.25f, posZ = -1.0f; // VIEW-space centre, metres.
        float   sizeW = 0.42f;                            // quad width, metres (height from texture aspect).
        float   opacity = 0.92f;                          // peak opacity ceiling.
        int32_t overlayZ = 20;                            // sessionLayersPlacement.
        int     texW = 768, texH = 384;                   // swapchain / raster size.
    };

    CHudSession() = default;
    ~CHudSession();

    bool init(CEgl& egl, const SParams& p);
    void run();      // pump until stdin EOF / session exit.
    void destroy();

  private:
    bool createInstance();
    bool getSystem();
    bool createSession(CEgl& egl);
    bool createSpace();
    bool chooseSwapchainFormat();
    bool createSwapchain();

    void pollEvents();
    bool renderFrame();
    void uploadImage(const uint8_t* rgba); // acquire/upload/release one swapchain image.
    bool readStdin();                       // drain stdin; update m_view; return false on EOF.

    CEgl*      m_egl      = nullptr;
    SParams    m_p;

    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_systemId = XR_NULL_SYSTEM_ID;
    XrSession  m_session  = XR_NULL_HANDLE;
    XrSpace    m_viewSpace = XR_NULL_HANDLE;

    XrSwapchain m_swapchain      = XR_NULL_HANDLE;
    int64_t     m_swapchainFormat = 0;

    XrSessionState m_state          = XR_SESSION_STATE_UNKNOWN;
    bool           m_sessionRunning = false;
    bool           m_exit           = false;
    bool           m_exitRequested  = false;
    bool           m_haveColorScale = false;
    bool           m_stdinEof       = false;

    // Current content + fade clock (renderer monotonic ms).
    int64_t     m_shownAtMs = 0; // when the current content began showing.
    bool        m_hasContent = false;
    std::string m_stdinBuf;

    // Fade envelope of the current content.
    int   m_riseMs = 110, m_holdMs = -1, m_fadeMs = 450;
    float m_ceil   = 0.92f;

    std::string m_runtimeName;
};
