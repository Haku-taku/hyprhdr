#define WLR_USE_UNSTABLE

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/managers/screenshare/ScreenshareManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/output/MonitorResources.hpp>
#include <hyprland/src/protocols/ColorManagement.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Context.hpp>
#include <hyprland/src/helpers/Format.hpp>
#include <hyprland/src/helpers/CMType.hpp>
#include <hyprland/src/helpers/cm/ColorManagement.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

static HANDLE PHANDLE = nullptr;

static CFunctionHook* g_pCalculateConstraintsHook   = nullptr;
static CFunctionHook* g_pBeginCopyHook              = nullptr;
static CFunctionHook* g_pMirrorFBHook               = nullptr;
static CFunctionHook* g_pSaveBufferForMirrorHook    = nullptr;
static CFunctionHook* g_pGetHDRImageDescriptionHook = nullptr;
static CFunctionHook* g_pSessionStopHook            = nullptr;

// The sessions that were offered HDR, with the client the offer was made to. The format
// offered and the description the frame is written with must come from one decision.
static std::unordered_map<Screenshare::CScreenshareSession*, wl_client*> g_hdrSessions;

static CConfigValue<Config::BOOL> PSCREENCOPYHDR;
static CConfigValue<Config::BOOL> PDEBUG;
static CConfigValue<Config::BOOL> PFIXREFERENCE;

template <typename... Args>
static void dbg(const std::format_string<Args...>& format, Args&&... args) {
    if (!*PDEBUG)
        return;

    const auto MESSAGE = std::format(format, std::forward<Args>(args)...);
    std::fprintf(stderr, "[hyprhdr] %s\n", MESSAGE.c_str());
    std::fflush(stderr);
}

// Accessors for the private members the hooks need; the header is the single source of truth.
template <typename Tag, typename Tag::type M>
struct Rob {
    friend typename Tag::type get(Tag) {
        return M;
    }
};

struct SessionClient {
    using type = wl_client* Screenshare::CScreenshareSession::*;
    friend type get(SessionClient);
};
template struct Rob<SessionClient, &Screenshare::CScreenshareSession::m_client>;

struct SessionFormats {
    using type = std::vector<DRMFormat> Screenshare::CScreenshareSession::*;
    friend type get(SessionFormats);
};
template struct Rob<SessionFormats, &Screenshare::CScreenshareSession::m_formats>;

struct SessionType {
    using type = Screenshare::eScreenshareType Screenshare::CScreenshareSession::*;
    friend type get(SessionType);
};
template struct Rob<SessionType, &Screenshare::CScreenshareSession::m_type>;

struct FrameSession {
    using type = WP<Screenshare::CScreenshareSession> Screenshare::CScreenshareFrame::*;
    friend type get(FrameSession);
};
template struct Rob<FrameSession, &Screenshare::CScreenshareFrame::m_session>;

struct ResourcesMonitor {
    using type = WP<Monitor::CMonitor> Monitor::CMonitorResources::*;
    friend type get(ResourcesMonitor);
};
template struct Rob<ResourcesMonitor, &Monitor::CMonitorResources::m_monitor>;

static bool isHDRDescription(const NColorManagement::PImageDescription& description) {
    if (!description)
        return false;

    const auto TF = description->value().transferFunction;
    return TF == NColorManagement::CM_TRANSFER_FUNCTION_ST2084_PQ || TF == NColorManagement::CM_TRANSFER_FUNCTION_HLG;
}

static bool isHDRMonitor(const PHLMONITOR& monitor) {
    return monitor && isHDRDescription(monitor->m_imageDescription);
}

static bool isClientCMAware(wl_client* client) {
    return PROTO::colorManagement && client && PROTO::colorManagement->isClientCMAware(client);
}

// One decision, used both for the format offered and for the description the capture is written with.
static bool hdrCaptureAllowed(Screenshare::CScreenshareSession* session) {
    if (!*PSCREENCOPYHDR || !session)
        return false;

    if ((session->*get(SessionType{})) == Screenshare::SHARE_WORKSPACE)
        return false;

    if (!isHDRMonitor(session->monitor()))
        return false;

    return isClientCMAware(session->*get(SessionClient{}));
}

