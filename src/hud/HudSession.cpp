#include "HudSession.hpp"

#include "Egl.hpp"
#include "HudMessage.hpp"
#include "HudModel.hpp"
#include "HudText.hpp"
#include "Log.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern std::atomic<bool> g_stopRequested;

#define XR_CHK(expr)                                                                   \
    do {                                                                               \
        XrResult _r = (expr);                                                          \
        if (XR_FAILED(_r)) {                                                           \
            Log::log(Log::ERR, "[hud] " #expr " failed: {}", (int)_r);                \
            return false;                                                              \
        }                                                                              \
    } while (0)

namespace {
    int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    constexpr int64_t kSRGBA = 0x8C43; // GL_SRGB8_ALPHA8
    constexpr int64_t kRGBA8 = 0x8058; // GL_RGBA8
}

CHudSession::~CHudSession() {
    destroy();
}

bool CHudSession::createInstance() {
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> extProps(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    if (extCount)
        xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, extProps.data());
    auto hasExt = [&](const char* n) {
        for (auto& e : extProps)
            if (std::strcmp(e.extensionName, n) == 0)
                return true;
        return false;
    };

    const char* required[] = {"XR_MNDX_egl_enable", "XR_KHR_opengl_es_enable", XR_EXTX_OVERLAY_EXTENSION_NAME};
    std::vector<const char*> exts;
    for (auto* r : required) {
        if (!hasExt(r)) {
            Log::log(Log::ERR, "[hud] required extension '{}' unavailable", r);
            return false;
        }
        exts.push_back(r);
    }
    m_haveColorScale = hasExt(XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME);
    if (m_haveColorScale)
        exts.push_back(XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME);

    XrApplicationInfo appInfo = {};
    std::strncpy(appInfo.applicationName, "hypxrvoice-hud", XR_MAX_APPLICATION_NAME_SIZE - 1);
    appInfo.applicationVersion = 1;
    std::strncpy(appInfo.engineName, "hypxrvoice", XR_MAX_ENGINE_NAME_SIZE - 1);
    appInfo.engineVersion = 1;
    appInfo.apiVersion    = XR_API_VERSION_1_0;

    XrInstanceCreateInfo info  = {XR_TYPE_INSTANCE_CREATE_INFO};
    info.applicationInfo       = appInfo;
    info.enabledExtensionCount = (uint32_t)exts.size();
    info.enabledExtensionNames = exts.data();
    XR_CHK(xrCreateInstance(&info, &m_instance));

    XrInstanceProperties props = {XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(m_instance, &props)))
        m_runtimeName = props.runtimeName;
    Log::log(Log::INFO, "[hud] instance created (runtime: {}, color_scale_bias: {})",
             m_runtimeName.empty() ? "?" : m_runtimeName, m_haveColorScale);
    return true;
}

bool CHudSession::getSystem() {
    XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor      = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHK(xrGetSystem(m_instance, &si, &m_systemId));
    return true;
}

bool CHudSession::createSession(CEgl& egl) {
    PFN_xrGetOpenGLESGraphicsRequirementsKHR pfn = nullptr;
    XR_CHK(xrGetInstanceProcAddr(m_instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                                 reinterpret_cast<PFN_xrVoidFunction*>(&pfn)));
    XrGraphicsRequirementsOpenGLESKHR reqs = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    XR_CHK(pfn(m_instance, m_systemId, &reqs));

    XrGraphicsBindingEGLMNDX binding = {XR_TYPE_GRAPHICS_BINDING_EGL_MNDX};
    binding.getProcAddress           = (PFNEGLGETPROCADDRESSPROC)eglGetProcAddress;
    binding.display                  = egl.m_display;
    binding.config                   = egl.m_config;
    binding.context                  = egl.m_context;

    // Chain the overlay create-info between the session info and the EGL binding so our
    // quad composites above HypXRland's monitors (higher sessionLayersPlacement = top).
    XrSessionCreateInfoOverlayEXTX overlay = {XR_TYPE_SESSION_CREATE_INFO_OVERLAY_EXTX};
    overlay.createFlags            = 0;
    overlay.sessionLayersPlacement = (uint32_t)m_p.overlayZ;
    overlay.next                   = &binding;

    XrSessionCreateInfo si = {XR_TYPE_SESSION_CREATE_INFO};
    si.systemId            = m_systemId;
    si.next                = &overlay;

    XR_CHK(xrCreateSession(m_instance, &si, &m_session));
    Log::log(Log::INFO, "[hud] overlay session created (placement {})", m_p.overlayZ);
    return true;
}

