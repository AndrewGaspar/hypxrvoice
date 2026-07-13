#include "Egl.hpp"
#include "Log.hpp"

// EGL/GLES headers only — this TU never touches OpenXR, so it needs no platform
// macros (mirrors XRGraphics.cpp).
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>

#include <fcntl.h>
#include <unistd.h>
#include <vector>

#include <gbm.h>
#include <xf86drm.h>

#ifndef EGL_DRM_RENDER_NODE_FILE_EXT
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#endif
#ifndef EGL_DRM_DEVICE_FILE_EXT
#define EGL_DRM_DEVICE_FILE_EXT 0x3233
#endif

bool CEgl::selectDisplay(const std::string& gpuOverride) {
    using PFNEGLGETPLATFORMDISPLAYEXTPROC_t = EGLDisplay (*)(EGLenum, void*, const EGLint*);
    using PFNEGLQUERYDEVICESEXTPROC_t       = EGLBoolean (*)(EGLint, EGLDeviceEXT*, EGLint*);
    using PFNEGLQUERYDEVICESTRINGEXTPROC_t  = const char* (*)(EGLDeviceEXT, EGLint);
    auto eglGetPlatformDisplayEXT_fn        = (PFNEGLGETPLATFORMDISPLAYEXTPROC_t)eglGetProcAddress("eglGetPlatformDisplayEXT");
    auto eglQueryDevicesEXT_fn              = (PFNEGLQUERYDEVICESEXTPROC_t)eglGetProcAddress("eglQueryDevicesEXT");
    auto eglQueryDeviceStringEXT_fn         = (PFNEGLQUERYDEVICESTRINGEXTPROC_t)eglGetProcAddress("eglQueryDeviceStringEXT");

    if (!eglGetPlatformDisplayEXT_fn || !eglQueryDevicesEXT_fn || !eglQueryDeviceStringEXT_fn) {
        Log::log(Log::ERR, "[egl] EGL device/platform-display extensions unavailable");
        return false;
    }

    const bool  overridden = !gpuOverride.empty();
    std::string target     = gpuOverride;
    if (overridden)
        Log::log(Log::DEBUG, "[egl] target render node: {} (--gpu override)", target);
    else
        Log::log(Log::DEBUG, "[egl] no --gpu given; scanning for first working render node");

    EGLint numDevs = 0;
    eglQueryDevicesEXT_fn(0, nullptr, &numDevs);
    std::vector<EGLDeviceEXT> devs(numDevs);
    eglQueryDevicesEXT_fn(numDevs, devs.data(), &numDevs);
    Log::log(Log::DEBUG, "[egl] found {} EGL device(s)", numDevs);

    for (auto dev : devs) {
        // Prefer the render-node path, fall back to the primary/card path.
        const char* path = eglQueryDeviceStringEXT_fn(dev, EGL_DRM_RENDER_NODE_FILE_EXT);
        if (!path)
            path = eglQueryDeviceStringEXT_fn(dev, EGL_DRM_DEVICE_FILE_EXT);
        if (!path)
            continue;

        // With --gpu, accept only the matching device.
        if (overridden && target != path)
            continue;

        // Use EGL_PLATFORM_GBM_KHR (the normal Mesa rendering path).
        // EGL_PLATFORM_DEVICE_EXT is headless/compute-only and leaves gallium
        // pipe_context state partially uninitialised, which crashes driUnbindContext
        // (HypXRland doc 01). GBM avoids that.
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        struct gbm_device* gbm = gbm_create_device(fd);
        if (!gbm) {
            close(fd);
            continue;
        }
        EGLDisplay dpy = eglGetPlatformDisplayEXT_fn(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
        if (dpy == EGL_NO_DISPLAY) {
            gbm_device_destroy(gbm);
            close(fd);
            continue;
        }

        Log::log(Log::INFO, "[egl] using GBM EGL display on {}", path);
        m_display = dpy;
        m_gbm     = gbm;
        m_gbmFd   = fd;
        return true;
    }

    if (overridden)
        Log::log(Log::ERR, "[egl] --gpu '{}' matched no EGL device", gpuOverride);
    else
        Log::log(Log::ERR, "[egl] no usable EGL render node found");
    return false;
}

bool CEgl::init(const std::string& gpuOverride) {
    if (!selectDisplay(gpuOverride))
        return false;

    EGLint major = 0, minor = 0;
    if (!eglInitialize(m_display, &major, &minor)) {
        Log::log(Log::ERR, "[egl] eglInitialize failed (0x{:x})", (unsigned)eglGetError());
        return false;
    }
    Log::log(Log::DEBUG, "[egl] EGL {}.{} initialized", major, minor);

    eglBindAPI(EGL_OPENGL_ES_API);

    // Pick a GLES3 config. The default EGL_SURFACE_TYPE is EGL_WINDOW_BIT, which
    // can exclude the configs we need on some devices, so widen it. We render only
    // into FBOs bound to swapchain textures, so surface type barely matters.
    EGLConfig    cfg          = nullptr;
    EGLint       n            = 0;
    const EGLint attempts[][9] = {
        {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, (EGLint)(EGL_WINDOW_BIT | EGL_PBUFFER_BIT), EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_NONE},
        {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_NONE},
        {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, 0, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_NONE},
        {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE, 0, EGL_NONE, 0, EGL_NONE},
    };
    for (auto& attrs : attempts) {
        if (eglChooseConfig(m_display, attrs, &cfg, 1, &n) && n > 0)
            break;
        cfg = nullptr;
    }
    if (!cfg) {
        // Manual scan (works around Mesa eglChooseConfig quirks).
        EGLint numAll = 0;
        eglGetConfigs(m_display, nullptr, 0, &numAll);
        std::vector<EGLConfig> all(numAll);
        eglGetConfigs(m_display, all.data(), numAll, &numAll);
        for (auto& c : all) {
            EGLint rt = 0;
            eglGetConfigAttrib(m_display, c, EGL_RENDERABLE_TYPE, &rt);
            if (rt & EGL_OPENGL_ES3_BIT) {
                cfg = c;
                break;
            }
        }
    }
    if (!cfg) {
        Log::log(Log::ERR, "[egl] no suitable GLES3 EGL config (0x{:x})", (unsigned)eglGetError());
        return false;
    }
    m_config = cfg;

    const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    m_context               = eglCreateContext(m_display, m_config, EGL_NO_CONTEXT, ctxAttrs);
    if (m_context == EGL_NO_CONTEXT) {
        Log::log(Log::ERR, "[egl] eglCreateContext failed (0x{:x})", (unsigned)eglGetError());
        return false;
    }

    Log::log(Log::DEBUG, "[egl] GLES3 context created");
    return true;
}

void CEgl::makeCurrent() {
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_context);
}

void CEgl::release() {
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void CEgl::destroy() {
    if (m_display != EGL_NO_DISPLAY)
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (m_context != EGL_NO_CONTEXT) {
        eglDestroyContext(m_display, m_context);
        m_context = EGL_NO_CONTEXT;
    }
    if (m_display != EGL_NO_DISPLAY) {
        eglTerminate(m_display);
        m_display = EGL_NO_DISPLAY;
    }
    m_config = nullptr;
    // The display depends on the GBM device — destroy it last.
    if (m_gbm) {
        gbm_device_destroy(m_gbm);
        m_gbm = nullptr;
    }
    if (m_gbmFd >= 0) {
        close(m_gbmFd);
        m_gbmFd = -1;
    }
}