using FnCalculateConstraints   = void (*)(Screenshare::CScreenshareSession*);
using FnBeginCopy              = bool (*)(Screenshare::CScreenshareFrame*, SP<Render::IFramebuffer>);
using FnMirrorFB               = SP<Render::IFramebuffer> (*)(Monitor::CMonitorResources*);
using FnSaveBufferForMirror    = bool (*)(Render::GL::CHyprOpenGLImpl*, Render::CRenderContext&, const Hyprutils::Math::CBox&);
using FnGetHDRImageDescription = NColorManagement::PImageDescription (*)(Monitor::CMonitor*, NCMType::eCMType);

static void hkCalculateConstraints(Screenshare::CScreenshareSession* self) {
    reinterpret_cast<FnCalculateConstraints>(g_pCalculateConstraintsHook->m_original)(self);

    if (!self)
        return;

    const bool ALLOWED = hdrCaptureAllowed(self);
    if (ALLOWED)
        g_hdrSessions[self] = self->*get(SessionClient{});
    else
        g_hdrSessions.erase(self);

    const auto MONITOR = self->monitor();
    dbg("constraints: enabled={} hdr={} aware={} type={} offer_hdr={}", static_cast<bool>(*PSCREENCOPYHDR), isHDRMonitor(MONITOR),
        isClientCMAware(self->*get(SessionClient{})), static_cast<int>(self->*get(SessionType{})), ALLOWED);

    if (!ALLOWED)
        return;

    // A ten-bit buffer is the only signal wlr-screencopy has that the pixels are the
    // output's own encoding, so offer one; the same order the stock path uses.
    auto& formats = self->*get(SessionFormats{});
    formats.clear();
    formats.push_back(DRM_FORMAT_ARGB2101010);
    formats.push_back(DRM_FORMAT_XRGB2101010);
    for (auto& format : formats) {
        if (format == DRM_FORMAT_XRGB2101010 || format == DRM_FORMAT_ARGB2101010)
            format = DRM_FORMAT_XBGR2101010;
    }

    dbg("offering 10-bit HDR capture to a colour-management-aware client");
}

static bool sessionOfferedHDR(Screenshare::CScreenshareSession* session) {
    if (!session)
        return false;

    const auto IT = g_hdrSessions.find(session);
    return IT != g_hdrSessions.end() && IT->second == session->*get(SessionClient{});
}

static bool hkBeginCopy(Screenshare::CScreenshareFrame* self, SP<Render::IFramebuffer> framebuffer) {
    const bool RESULT = reinterpret_cast<FnBeginCopy>(g_pBeginCopyHook->m_original)(self, framebuffer);
    if (!RESULT)
        return RESULT;

    const auto SESSION = self->*get(FrameSession{});
    if (!SESSION || !sessionOfferedHDR(SESSION.get()))
        return RESULT;

    if ((SESSION.get()->*get(SessionType{})) == Screenshare::SHARE_WORKSPACE)
        return RESULT;

    const auto MONITOR = SESSION->monitor();
    if (!isHDRMonitor(MONITOR) || !MONITOR->m_imageDescription)
        return RESULT;

    auto& CTX = g_pHyprRenderer->context();
    if (!CTX.m_data.outFB)
        return RESULT;

    CTX.m_data.outFB->setImageDescription(MONITOR->m_imageDescription);
    dbg("capture on {} described as the output's own HDR space", MONITOR->m_name);

    return RESULT;
}

