// v4 API — 0.18.0 (VR-4): per-view VR placement + controller bindings.
//
// One file per ABI release. The block marker this file replaces is the durable
// record of which release introduced what, so a reader asking "what did 0.18.0
// add" reads this file and nothing else (CLAUDE.md invariant 10: the public ABI
// is append-only — these entry points stay exactly as they are).
//
// All of it is a thin translation onto Magelight::VR::*, which owns the overlay
// records and the pose snapshot. Nothing here holds registry state.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>

#include "Magelight.h"
#include "MagelightApi4.h"
#include "MagelightApi4/Registry.h"
#include "MagelightVR.h"


namespace Magelight::Api4 {

    using namespace MAGELIGHT_API;

    Result SetViewVRPlacement(ViewId view, const MAGELIGHT_API::VRPlacementDesc* desc)
    {
        if (!Magelight::IsViewValid(view)) return Result::InvalidView;
        if (!desc) {   // documented "restore the layer default" — do exactly that
            Magelight::VR::ClearPlacement(view);
            return Result::Ok;
        }
        if (desc->size < sizeof(MAGELIGHT_API::VRPlacementDesc)) return Result::InvalidArgument;
        Magelight::VR::Placement p;
        p.mode = static_cast<Magelight::VR::Mode>(desc->mode);
        if (desc->distanceMeters > 0.0f) p.distanceMeters = std::clamp(desc->distanceMeters, 0.2f, 20.0f);
        if (desc->widthMeters > 0.0f)    p.widthMeters    = std::clamp(desc->widthMeters, 0.05f, 20.0f);
        p.heightOffset = std::clamp(desc->heightOffset, -5.0f, 5.0f);
        Magelight::VR::SetPlacement(view, p);
        return Result::Ok;
    }

    Result GetViewVRPlacement(ViewId view, MAGELIGHT_API::VRPlacementDesc* out)
    {
        if (!out || out->size < sizeof(MAGELIGHT_API::VRPlacementDesc)) return Result::InvalidArgument;
        if (!Magelight::IsViewValid(view)) return Result::InvalidView;
        Magelight::VR::Placement p;
        if (!Magelight::VR::GetPlacement(view, p)) return Result::NotReady;   // no overlay yet
        out->mode = static_cast<std::uint8_t>(p.mode);
        out->distanceMeters = p.distanceMeters;
        out->widthMeters = p.widthMeters;
        out->heightOffset = p.heightOffset;
        out->curvature = p.curvature;
        out->autoCloseMeters = p.autoCloseMeters;
        return Result::Ok;
    }

    Result RecenterVRView(ViewId view)
    {
        if (!Magelight::IsViewValid(view)) return Result::InvalidView;
        Magelight::VR::Recenter(view);
        return Result::Ok;
    }

    Result BindVRHotkey(ViewId view, const MAGELIGHT_API::VRHotkeyDesc* desc)
    {
        if (!Magelight::IsViewValid(view)) return Result::InvalidView;
        if (!desc) { Magelight::VR::BindHotkey(view, 0, 0, 0, 0); return Result::Ok; }
        if (desc->size < sizeof(MAGELIGHT_API::VRHotkeyDesc)) return Result::InvalidArgument;
        if (desc->hand > 2) return Result::InvalidArgument;
        Magelight::VR::BindHotkey(view, desc->button, desc->modifier, desc->hand, desc->action);
        return Result::Ok;
    }

    Result BindVRHotkeyCallback(ViewId view, const MAGELIGHT_API::VRHotkeyDesc* desc,
                                MAGELIGHT_API::VRHotkeyFn cb, void* user)
    {
        if (!Magelight::IsViewValid(view)) return Result::InvalidView;
        if (!desc || !cb) { Magelight::VR::BindHotkeyCallback(view, 0, 0, 0, nullptr, nullptr); return Result::Ok; }
        if (desc->size < sizeof(MAGELIGHT_API::VRHotkeyDesc)) return Result::InvalidArgument;
        if (desc->hand > 2) return Result::InvalidArgument;
        Magelight::VR::BindHotkeyCallback(view, desc->button, desc->modifier, desc->hand,
                                          reinterpret_cast<Magelight::VR::HotkeyFn>(cb), user);
        return Result::Ok;
    }

    Result SetVRButtonListener(MAGELIGHT_API::VRButtonEdgeFn cb, void* user)
    {
        // No view: this is process-wide capture mode, not a per-view binding.
        Magelight::VR::SetButtonListener(reinterpret_cast<Magelight::VR::ButtonEdgeFn>(cb), user);
        return Result::Ok;
    }

    // The hotkey override is 0.18.0-adjacent rather than VR-specific (it is
    // read by BindHotkey in Api.cpp), so it sits with the VR block it shipped
    // alongside in the export table's ordering.
    void SetHotkeyOverride(const char* modSlashView, std::uint32_t dxScancode)
    {
        if (!modSlashView || !*modSlashView) return;
        std::lock_guard<std::mutex> lk(s_mutex);
        s_hotkeyOverrides[Lower(modSlashView)] = dxScancode;
    }

}  // namespace Magelight::Api4