bool CHudSession::createSpace() {
    XrReferenceSpaceCreateInfo info = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    info.poseInReferenceSpace       = {{0, 0, 0, 1}, {0, 0, 0}};
    info.referenceSpaceType         = XR_REFERENCE_SPACE_TYPE_VIEW; // head-locked (native, free).
    XR_CHK(xrCreateReferenceSpace(m_session, &info, &m_viewSpace));
    return true;
}

bool CHudSession::chooseSwapchainFormat() {
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(m_session, 0, &n, nullptr);
    std::vector<int64_t> formats(n);
    if (n)
        xrEnumerateSwapchainFormats(m_session, n, &n, formats.data());
    m_swapchainFormat = kSRGBA;
    if (!formats.empty()) {
        int64_t chosen = formats[0];
        for (auto f : formats)
            if (f == kSRGBA) { chosen = f; break; }
        if (chosen != kSRGBA)
            for (auto f : formats)
                if (f == kRGBA8) { chosen = f; break; }
        m_swapchainFormat = chosen;
    }
    return true;
}

bool CHudSession::createSwapchain() {
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags            = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format                = m_swapchainFormat;
    ci.sampleCount           = 1;
    ci.width                 = (uint32_t)m_p.texW;
    ci.height                = (uint32_t)m_p.texH;
    ci.faceCount             = 1;
    ci.arraySize             = 1;
    ci.mipCount              = 1;
    XR_CHK(xrCreateSwapchain(m_session, &ci, &m_swapchain));
    return true;
}

