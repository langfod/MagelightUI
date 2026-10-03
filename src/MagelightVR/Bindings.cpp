// VR presenter — controller hotkey bindings, chords, and press-to-bind capture.
//
// Self-contained: its own s_bindMutex, its own state, and no reach into the
// overlay records or the pose snapshot. The only outward calls are into the
// runtime (s_system) and Api4::DispatchViewAction.
//
// The epoch is the subtle part and it is load-bearing: bumped under
// s_bindMutex on ANY bind change, it makes the dispatch loop re-latch EVERY
// view rather than the one that changed, so a chord still physically held when
// capture ends can never fire a bystander view's action.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "MagelightVR.h"
#include "MagelightApi4.h"          // DispatchViewAction (game-thread gated action)
#include "MagelightVR/State.h"

#include <cstddef>

#include <windows.h>

namespace Magelight::VR {

    std::mutex s_bindMutex;
    std::map<ViewId, VRBinding> s_bindings;
    std::uint32_t               s_bindEpoch = 0;
    bool                        s_edgeSeedPending = false;
    std::map<ViewId, bool>      s_bindWas;
    std::map<ViewId, std::uint32_t> s_bindWasEpoch;
    std::map<ViewId, std::uint64_t> s_bindLastTick;
    std::uint64_t               s_captureLastTick = 0;

    ButtonEdgeFn  s_edgeCb   = nullptr;
    void*         s_edgeUser = nullptr;
    std::uint32_t s_edgeWasMask[2]   = { 0, 0 };
    std::uint32_t s_edgePending[2]   = { 0, 0 };