static SP<Render::IFramebuffer> hkMirrorFB(Monitor::CMonitorResources* self) {
    auto framebuffer = reinterpret_cast<FnMirrorFB>(g_pMirrorFBHook->m_original)(self);
    if (!framebuffer)
        return framebuffer;

    const auto MONITOR = (self->*get(ResourcesMonitor{})).get();
    if (!MONITOR || !MONITOR->m_imageDescription)
        return framebuffer;

    const bool      HDRCAP  = (*PSCREENCOPYHDR || !g_hdrSessions.empty()) && isHDRDescription(MONITOR->m_imageDescription);
    const bool      TENBIT  = HDRCAP || MONITOR->m_activeMonitorRule.m_enable10bit;
    const DRMFormat WANTFMT = TENBIT ? DRM_FORMAT_XRGB2101010 : DRM_FORMAT_XRGB8888;

    bool            changed = false;

    if (framebuffer->m_drmFormat != WANTFMT) {
        if (!framebuffer->alloc(static_cast<int>(MONITOR->m_transformedSize.x), static_cast<int>(MONITOR->m_transformedSize.y), WANTFMT)) {
            LOG(Log::ERR, "[hyprhdr] failed to allocate the mirror for {} as {}", MONITOR->m_name, TENBIT ? "10-bit" : "8-bit");
            return framebuffer;
        }
        changed = true;
    }

    if (HDRCAP) {
        if (!framebuffer->imageDescription() || framebuffer->imageDescription()->id() != MONITOR->m_imageDescription->id()) {
            framebuffer->setImageDescription(MONITOR->m_imageDescription);
            changed = true;
        }
    } else if (isHDRDescription(MONITOR->m_imageDescription) && framebuffer->imageDescription() &&
               framebuffer->imageDescription()->id() == MONITOR->m_imageDescription->id()) {
        // The plugin's HDR mirror was turned off at runtime; go back to the sRGB copy.
        framebuffer->setImageDescription(NColorManagement::DEFAULT_SRGB_IMAGE_DESCRIPTION);
        changed = true;
    }

    if (changed) {
        self->invalidateMirrorFB();
        dbg("mirror for {} rebuilt: ten-bit={} hdr={}", MONITOR->m_name, TENBIT, HDRCAP);
    }

    return framebuffer;
}

static bool hkSaveBufferForMirror(Render::GL::CHyprOpenGLImpl* self, Render::CRenderContext& ctx, const Hyprutils::Math::CBox& box) {
    const auto MONITOR = ctx.m_data.pMonitor.get();
    if (!MONITOR || (!*PSCREENCOPYHDR && g_hdrSessions.empty()) || !isHDRDescription(MONITOR->m_imageDescription))
        return reinterpret_cast<FnSaveBufferForMirror>(g_pSaveBufferForMirrorHook->m_original)(self, ctx, box);

    const auto RESOURCES = MONITOR->resources().get();
    if (!RESOURCES)
        return reinterpret_cast<FnSaveBufferForMirror>(g_pSaveBufferForMirrorHook->m_original)(self, ctx, box);

    // The MRT "unmodified copy" maps everything into sRGB over the SDR range. Mirror the
    // linear work buffer instead, and keep the shader CM on even while a fullscreen CTM
    // has put the monitor into its no-shader mode; the mirror is a framebuffer of ours.
    // resources() is a weak ref over a unique pointer, so keep raw pointers here.
    struct SRestore {
        Monitor::CMonitor*          monitor    = nullptr;
        Monitor::CMonitorResources* resources  = nullptr;
        SP<Render::ITexture>        mirrorTex;
        bool                        noShaderCM = false;

        ~SRestore() {
            if (resources)
                resources->m_mirrorTex = mirrorTex;
            if (monitor)
                monitor->m_noShaderCTM = noShaderCM;
        }
    } restore{
        .monitor    = MONITOR,
        .resources  = RESOURCES,
        .mirrorTex  = RESOURCES->m_mirrorTex,
        .noShaderCM = MONITOR->m_noShaderCTM,
    };

    RESOURCES->m_mirrorTex = nullptr;
    MONITOR->m_noShaderCTM = false;

    return reinterpret_cast<FnSaveBufferForMirror>(g_pSaveBufferForMirrorHook->m_original)(self, ctx, box);
}

static NColorManagement::PImageDescription hkGetHDRImageDescription(Monitor::CMonitor* self, NCMType::eCMType cmType) {
    const auto DESCRIPTION = reinterpret_cast<FnGetHDRImageDescription>(g_pGetHDRImageDescriptionHook->m_original)(self, cmType);
    if (!DESCRIPTION || !*PFIXREFERENCE)
        return DESCRIPTION;

    // A client divides by the reference to turn its 1.0 into light, so publish the level
    // SDR white is actually rendered at on this output, not the built-in HDR constant.
    const int   SDRMAX = self->m_activeMonitorRule.m_sdrMaxLuminance > 0 ? self->m_activeMonitorRule.m_sdrMaxLuminance : static_cast<int>(SDR_MAX_LUMINANCE);
    const float BRIGHT = self->m_activeMonitorRule.m_sdrBrightness > 0 ? self->m_activeMonitorRule.m_sdrBrightness : 1.F;
    const auto  REF    = static_cast<uint32_t>(std::lround(static_cast<float>(SDRMAX) * BRIGHT));

    auto VALUE = DESCRIPTION->value();
    if (VALUE.luminances.reference == REF)
        return DESCRIPTION;

    VALUE.luminances.reference = REF;
    const auto FIXED           = NColorManagement::CImageDescription::from(VALUE);

    dbg("output {} publishes {} cd/m² as the HDR reference", self->m_name, REF);

    return FIXED;
}