void CHudSession::uploadImage(const uint8_t* rgba) {
    uint32_t                    idx     = 0;
    XrSwapchainImageAcquireInfo acq     = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(m_swapchain, &acq, &idx)))
        return;
    XrSwapchainImageWaitInfo wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout                  = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(m_swapchain, &wait)))
        return;

    // Re-enumerate images each upload is wasteful; do it once lazily.
    static std::vector<uint32_t> texes;
    if (texes.empty()) {
        uint32_t c = 0;
        xrEnumerateSwapchainImages(m_swapchain, 0, &c, nullptr);
        std::vector<XrSwapchainImageOpenGLESKHR> imgs(c, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        xrEnumerateSwapchainImages(m_swapchain, c, &c,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
        for (auto& im : imgs)
            texes.push_back(im.image);
    }
    if (idx < texes.size()) {
        // GL texture origin is bottom-left; our raster is top-row-first, so flip rows.
        std::vector<uint8_t> flip((size_t)m_p.texW * m_p.texH * 4);
        const size_t         row = (size_t)m_p.texW * 4;
        for (int y = 0; y < m_p.texH; y++)
            std::memcpy(&flip[(size_t)(m_p.texH - 1 - y) * row], &rgba[(size_t)y * row], row);

        glBindTexture(GL_TEXTURE_2D, texes[idx]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_p.texW, m_p.texH, GL_RGBA, GL_UNSIGNED_BYTE, flip.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFinish();
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    XrSwapchainImageReleaseInfo rel = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(m_swapchain, &rel);
}

bool CHudSession::readStdin() {
    char    buf[4096];
    ssize_t r;
    while ((r = read(STDIN_FILENO, buf, sizeof(buf))) > 0)
        m_stdinBuf.append(buf, (size_t)r);
    if (r == 0) {
        m_stdinEof = true;
        return false; // EOF: daemon closed the pipe.
    }
    // r < 0: EAGAIN (nonblocking, nothing more) is normal.

    // Process complete lines; the LAST view wins for this cycle.
    size_t pos;
    bool   updated = false;
    SHudView latest;
    while ((pos = m_stdinBuf.find('\n')) != std::string::npos) {
        std::string line = m_stdinBuf.substr(0, pos);
        m_stdinBuf.erase(0, pos + 1);
        SHudView v;
        if (HudMsg::parse(line, v)) {
            latest  = v;
            updated = true;
        }
    }
    if (!updated)
        return true;

    if (latest.state == EHudState::Hidden) {
        m_hasContent = false;
        return true;
    }
    // Rasterise + upload the new content once; snapshot its fade envelope + clock.
    SHudImage img = renderHud(latest, m_p.texW, m_p.texH);
    if (!img.empty())
        uploadImage(img.rgba.data());
    m_hasContent = true;
    m_shownAtMs  = nowMs();
    m_riseMs     = latest.riseMs;
    m_holdMs     = latest.holdMs;
    m_fadeMs     = latest.fadeMs;
    m_ceil       = std::min(latest.opacityCeil, m_p.opacity);
    return true;
}

void CHudSession::pollEvents() {
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    XrResult          r;
    while (m_instance != XR_NULL_HANDLE && (r = xrPollEvent(m_instance, &ev)) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            m_state = e->state;
            switch (m_state) {
                case XR_SESSION_STATE_READY: {
                    XrSessionBeginInfo bi           = {XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (XR_SUCCEEDED(xrBeginSession(m_session, &bi))) {
                        m_sessionRunning = true;
                        Log::log(Log::INFO, "[hud] session begun");
                    }
                    break;
                }
                case XR_SESSION_STATE_STOPPING:
                    xrEndSession(m_session);
                    m_sessionRunning = false;
                    break;
                case XR_SESSION_STATE_EXITING:
                case XR_SESSION_STATE_LOSS_PENDING:
                    m_exit = true;
                    break;
                default: break;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            m_exit = true;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    if (r == XR_ERROR_INSTANCE_LOST || r == XR_ERROR_SESSION_LOST)
        m_exit = true;
}

bool CHudSession::renderFrame() {
    XrFrameWaitInfo wi = {XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState    fs = {XR_TYPE_FRAME_STATE};
    XrResult        rw = xrWaitFrame(m_session, &wi, &fs);
    if (rw == XR_ERROR_SESSION_LOST || rw == XR_ERROR_INSTANCE_LOST) { m_exit = true; return false; }
    if (XR_FAILED(rw)) return false;

    XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
    XrResult         rb = xrBeginFrame(m_session, &bi);
    if (rb == XR_ERROR_SESSION_LOST || rb == XR_ERROR_INSTANCE_LOST) { m_exit = true; return false; }

    // Compute the current fade factor from the envelope.
    float alpha = 0.f;
    if (m_hasContent && fs.shouldRender) {
        int64_t el = nowMs() - m_shownAtMs;
        if (m_riseMs > 0 && el < m_riseMs)
            alpha = m_ceil * ((float)el / m_riseMs);
        else {
            int64_t ar = el - (m_riseMs > 0 ? m_riseMs : 0);
            if (m_holdMs < 0 || ar < m_holdMs)
                alpha = m_ceil;
            else {
                int64_t into = ar - m_holdMs;
                alpha = (m_fadeMs > 0 && into < m_fadeMs) ? m_ceil * (1.f - (float)into / m_fadeMs) : 0.f;
            }
        }
        if (alpha <= 0.001f)
            m_hasContent = false; // fully faded — stop submitting.
    }

    XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerColorScaleBiasKHR csb = {XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    const bool submit = m_hasContent && alpha > 0.001f && fs.shouldRender;
    if (submit) {
        // Our raster is PREMULTIPLIED alpha, so we do NOT set UNPREMULTIPLIED_ALPHA_BIT.
        quad.layerFlags        = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        quad.space             = m_viewSpace;
        quad.eyeVisibility     = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = m_swapchain;
        quad.subImage.imageRect = {{0, 0}, {m_p.texW, m_p.texH}};
        quad.subImage.imageArrayIndex = 0;
        quad.pose.orientation  = {0, 0, 0, 1};
        quad.pose.position     = {m_p.posX, m_p.posY, m_p.posZ};
        float h                = m_p.sizeW * (float)m_p.texH / (float)m_p.texW;
        quad.size              = {m_p.sizeW, h};

        if (m_haveColorScale) {
            // Premultiplied content: scale ALL channels by alpha to fade correctly.
            csb.colorScale = {alpha, alpha, alpha, alpha};
            csb.colorBias  = {0, 0, 0, 0};
            csb.next       = nullptr;
            quad.next      = &csb;
        }
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
    }

    XrFrameEndInfo ei        = {XR_TYPE_FRAME_END_INFO};
    ei.displayTime           = fs.predictedDisplayTime;
    ei.environmentBlendMode  = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount            = (uint32_t)layers.size();
    ei.layers                = layers.data();
    XrResult re              = xrEndFrame(m_session, &ei);
    if (re == XR_ERROR_SESSION_LOST || re == XR_ERROR_INSTANCE_LOST) { m_exit = true; return false; }
    return true;
}

bool CHudSession::init(CEgl& egl, const SParams& p) {
    m_egl = &egl;
    m_p   = p;
    m_ceil = p.opacity;

    // Non-blocking stdin so the frame loop is never stalled waiting for content.
    int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (fl >= 0)
        fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);

    if (!createInstance())
        return false;
    if (!getSystem())
        return false;
    egl.makeCurrent(); // held current for the whole session (fence contract).
    if (!createSession(egl))
        return false;
    if (!createSpace())
        return false;
    if (!chooseSwapchainFormat())
        return false;
    if (!createSwapchain())
        return false;
    return true;
}

void CHudSession::run() {
    Log::log(Log::INFO, "[hud] entering frame loop");
    while (!m_exit) {
        pollEvents();
        if (m_exit)
            break;

        if (g_stopRequested.load() && !m_exitRequested) {
            if (m_session != XR_NULL_HANDLE && m_sessionRunning)
                xrRequestExitSession(m_session);
            else
                m_exit = true;
            m_exitRequested = true;
        }

        // Drain any pending content; EOF on stdin => the daemon is gone, exit cleanly.
        if (!readStdin() && m_stdinEof) {
            if (m_session != XR_NULL_HANDLE && m_sessionRunning && !m_exitRequested) {
                xrRequestExitSession(m_session);
                m_exitRequested = true;
            } else if (!m_sessionRunning) {
                m_exit = true;
            }
        }

        if (!m_sessionRunning) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        renderFrame(); // xrWaitFrame paces us.
    }
    Log::log(Log::INFO, "[hud] frame loop exited");
}

void CHudSession::destroy() {
    if (m_swapchain != XR_NULL_HANDLE) { xrDestroySwapchain(m_swapchain); m_swapchain = XR_NULL_HANDLE; }
    if (m_viewSpace != XR_NULL_HANDLE) { xrDestroySpace(m_viewSpace); m_viewSpace = XR_NULL_HANDLE; }
    if (m_session != XR_NULL_HANDLE) { xrDestroySession(m_session); m_session = XR_NULL_HANDLE; }
    if (m_egl)
        m_egl->release();
    if (m_instance != XR_NULL_HANDLE) { xrDestroyInstance(m_instance); m_instance = XR_NULL_HANDLE; }
}
