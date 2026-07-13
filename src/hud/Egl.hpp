#pragma once

#include <EGL/egl.h>
#include <string>

struct gbm_device;

// CEgl owns a GBM-backed EGL display + an OpenGL ES 3 context, selected on a
// specific DRM render node.
//
// Adapted from HypXRland's CXRGraphics (src/openxr/XRGraphics.cpp), heavily
// simplified: hypxrpaper never imports monitor DMA-BUFs, so there is no blit
// program, no external-OES texture, and no Aquamarine coupling. We just need one
// context to upload the panorama once and to satisfy the OpenXR EGL binding.
//
// EGL fence-contract lesson (HypXRland commit 95c541a8): the runtime (Monado)
// inserts native GL fences into *our* command stream during
// xrAcquire..xrRelease and xrEndFrame and never binds a context itself for the
// SYNCHRONIZE. So our context must be CURRENT across those calls. We keep it
// current on the single (main) thread for the whole session — see Session.cpp.
class CEgl {
  public:
    CEgl()  = default;
    ~CEgl() = default;

    // Pick the EGL display on a DRM render node and create the GLES3 context.
    // gpuOverride: a specific /dev/dri/renderD* path, or empty to scan for the
    // first working render node (mirrors CXRGraphics' scan order). Returns false
    // on failure.
    bool init(const std::string& gpuOverride);

    // Teardown: context current -> caller deletes GL objects first -> then this.
    void destroy();

    // Make the context current / unbind. current() binds to no surface (we render
    // only into FBO-wrapped swapchain textures and pbuffer-less contexts).
    void makeCurrent();
    void release();

    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLContext m_context = EGL_NO_CONTEXT;
    EGLConfig  m_config  = nullptr;

  private:
    bool selectDisplay(const std::string& gpuOverride);

    struct gbm_device* m_gbm   = nullptr;
    int                m_gbmFd = -1;
};
