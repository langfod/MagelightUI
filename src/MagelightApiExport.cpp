// The exported plugin API — serves api/MagelightUI_API.h's versioned structs
// to other SKSE plugins (SeverActions). Thin captureless-lambda wrappers
// adapt the C ABI (const char*) to the internal C++ surface; the structs are
// static, so the returned pointers stay valid for the process lifetime.

#include "Magelight.h"
#include "MagelightApi4.h"

#include "../api/MagelightUI_API.h"

namespace {

    using MAGELIGHT_API::ImageId;
    using MAGELIGHT_API::ViewId;

    // Shared implementations (both structs point at the same functions).
    ViewId ApiCreateView(const char* htmlPath, int x, int y, int w, int h,
                         MAGELIGHT_API::DomReadyFn onDomReady, bool clickThrough, bool startVisible)
    {
        return Magelight::CreateView(htmlPath, x, y, w, h, onDomReady, clickThrough, startVisible);
    }
    bool ApiIsViewValid(ViewId view) { return Magelight::IsViewValid(view); }
    void ApiShowView(ViewId view, bool show) { Magelight::ShowView(view, show); }
    void ApiSetViewBounds(ViewId view, int x, int y, int w, int h)
    {
        Magelight::SetViewBounds(view, x, y, w, h);
    }
    void ApiRegisterJSListener(ViewId view, const char* name, MAGELIGHT_API::JsListenerFn cb)
    {
        Magelight::RegisterJSListener(view, name, cb);
    }
    void ApiInteropCall(ViewId view, const char* fn, const char* arg)
    {
        Magelight::InteropCall(view, fn, std::string(arg ? arg : ""));
    }
    void ApiInvokeJS(ViewId view, const char* script)
    {
        if (script) Magelight::InvokeJS(view, std::string(script));
    }
    void ApiEnterUIMode(ViewId view) { Magelight::EnterUIMode(view); }
    void ApiEnterUIModeEx(ViewId view, bool pauseGame) { Magelight::EnterUIModeEx(view, pauseGame); }
    void ApiExitUIMode() { Magelight::ExitUIMode(); }
    bool ApiIsUIModeActive() { return Magelight::IsUIModeActive(); }
    void ApiSetUIModeExitCallback(MAGELIGHT_API::UIModeExitFn cb)
    {
        Magelight::SetUIModeExitCallback(cb);
    }

    bool ApiIsGpuAccelerated() { return Magelight::IsGpuAccelerated(); }
    ImageId ApiRegisterTextureImage(const char* name, ID3D11ShaderResourceView* srv,
                                    std::uint32_t w, std::uint32_t h)
    {
        return Magelight::RegisterTextureImage(name, srv, w, h);
    }
    void ApiUpdateTextureImage(ImageId image, ID3D11ShaderResourceView* srv)
    {
        Magelight::UpdateTextureImage(image, srv);
    }
    void ApiInvalidateImage(ImageId image) { Magelight::InvalidateImage(image); }
    void ApiUnregisterImage(ImageId image) { Magelight::UnregisterImage(image); }
    const char* ApiImageUrl(ImageId image) { return Magelight::ImageUrl(image); }

    const MAGELIGHT_API::MagelightApi1 s_api1{
        MAGELIGHT_API::kApiVersion1,
        PLUGIN_VERSION,
        &ApiCreateView, &ApiIsViewValid, &ApiShowView, &ApiSetViewBounds,
        &ApiRegisterJSListener, &ApiInteropCall, &ApiInvokeJS,
        &ApiEnterUIMode, &ApiExitUIMode, &ApiIsUIModeActive, &ApiSetUIModeExitCallback,
    };

    const MAGELIGHT_API::MagelightApi2 s_api2{
        MAGELIGHT_API::kApiVersion2,
        PLUGIN_VERSION,
        &ApiCreateView, &ApiIsViewValid, &ApiShowView, &ApiSetViewBounds,
        &ApiRegisterJSListener, &ApiInteropCall, &ApiInvokeJS,
        &ApiEnterUIMode, &ApiExitUIMode, &ApiIsUIModeActive, &ApiSetUIModeExitCallback,
        &ApiIsGpuAccelerated, &ApiRegisterTextureImage, &ApiUpdateTextureImage,
        &ApiInvalidateImage, &ApiUnregisterImage, &ApiImageUrl,
    };