    namespace {
        // A modifier requirement, or an analog axis standing in for one.
        bool ButtonHeld(const vr::VRControllerState_t& st, std::uint32_t button)
        {
            if (button == 0) return true;   // no modifier required
            if ((st.ulButtonPressed & vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(button))) != 0) return true;
            // Analog fallbacks: trigger and grip report through their axes on
            // Touch/Index even when the digital bit never sets.
            if (button == 33 && st.rAxis[1].x > 0.5f) return true;
            if (button == 2  && st.rAxis[2].x > 0.5f) return true;
            return false;
        }
    }  // namespace

    // Polled every frame, UI mode on or off — that is how a view opens.
    void TickVRHotkeys()
    {
        if (!s_system) return;
        std::map<ViewId, VRBinding> binds;
        ButtonEdgeFn edgeCb = nullptr;
        void*        edgeUser = nullptr;
        std::uint32_t epoch = 0;
        bool          doSeed = false;
        {
            std::lock_guard<std::mutex> lk(s_bindMutex);
            edgeCb = s_edgeCb; edgeUser = s_edgeUser;
            epoch  = s_bindEpoch;
            if (edgeCb) { doSeed = s_edgeSeedPending; s_edgeSeedPending = false; }
            if (s_bindings.empty() && !edgeCb) return;
            binds = s_bindings;
        }
        vr::VRControllerState_t st[2]{};
        bool have[2] = { false, false };
        for (int hand = 0; hand < 2; ++hand) {
            const auto role = (hand == 1) ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand;
            const vr::TrackedDeviceIndex_t idx = s_system->GetTrackedDeviceIndexForControllerRole(role);
            if (idx == vr::k_unTrackedDeviceIndexInvalid || idx >= vr::k_unMaxTrackedDeviceCount) continue;
            have[hand] = s_system->GetControllerState(idx, &st[hand], sizeof(st[hand]));
        }
        if (!have[0] && !have[1]) return;

        // ── Capture mode: report edges, dispatch nothing ──────────────────
        if (edgeCb) {
            if (doSeed) {   // first tick after arm — seed on the owning thread
                s_edgeWasMask[0] = s_edgeWasMask[1] = 0xFFFFFFFFu;
                s_edgePending[0] = s_edgePending[1] = 0;
            }
            auto isModifierClass = [](std::uint32_t code) { return code == 2 || code == 33; };
            for (int hand = 0; hand < 2; ++hand) {
                if (!have[hand]) { s_edgeWasMask[hand] = 0; s_edgePending[hand] = 0; continue; }
                std::uint32_t mask = 0;
                for (std::size_t i = 0; i < std::size(kEdgeCodes); ++i)
                    if (ButtonHeld(st[hand], kEdgeCodes[i])) mask |= (1u << i);
                const std::uint32_t fresh = mask & ~s_edgeWasMask[hand];
                s_edgeWasMask[hand] = mask;

                std::uint32_t button = 0, heldOther = 0;
                if (fresh) {
                    // The pressed button is the newly-down one; the modifier
                    // is the first OTHER known button already held, in
                    // kEdgeCodes preference order.
                    for (std::size_t i = 0; i < std::size(kEdgeCodes); ++i)
                        if (fresh & (1u << i)) { button = kEdgeCodes[i]; break; }
                    for (std::size_t i = 0; i < std::size(kEdgeCodes); ++i)
                        if ((mask & (1u << i)) && kEdgeCodes[i] != button) { heldOther = kEdgeCodes[i]; break; }
                    if (isModifierClass(button) && heldOther == 0) {
                        // Alone: maybe the HOLD half. Wait for a second
                        // button or the release.
                        s_edgePending[hand] = button;
                        continue;
                    }
                } else if (s_edgePending[hand]) {
                    // No new press this tick: the pending modifier either
                    // is still held (keep waiting) or was released bare.
                    bool stillHeld = false;
                    for (std::size_t i = 0; i < std::size(kEdgeCodes); ++i)
                        if (kEdgeCodes[i] == s_edgePending[hand]) stillHeld = (mask & (1u << i)) != 0;
                    if (stillHeld) continue;
                    button = s_edgePending[hand];
                    heldOther = 0;
                } else {
                    continue;
                }
                s_edgePending[hand] = 0;
                const std::uint64_t now = GetTickCount64();
                if (now - s_captureLastTick < 250) continue;
                s_captureLastTick = now;
                SKSE::log::info("Magelight VR: button edge captured — button {} held {} hand {}",
                                button, heldOther, static_cast<std::uint8_t>(hand));
                edgeCb(button, heldOther, static_cast<std::uint8_t>(hand), edgeUser);
                break;   // one edge per tick is plenty for a capture UI
            }
            return;
        }

        for (const auto& [view, b] : binds) {
            if (!b.button) continue;
            bool down = false;
            for (int hand = 0; hand < 2; ++hand) {
                if (!have[hand]) continue;
                if (b.hand == 1 && hand != 0) continue;
                if (b.hand == 2 && hand != 1) continue;
                if (ButtonHeld(st[hand], b.button) && ButtonHeld(st[hand], b.modifier)) { down = true; break; }
            }
            bool& was = s_bindWas[view];
            auto& seenEpoch = s_bindWasEpoch[view];
            if (seenEpoch != epoch) {
                // The bind set changed (a (re)bind, an unbind, or capture
                // arming/ending). Re-latch EVERY view to live state so a
                // chord still physically held — from the press that chose
                // it, or from an unrelated view's chord — must be released
                // before it can fire. Reseeding only the rebound view left
                // bystander views to fire on a held-through chord.
                seenEpoch = epoch;
                was = down;
                continue;
            }
            if (down && !was) {
                // Per-view debounce: a single global timestamp let one
                // view's fire swallow another view's distinct hotkey within
                // 250ms (finding 5).
                std::uint64_t& lastTick = s_bindLastTick[view];
                const std::uint64_t now = GetTickCount64();
                if (now - lastTick >= 250) {
                    lastTick = now;
                    // A mod that owns its own open/close semantics gets the
                    // edge raw; everyone else gets the built-in action.
                    if (b.cb) b.cb(view, b.user);
                    else      Api4::DispatchViewAction(view, b.action, "vr");
                }
            }
            was = down;
        }
    }

    void BindHotkey(ViewId view, std::uint32_t button, std::uint32_t modifier,
                    std::uint8_t hand, std::uint32_t action)
    {
        std::lock_guard<std::mutex> lk(s_bindMutex);
        if (!button) { s_bindings.erase(view); ++s_bindEpoch; return; }
        s_bindings[view] = VRBinding{ button, modifier, action, hand, nullptr, nullptr };
        ++s_bindEpoch;
        SKSE::log::info("Magelight VR: view {} bound to controller button {} (modifier {}, hand {}, action {})",
            view, button, modifier, static_cast<int>(hand), action);
    }

    void SetButtonListener(ButtonEdgeFn cb, void* user)
    {
        std::lock_guard<std::mutex> lk(s_bindMutex);
        s_edgeCb = cb; s_edgeUser = user;
        // Every bound view must re-latch on the next tick — a chord held
        // through the capture must not fire a view when capture ends (finding 1).
        ++s_bindEpoch;
        // Ask the present-thread tick to seed the capture masks ("suppress
        // everything currently held, so the button the user clicked Rebind
        // with is not read as a fresh edge"). Writing that seed HERE, on the
        // game thread, raced the tick's own mask writes (finding 7).
        s_edgeSeedPending = (cb != nullptr);
        SKSE::log::info("Magelight VR: button listener {}", cb ? "armed — bindings suppressed" : "cleared");
    }

    void BindHotkeyCallback(ViewId view, std::uint32_t button, std::uint32_t modifier,
                            std::uint8_t hand, HotkeyFn cb, void* user)
    {
        std::lock_guard<std::mutex> lk(s_bindMutex);
        if (!button || !cb) { s_bindings.erase(view); ++s_bindEpoch; return; }
        s_bindings[view] = VRBinding{ button, modifier, 0u, hand, cb, user };
        ++s_bindEpoch;
        SKSE::log::info("Magelight VR: view {} bound to controller button {} (modifier {}, hand {}) -> mod callback",
            view, button, modifier, static_cast<int>(hand));
    }

    bool ViewBindsButton(ViewId view, std::uint32_t openvrButton)
    {
        if (!view || !openvrButton) return false;
        std::lock_guard<std::mutex> lk(s_bindMutex);
        const auto it = s_bindings.find(view);
        return it != s_bindings.end() && it->second.button == openvrButton;
    }

    void UnbindView(ViewId view)
    {
        {
            std::lock_guard<std::mutex> lk(s_bindMutex);
            s_bindings.erase(view);
            ++s_bindEpoch;
        }
        s_bindWas.erase(view);
        s_bindWasEpoch.erase(view);
        s_bindLastTick.erase(view);
        // Drop the keyboard focus too: the view going away must not leave the
        // runtime's keyboard up for a view that no longer exists. s_kbView is
        // the keyboard's, but the view lifecycle is the only thing that knows
        // both, so the clear belongs here.
        if (s_kbView == view) s_kbView = 0;
    }

}  // namespace Magelight::VR