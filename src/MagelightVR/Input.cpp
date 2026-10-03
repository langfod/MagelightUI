// VR presenter — engine input edges.
//
// The game thread's sink feeds controller button and thumbstick events here;
// the present thread consumes them (see the laser in Overlays.cpp). This is the
// one file in the module that owns its state outright: HandInput is written
// here and read elsewhere, so nothing but this file may interpret a device
// index or latch a rising edge.
//
// Everything is atomic because the two sides are different threads. The two
// plain bools (gripWasDown / menuWasDown) are game-thread only — they are the
// rising-edge latches, and nothing else reads them.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "MagelightVR.h"
#include "State.h"

#include <algorithm>
#include <cmath>

#include <windows.h>

namespace Magelight::VR {

    HandInput s_hands[2];   // 0 = left, 1 = right (physical), resolved via IsLeftHandedMode at use

    namespace {
        // Physical hand from the engine's device index. The engine reports
        // primary/secondary, which is right/left on every controller Skyrim VR
        // has shipped; the runtime's own left-handed mode is resolved at use.
        int HandIndexForDevice(int device)
        {
            using D = RE::INPUT_DEVICE;
            switch (static_cast<D>(device)) {
            case D::kVivePrimary: case D::kOculusPrimary: case D::kWMRPrimary: return 1;     // primary = right (physical)
            case D::kViveSecondary: case D::kOculusSecondary: case D::kWMRSecondary: return 0;
            default: return -1;
            }
        }
    }  // namespace

    void NoteButton(int device, std::uint32_t code, float value)
    {
        const int h = HandIndexForDevice(device);
        if (h < 0) return;
        using K = RE::BSOpenVRControllerDevice::Keys;
        HandInput& hi = s_hands[h];
        constexpr float kPressLevel = 0.5f;   // analog axes: half travel = pressed
        const bool down = value > kPressLevel;
        if (code == K::kTrigger) {
            hi.triggerDown.store(down);        // LEVEL; the laser derives the click edges
        } else if (code == K::kGrip || code == K::kGripAlt) {
            if (down && !hi.gripWasDown) hi.gripEdge.store(true);   // rising edge only
            hi.gripWasDown = down;
        } else if (code == K::kBY) {
            if (down && !hi.menuWasDown) hi.menuEdge.store(true);
            hi.menuWasDown = down;
        }
    }

    void NoteThumbstick(int device, float /*x*/, float y)
    {
        const int h = HandIndexForDevice(device);
        if (h < 0) return;
        s_hands[h].stickY_mil.store(static_cast<int>(std::lround(std::clamp(y, -1.0f, 1.0f) * 1000.0f)));
        s_hands[h].stickTick.store(GetTickCount64());
    }

}  // namespace Magelight::VR