using FnSessionStop = void (*)(Screenshare::CScreenshareSession*);

static void hkSessionStop(Screenshare::CScreenshareSession* self) {
    g_hdrSessions.erase(self);
    reinterpret_cast<FnSessionStop>(g_pSessionStopHook->m_original)(self);
}

static CFunctionHook* makeHook(const std::string& name, std::initializer_list<std::string_view> needles, void* destination) {
    void* address = nullptr;

    for (const auto& function : HyprlandAPI::findFunctionsByName(PHANDLE, name)) {
        for (const auto& needle : needles) {
            if (function.demangled.find(needle) != std::string::npos) {
                address = function.address;
                break;
            }
        }
        if (address)
            break;
    }

    if (!address)
        throw std::runtime_error("[hyprhdr] no function matched " + name);

    auto* hook = HyprlandAPI::createFunctionHook(PHANDLE, address, destination);
    if (!hook || !hook->hook())
        throw std::runtime_error("[hyprhdr] failed to hook " + name);

    return hook;
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string SERVER_HASH = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();
    if (SERVER_HASH != CLIENT_HASH)
        throw std::runtime_error("[hyprhdr] version mismatch (headers != running Hyprland)");

    using namespace Config::Values;

    if (!HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CBoolValue>("plugin:hyprhdr:screencopy_hdr",
                                                                       "hand HDR pixels to colour-management-aware screencopy clients", false)) ||
        !HyprlandAPI::addConfigValueV2(PHANDLE,
                                       makeShared<CBoolValue>("plugin:hyprhdr:debug", "log the HDR capture decisions", false)) ||
        !HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CBoolValue>("plugin:hyprhdr:fix_hdr_reference",
                                                                       "publish the real SDR white as the HDR description's reference luminance", true)))
        throw std::runtime_error("[hyprhdr] failed to register config values");

    PSCREENCOPYHDR.bind("plugin:hyprhdr:screencopy_hdr");
    PDEBUG.bind("plugin:hyprhdr:debug");
    PFIXREFERENCE.bind("plugin:hyprhdr:fix_hdr_reference");

    g_pCalculateConstraintsHook = makeHook("calculateConstraints", {"CScreenshareSession::calculateConstraints("}, (void*)&hkCalculateConstraints);
    g_pBeginCopyHook            = makeHook("beginCopy", {"CScreenshareFrame::beginCopy("}, (void*)&hkBeginCopy);
    g_pMirrorFBHook             = makeHook("mirrorFB", {"CMonitorResources::mirrorFB()"}, (void*)&hkMirrorFB);
    g_pSaveBufferForMirrorHook  = makeHook("saveBufferForMirror", {"CHyprOpenGLImpl::saveBufferForMirror("}, (void*)&hkSaveBufferForMirror);
    g_pGetHDRImageDescriptionHook =
        makeHook("getHDRImageDescription", {"CMonitor::getHDRImageDescription("}, (void*)&hkGetHDRImageDescription);
    g_pSessionStopHook = makeHook("stop", {"CScreenshareSession::stop()"}, (void*)&hkSessionStop);

    return PLUGIN_DESCRIPTION_INFO{"hyprhdr", "HDR pixels for colour-management-aware screencopy clients", "Haku-taku", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    for (auto** hook : {&g_pCalculateConstraintsHook, &g_pBeginCopyHook, &g_pMirrorFBHook, &g_pSaveBufferForMirrorHook,
                        &g_pGetHDRImageDescriptionHook, &g_pSessionStopHook}) {
        if (!*hook)
            continue;

        HyprlandAPI::removeFunctionHook(PHANDLE, *hook);
        *hook = nullptr;
    }
}