    const MAGELIGHT_API::MagelightApi3 s_api3{
        MAGELIGHT_API::kApiVersion3,
        PLUGIN_VERSION,
        &ApiCreateView, &ApiIsViewValid, &ApiShowView, &ApiSetViewBounds,
        &ApiRegisterJSListener, &ApiInteropCall, &ApiInvokeJS,
        &ApiEnterUIMode, &ApiExitUIMode, &ApiIsUIModeActive, &ApiSetUIModeExitCallback,
        &ApiIsGpuAccelerated, &ApiRegisterTextureImage, &ApiUpdateTextureImage,
        &ApiInvalidateImage, &ApiUnregisterImage, &ApiImageUrl,
        &ApiEnterUIModeEx,
    };

    const MAGELIGHT_API::MagelightApi4 s_api4{
        MAGELIGHT_API::kApiVersion4,
        PLUGIN_VERSION,
        &ApiCreateView, &ApiIsViewValid, &ApiShowView, &ApiSetViewBounds,
        &ApiRegisterJSListener, &ApiInteropCall, &ApiInvokeJS,
        &ApiEnterUIMode, &ApiExitUIMode, &ApiIsUIModeActive, &ApiSetUIModeExitCallback,
        &ApiIsGpuAccelerated, &ApiRegisterTextureImage, &ApiUpdateTextureImage,
        &ApiInvalidateImage, &ApiUnregisterImage, &ApiImageUrl,
        &ApiEnterUIModeEx,
        &Magelight::Api4::RegisterMod, &Magelight::Api4::UnregisterMod,
        &Magelight::Api4::CreateViewEx, &Magelight::Api4::DestroyView,
        &Magelight::Api4::ReloadView, &Magelight::Api4::Navigate,
        &Magelight::Api4::RegisterJSListenerEx,
        &Magelight::Api4::RequestUIMode, &Magelight::Api4::ReleaseUIMode, &Magelight::Api4::GetUIModeOwner,
        &Magelight::Api4::RaiseView, &Magelight::Api4::GetViewInfo, &Magelight::Api4::GetDisplaySize,
        &Magelight::Api4::QueryCapability, &Magelight::Api4::GetLastErrorMessage,
        &Magelight::Api4::BindHotkey, &Magelight::Api4::RegisterTextureImageEx,
        MAGELIGHT_API::PackVersion(PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR, PLUGIN_VERSION_PATCH),
        // 0.14.0 appendix (after hostVersionNumber — the v4 struct only ever grows at the end)
        &Magelight::Api4::EvalJS,
        // 0.16.0 appendix
        &Magelight::Api4::SetViewCutout, &Magelight::Api4::SetViewHibernate,
        // 0.18.0 appendix (VR-4) — SAME ORDER as the struct's tail (invariant 10)
        &Magelight::Api4::SetViewVRPlacement, &Magelight::Api4::GetViewVRPlacement,
        &Magelight::Api4::RecenterVRView, &Magelight::Api4::BindVRHotkey,
        // 0.21.0 appendix — SAME ORDER as the struct's tail (invariant 10)
        &Magelight::Api4::BindVRHotkeyCallback,
        // 0.26.0 appendix
        &Magelight::Api4::SetVRButtonListener,
        // 0.26.8 appendix
        &Magelight::Api4::FindView,
        // 0.26.9 appendix
        &Magelight::Api4::SetViewScale,
        // 0.28.0 appendix
        &Magelight::Api4::SetEscapeCapture,
        &Magelight::Api4::ShowInspector,
        &Magelight::Api4::IsInspectorVisible,
        &Magelight::Api4::SetViewOrder,
        &Magelight::Api4::GetViewOrder,
        &Magelight::Api4::SetScrollStep,
        // 0.28.2 appendix
        &Magelight::Api4::SetNetworkPolicy,
        // 0.29.0 appendix — SAME ORDER as the struct's tail (invariant 10)
        &Magelight::Api4::PlayUISound,
        &Magelight::Api4::SetViewSounds,
        // 0.30.1 appendix
        &Magelight::Api4::PostGameTask,
        // 0.31.0 appendix
        &Magelight::Api4::SetViewFreezeWorld,
        &Magelight::Api4::SetViewLoadOnShow,
    };

}  // namespace

extern "C" __declspec(dllexport) void* Magelight_RequestApi(std::uint32_t version)
{
    switch (version) {
    case MAGELIGHT_API::kApiVersion4:
        return const_cast<MAGELIGHT_API::MagelightApi4*>(&s_api4);
    case MAGELIGHT_API::kApiVersion1:
        return const_cast<MAGELIGHT_API::MagelightApi1*>(&s_api1);
    case MAGELIGHT_API::kApiVersion2:
        return const_cast<MAGELIGHT_API::MagelightApi2*>(&s_api2);
    case MAGELIGHT_API::kApiVersion3:
        return const_cast<MAGELIGHT_API::MagelightApi3*>(&s_api3);
    default:
        return nullptr;
    }
}
