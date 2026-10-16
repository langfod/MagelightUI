// The host's D3D resource creation and the composite pass - stage 4 of the
// split. See Render.h for what the objects are.
//
// ── Why device creation is its own TU ───────────────────────────────────────
//
// Everything here runs ONCE, lazily, inside the present hook, and only after
// the world is ready (CLAUDE.md invariant 1: the swapchain that presents
// main-menu frames is a DIFFERENT thread from the one that presents in-game
// frames, and WebKit binds its main thread at Renderer::Create). It is split
// from Frame.cpp because the two have genuinely different lifetimes: this file
// runs once per session and then is never called again, while Frame.cpp runs
// every single frame for as long as the UI is up.
//
// ── Rules that keep this correct ───────────────────────────────────────────
//
//  1. THE GPU DRIVER MUST BE REGISTERED BEFORE Renderer::Create. The driver is
//     created against the GAME's device and context (both fetched from the
//     swapchain). Any failure falls back to the CPU surface path, which is a
//     supported configuration, not a failure: s_forceCpu selects it explicitly.
//  2. Our own FileSystem / Logger / Clipboard (host/Platform.cpp) are installed
//     here - this is the ONLY place the platform is configured.
//  3. s_device is never Released: it came from the swapchain GetDevice and a
//     wrapper is not obliged to AddRef it. Everything else created below IS
//     released, by the matching teardown.

#include "host/Render.h"
#include "host/State.h"
#include "host/Views.h"
#include "host/Platform.h"

#include "Magelight.h"
#include "../MagelightFonts.h"
#include "MagelightApi4.h"
#include "MagelightVR.h"

#include <JavaScriptCore/JavaScript.h>
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#pragma warning(push)
#pragma warning(disable : 4100)
#include <Ultralight/ImageSource.h>
#include <Ultralight/RenderTarget.h>
#include <Ultralight/Ultralight.h>
#include <Ultralight/platform/Config.h>
#include <Ultralight/platform/Platform.h>
#include <Ultralight/platform/Surface.h>
#include <AppCore/Platform.h>   // GetPlatformFontLoader
#pragma warning(pop)

namespace Magelight {

    static constexpr const char* kShaderSrc = R"HLSL(
struct VSIn  { float2 pos : POS; float2 uv : TEX; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(VSIn i) { VSOut o; o.pos = float4(i.pos, 0.0, 1.0); o.uv = i.uv; return o; }
Texture2D    tex0 : register(t0);
SamplerState smp0 : register(s0);
cbuffer Cut : register(b0) { float4 cut; };   // uv rect (x0,y0,x1,y1); x1<=x0 = none
float4 ps_main(VSOut i) : SV_Target {
    if (cut.z > cut.x && i.uv.x >= cut.x && i.uv.x < cut.z && i.uv.y >= cut.y && i.uv.y < cut.w) discard;
    return tex0.Sample(smp0, i.uv);
}
// VR copy pass: Ultralight's targets are PREMULTIPLIED; an OpenVR overlay
// texture is composited as straight alpha (SteamVR's default without
// VROverlayFlags_IsPremultiplied, which IVROverlay_016 predates). Divide out.
float4 ps_straight(VSOut i) : SV_Target {
    if (cut.z > cut.x && i.uv.x >= cut.x && i.uv.x < cut.z && i.uv.y >= cut.y && i.uv.y < cut.w) discard;
    float4 c = tex0.Sample(smp0, i.uv);
    if (c.a > 0.0001) c.rgb /= c.a;
    return c;
}
)HLSL";

    // Vertex is declared in host/Render.h - Draw.cpp sizes the buffer with it and
    // Frame.cpp fills it.

    // Classic arrow, 12x19: 'X' = black outline, 'o' = white fill, '.' =
    // transparent. Baked into an immutable premultiplied-BGRA texture and
    // drawn as the topmost quad at the MenuCursor position (hotspot 0,0).
    // kCursorW / kCursorH are declared in host/Render.h - Frame.cpp keeps the
    // fallback arrow's aspect ratio from them.
    static constexpr const char* kCursorMask[kCursorH] = {
        "X...........",
        "XX..........",
        "XoX.........",
        "XooX........",
        "XoooX.......",
        "XooooX......",
        "XoooooX.....",
        "XooooooX....",
        "XoooooooX...",
        "XooooooooX..",
        "XoooooXXXXX.",
        "XooXooX.....",
        "XoX.XooX....",
        "XX..XooX....",
        "X....XooX...",
        ".....XooX...",
        "......XooX..",
        "......XooX..",
        ".......XX...",
    };

    static bool CreateCursorTexture()
    {
        std::uint32_t px[kCursorW * kCursorH];
        for (int y = 0; y < kCursorH; ++y) {
            for (int x = 0; x < kCursorW; ++x) {
                const char c = kCursorMask[y][x];
                px[y * kCursorW + x] =
                    (c == 'X') ? 0xFF000000u : (c == 'o') ? 0xFFFFFFFFu : 0u;
            }
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kCursorW; td.Height = kCursorH;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sub{ px, kCursorW * sizeof(std::uint32_t), 0 };
        if (FAILED(s_device->CreateTexture2D(&td, &sub, &s_cursorTex))) {
            // 0.30.5: name the step. This function is called from a loop over the
            // three cursor textures, so the reason names WHICH one failed.
            NoteRenderFailure("the cursor texture could not be created");
            return false;
        }
        if (FAILED(s_device->CreateShaderResourceView(s_cursorTex, nullptr, &s_cursorSrv))) {
            NoteRenderFailure("the cursor texture's view could not be created");
            return false;
        }
        return true;
    }

    // Decode a PNG (any WIC-readable image) into a premultiplied-BGRA
    // immutable texture - the same encoding the baked arrow and every
    // Ultralight surface use, so the compositor's ONE/INV_SRC_ALPHA blend
    // applies unchanged. Capped at 512 (a cursor, not a wallpaper).
    // The VR laser-end pointer: a small white disc with a dark rim, alpha
    // antialiased, hotspot at the centre. An arrow made sense as a desktop
    // cursor; at the end of a laser it read as a big distracting sprite
    // (field 2026-09-06). 32x32 so it stays crisp when drawn at 8-16px.
    static constexpr int kDotSize = 32;
    // Core over rim, both 0xRRGGBB. Premultiplied like every mark the VR copy draws (ps_straight divides it out).
    bool MakeDotTexture(std::uint32_t coreRgb, std::uint32_t rimRgb, ID3D11Texture2D*& tex,
                        ID3D11ShaderResourceView*& srv)
    {
        std::uint32_t px[kDotSize * kDotSize];
        const float c = (kDotSize - 1) * 0.5f, rCore = 9.0f, rRim = 12.0f;
        const auto chan = [](std::uint32_t rgb, int shift) { return static_cast<float>((rgb >> shift) & 0xFF); };
        for (int y = 0; y < kDotSize; ++y) {
            for (int x = 0; x < kDotSize; ++x) {
                const float d = std::sqrt((x - c) * (x - c) + (y - c) * (y - c));
                float a = std::clamp(rRim + 0.5f - d, 0.0f, 1.0f);          // outer edge, antialiased
                const float core = std::clamp(rCore + 0.5f - d, 0.0f, 1.0f);  // the core over the rim (a is 1 here)
                const std::uint32_t A = static_cast<std::uint32_t>(a * 255.0f + 0.5f);
                std::uint32_t rgb = 0;
                for (int shift = 16; shift >= 0; shift -= 8) {
                    const float v = chan(rimRgb, shift) + (chan(coreRgb, shift) - chan(rimRgb, shift)) * core;
                    rgb |= static_cast<std::uint32_t>(v * a + 0.5f) << shift;
                }
                px[y * kDotSize + x] = (A << 24) | rgb;
            }
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kDotSize; td.Height = kDotSize;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sub{ px, kDotSize * sizeof(std::uint32_t), 0 };
        if (FAILED(s_device->CreateTexture2D(&td, &sub, &tex))) return false;
        if (FAILED(s_device->CreateShaderResourceView(tex, nullptr, &srv))) {
            tex->Release();
            tex = nullptr;
            return false;
        }
        return true;
    }
    static bool CreateDotTexture() { return MakeDotTexture(0xFFFFFF, 0x000000, s_dotTex, s_dotSrv); }

    bool DecodeImagePixels(const std::filesystem::path& path, unsigned maxPx, std::vector<std::uint8_t>& px,
                           int& outW, int& outH, std::string* why)
    {
        using Microsoft::WRL::ComPtr;
        const auto fail = [why](const std::string& w) { if (why) *why = w; return false; };
        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))))
            return fail("the image decoder is unavailable");
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnLoad, &decoder)))
            return fail("not an image Windows can read");
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame))) return fail("no image frame");
        ComPtr<IWICFormatConverter> conv;
        if (FAILED(factory->CreateFormatConverter(&conv))) return fail("the image decoder is unavailable");
        if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom)))
            return fail("the pixel format cannot be converted");
        UINT w = 0, h = 0;
        if (FAILED(conv->GetSize(&w, &h)) || !w || !h) return fail("the image is empty");
        if (w > maxPx || h > maxPx)
            return fail("larger than " + std::to_string(maxPx) + "x" + std::to_string(maxPx) + " (" +
                        std::to_string(w) + "x" + std::to_string(h) + ")");
        px.assign(static_cast<size_t>(w) * h * 4, 0);
        if (FAILED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data())))
            return fail("the pixels could not be read");
        outW = static_cast<int>(w);
        outH = static_cast<int>(h);
        return true;
    }

    bool UploadPixels(const std::vector<std::uint8_t>& px, int w, int h,
                      ID3D11Texture2D** outTex, ID3D11ShaderResourceView** outSrv)
    {
        using Microsoft::WRL::ComPtr;
        if (!s_device || w <= 0 || h <= 0 || px.size() < static_cast<size_t>(w) * h * 4) return false;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(w); td.Height = static_cast<UINT>(h);
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sub{ px.data(), static_cast<UINT>(w) * 4, 0 };
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(s_device->CreateTexture2D(&td, &sub, &tex))) return false;
        ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(s_device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) return false;
        *outTex = tex.Detach();
        *outSrv = srv.Detach();
        return true;
    }

    bool DecodeCursorImageFile(const std::filesystem::path& path,
                            ID3D11Texture2D** outTex, ID3D11ShaderResourceView** outSrv,
                            int& outW, int& outH)
    {
        // The game already initialized COM on this (main) thread; S_FALSE /
        // RPC_E_CHANGED_MODE are both fine and we never uninitialize.
        (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::vector<std::uint8_t> px;
        int w = 0, h = 0;
        if (!DecodeImagePixels(path, 512, px, w, h, nullptr)) return false;
        if (!UploadPixels(px, w, h, outTex, outSrv)) return false;
        outW = w;
        outH = h;
        return true;
    }

    // Swap the baked arrow for the configured cursor art when it loads.
    static void TryLoadCustomCursor()
    {
        if (s_cursorFile.empty()) return;
        const bool absolute = s_cursorFile.size() > 1 && s_cursorFile[1] == ':';
        const std::filesystem::path p = absolute
            ? std::filesystem::path(s_cursorFile)
            : (s_runtimeDir / std::filesystem::path(s_cursorFile).make_preferred());
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) {
            SKSE::log::info("Magelight: no cursor art at {} - baked arrow", p.string());
            return;
        }
        ID3D11Texture2D* tex = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        int w = 0, h = 0;
        if (!DecodeCursorImageFile(p, &tex, &srv, w, h)) {
            SKSE::log::warn("Magelight: cursor art {} failed to load - baked arrow", p.string());
            return;
        }
        if (s_cursorSrv) s_cursorSrv->Release();
        if (s_cursorTex) s_cursorTex->Release();
        s_cursorTex = tex;
        s_cursorSrv = srv;
        s_cursorImgW = w;
        s_cursorImgH = h;
        s_cursorCustom = true;
        SKSE::log::info("Magelight: cursor art {} ({}x{}, hotspot {:.2f},{:.2f}, height {}px@1080p)",
            p.string(), w, h, s_cursorHotX, s_cursorHotY, s_cursorHeight);
    }

    // Logs a failed device-resource step and records it as the render-death reason.
    static bool DeviceStepFailed(const char* step, HRESULT hr)
    {
        char detail[64];
        std::snprintf(detail, sizeof(detail), "HRESULT 0x%08X", static_cast<unsigned>(hr));
        SKSE::log::error("Magelight: {} failed (0x{:08X})", step, static_cast<unsigned>(hr));
        NoteRenderFailure(step, detail);
        return false;
    }

    bool CreateDeviceResources(IDXGISwapChain* sc)
    {
        // Swapchain first: its device is the one its back buffer lives on. The engine's stands in only when it
        // refuses (a Streamline wrapper over a Direct3D 12 frame-generation swapchain answers E_NOINTERFACE).
        ID3D11Device* dev = nullptr;
        HRESULT hr = E_NOINTERFACE;
        bool fromEngine = false;
        // Engine mode mixes nothing from the swapchain: under Streamline its wrapper objects remove the device.
        if (s_engineMode.load() && EngineDevice()) {
            dev = EngineDevice();
            dev->AddRef();
            hr = S_OK;
            fromEngine = true;
        } else {
            hr = sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev));
        }
        if (SUCCEEDED(hr) && dev) {
            s_device = dev;
        } else if (ID3D11Device* engine = EngineDevice()) {
            engine->AddRef();
            s_device = engine;
            fromEngine = true;
            s_untrustedChain.store(true);
            s_lateComposite.store(false);
            SKSE::log::warn("Magelight: the game swapchain refused its device (0x{:08X}) - using the engine's device",
                static_cast<unsigned>(hr));
        } else {
            return DeviceStepFailed("the game swapchain refused its device and the engine has none", hr);
        }
        s_device->GetImmediateContext(&s_context);
        SKSE::log::info("Magelight: device from the {}", fromEngine ? "engine" : "swapchain");

        ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
        if (FAILED(D3DCompile(kShaderSrc, std::strlen(kShaderSrc), nullptr, nullptr, nullptr,
                "vs_main", "vs_4_0", 0, 0, &vsb, &err))) {
            SKSE::log::error("Magelight: VS compile failed: {}",
                err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            if (err) err->Release();
            NoteRenderFailure("the vertex shader did not compile");   // 0.30.5: say which step
            return false;
        }
        if (FAILED(D3DCompile(kShaderSrc, std::strlen(kShaderSrc), nullptr, nullptr, nullptr,
                "ps_main", "ps_4_0", 0, 0, &psb, &err))) {
            SKSE::log::error("Magelight: PS compile failed: {}",
                err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            if (err) err->Release();
            vsb->Release();
            NoteRenderFailure("the pixel shader did not compile");
            return false;
        }
        hr = s_device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &s_vs);
        bool ok = SUCCEEDED(hr) || DeviceStepFailed("CreateVertexShader", hr);
        if (ok) {
            hr = s_device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &s_ps);
            ok = SUCCEEDED(hr) || DeviceStepFailed("CreatePixelShader", hr);
        }
        // The straight-alpha variant is only needed by the VR presenter; a
        // compile failure here degrades to the premultiplied copy, never to
        // a dead overlay.
        {
            ID3DBlob *sb = nullptr, *serr = nullptr;
            if (SUCCEEDED(D3DCompile(kShaderSrc, std::strlen(kShaderSrc), nullptr, nullptr, nullptr,
                    "ps_straight", "ps_4_0", 0, 0, &sb, &serr))) {
                s_device->CreatePixelShader(sb->GetBufferPointer(), sb->GetBufferSize(), nullptr, &s_psStraight);
                sb->Release();
            } else {
                SKSE::log::warn("Magelight: ps_straight compile failed: {}",
                    serr ? static_cast<const char*>(serr->GetBufferPointer()) : "?");
            }
            if (serr) serr->Release();
        }
        if (ok) {
            const D3D11_INPUT_ELEMENT_DESC il[] = {
                { "POS", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEX", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };
            hr = s_device->CreateInputLayout(il, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &s_layout);
            ok = SUCCEEDED(hr) || DeviceStepFailed("CreateInputLayout", hr);
        }
        vsb->Release();
        psb->Release();
        if (!ok) return false;

        // CPU-surface textures are per-view now, created lazily on first
        // upload (see EnsureCpuTexture).
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(Vertex) * 4;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(hr = s_device->CreateBuffer(&bd, nullptr, &s_vb))) return DeviceStepFailed("CreateBuffer (vertices)", hr);
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = 16;
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(hr = s_device->CreateBuffer(&cbd, nullptr, &s_cb))) return DeviceStepFailed("CreateBuffer (constants)", hr);

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        if (FAILED(hr = s_device->CreateSamplerState(&sd, &s_sampler))) return DeviceStepFailed("CreateSamplerState", hr);

        // Ultralight surfaces are premultiplied BGRA: ONE / INV_SRC_ALPHA.
        D3D11_BLEND_DESC bld{};
        bld.RenderTarget[0].BlendEnable = TRUE;
        bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(hr = s_device->CreateBlendState(&bld, &s_blend))) return DeviceStepFailed("CreateBlendState", hr);
        // Straight-alpha 'over' for compositing INTO the VR copy target (whose
        // pixels are straight after ps_straight): rgb = src*a + dst*(1-a).
        {
            D3D11_BLEND_DESC sb = bld;
            sb.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
            sb.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            if (FAILED(s_device->CreateBlendState(&sb, &s_blendStraight))) s_blendStraight = nullptr;
        }

        if (!CreateCursorTexture()) return false;
        if (!CreateDotTexture()) SKSE::log::warn("Magelight: VR pointer dot texture failed - the arrow art stands in");
        TryLoadCustomCursor();  // optional; keeps the arrow on any failure
        return true;
    }

    static void FontLog(int level, const char* message)
    {
        if (level) SKSE::log::warn("Magelight: {}", message);
        else SKSE::log::info("Magelight: {}", message);
    }

    bool CreateUltralight()
    {
        ultralight::Config cfg;
        // resource_path_prefix defaults to "resources/" resolved against the
        // platform file system's base dir - our runtime dir carries the ICU
        // data + cacert there.
        //
        // cache_path: without one, WebKit keeps localStorage in memory and
        // every session forgets it. SA's frontend stores real user data
        // there (margin notes, UI scale, pins), so park it beside the SKSE
        // logs - a writable, mod-manager-free location. Failure = empty
        // path = the old in-memory behavior, never fatal.
        if (const auto cache = CacheDirPath(); !cache.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(cache, ec);
            if (!ec) {
                const auto u8 = cache.u8string();
                cfg.cache_path = ultralight::String(reinterpret_cast<const char*>(u8.c_str()));
            }
        }
        cfg.font_hinting = s_fontHinting == "smooth"     ? ultralight::FontHinting::Smooth
                         : s_fontHinting == "monochrome" ? ultralight::FontHinting::Monochrome
                         : s_fontHinting == "none"       ? ultralight::FontHinting::None
                                                         : ultralight::FontHinting::Normal;
        // 0 = inherit the profile's value (the Default profile is 2.2), not "no
        // gamma" - documented here because a bare `cfg.font_gamma` reads as unset.
        cfg.font_gamma = s_fontGamma;

        // MG: explicit memory pins for a game process, where the UI shares RAM
        // with the world and a runaway WebKit heap is a stutter/leak vector. 2.0's
        // defaults are tuned for standalone apps (a 256 MiB cache); we keep the
        // recycler cadence but cap the decoded-image / compiled-JS cache at what
        // a fullscreen HUD plausibly holds, and let idle collection return freed
        // memory to the OS when the page goes quiet.
        cfg.recycle_delay = 0.2;      // s; 2.0 default - cheap per-frame recycle
        cfg.idle_gc_enabled = true;   // push freed memory back to the OS at rest
        cfg.memory_cache_size = 256 * 1024 * 1024;

        SKSE::log::info("Magelight: font hinting '{}', gamma {:.2f}", s_fontHinting, s_fontGamma);
        ultralight::Platform::instance().set_config(cfg);
        // The platform loader with a last resort that always loads (MagelightFonts.h):
        // with Arial missing or unreadable, the stock one crashed the first page that needed it.
        ultralight::Platform::instance().set_font_loader(
            MagelightFonts::CreateLoader(ultralight::GetPlatformFontLoader(), &FontLog));
        // Our MlFileSystem, not AppCore's registry-MIME one - see Platform.h
        // rule 1 (module scripts silently refuse a non-JS MIME).
        ultralight::Platform::instance().set_file_system(&UlFileSystem());
        ultralight::Platform::instance().set_logger(&UlLogger());
        ultralight::Platform::instance().set_clipboard(&UlClipboard());

        // Milestone 2: GPU rasterization. The driver must be registered BEFORE
        // Renderer::Create. Created against the GAME's device/context (both
        // fetched from the swapchain in CreateDeviceResources). Any failure
        // falls back to the CPU surface path.
        if (s_forceCpu.load()) {
            SKSE::log::info("Magelight: forceCpu set - GPU driver skipped, CPU rasterizer in use");
        } else if (s_gpuCreate && s_device && s_context) {
            s_gpu = s_gpuCreate(s_device, s_context, &GpuLogBridge);
            if (s_gpu) {
                if (auto* drv = static_cast<ultralight::GPUDriver*>(s_gpuGetDriver(s_gpu))) {
                    ultralight::Platform::instance().set_gpu_driver(drv);
                    s_gpuActive = true;
                }
            }
            if (!s_gpuActive) {
                SKSE::log::error("Magelight: GPU driver creation failed - CPU fallback");
            }
        }

        // MSAA for page render targets. BEFORE Renderer::Create on purpose: the
        // sample count applies to targets created from now on, and no view
        // (staggered or not) can have made one yet. Done after this, the first
        // views would silently be single-sampled.
        if (s_gpuSamples) {
            const int got = s_gpuSamples(s_gpu, s_msaa.load());
            if (got == s_msaa.load())
                SKSE::log::info("Magelight: MSAA {}x", got);
            else if (got > 0)
                SKSE::log::info("Magelight: MSAA {}x (Magelight.json asks {}x, which this graphics card "
                                "cannot do for page targets)", got, s_msaa.load());
            else
                SKSE::log::warn("Magelight: MSAA refused by the driver (asked {}x); pages render "
                                "single-sampled", s_msaa.load());
        }

        s_ulRenderer = ultralight::Renderer::Create();
        if (!s_ulRenderer) {
            // 0.30.5: WebKit's Renderer::Create is where a missing runtime or a
            // refused AppCore binds would surface, and it returns null rather
            // than throwing — so without this the log said only "init failed".
            NoteRenderFailure("the Ultralight renderer could not be created");
            return false;
        }
        SKSE::log::info("Magelight: renderer created ({} path)", s_gpuActive ? "GPU" : "CPU");
        return true;
    }

    // Absolute path ? file:/// URL (PathToFileUrl) is in host/Platform.cpp.

    // Render thread, per frame: give every registry entry that lacks one an
    // Ultralight view. Views registered from the game thread before the
    // renderer existed materialize here on the correct (owning) thread.
    // Render thread. Persistent Ultralight Sessions by name - one per mod
    // (localStorage/IndexedDB/cookies isolated under cache_path/<name>).
    static std::map<std::string, ultralight::RefPtr<ultralight::Session>> s_sessions;
    static ultralight::RefPtr<ultralight::Session> GetOrCreateSession(const std::string& name)
    {
        if (name.empty()) return nullptr;   // the renderer's default session
        auto it = s_sessions.find(name);
        if (it != s_sessions.end()) return it->second;
        auto session = s_ulRenderer->CreateSession(true, name.c_str());
        if (session) SKSE::log::info("Magelight: session '{}' created (persistent, isolated storage)", name);
        else SKSE::log::error("Magelight: session '{}' could not be created - falling back to default", name);
        s_sessions[name] = session;
        return session;
    }

    // What a view may show as its main frame, and what the bridge is installed on: the banner (LoadHTML,
    // about:) and the HTML files MlFileSystem serves, which are the documents that carry a policy. A file:
    // SVG runs script with no policy and keeps the file: origin, and a data: document inherits none, so a
    // navigation to either is stopped like a remote one.
    static bool IsLocalPageUrl(const std::string& url)
    {
        if (url.empty() || url.rfind("about:", 0) == 0) return true;
        if (url.rfind("file:", 0) != 0) return false;
        std::string path = url;
        if (const auto q = path.find_first_of("?#"); q != std::string::npos) path.resize(q);
        for (auto& c : path) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return path.ends_with(".html") || path.ends_with(".htm");
    }

    class MlLoadListener final : public ultralight::LoadListener {
    public:
        // Ultralight fires OnBeginLoading SYNCHRONOUSLY inside LoadURL, which
        // MaterializeViews calls while holding s_viewsMutex - taking the lock here
        // is a self-deadlock that the SEH guard turned into a dead overlay (0.16.0
        // field log). So this takes no lock: it only stops a main-frame navigation
        // off the page's own schemes. A top-level navigation is outside the page's
        // CSP, and a remote document would get the bridge and the mod's network
        // reach; Stop() here keeps the request from being sent.
        void OnBeginLoading(ultralight::View* caller, uint64_t, bool is_main_frame,
            const ultralight::String& url) override
        {
            if (!is_main_frame) return;
            const std::string u = url.utf8().data();
            if (IsLocalPageUrl(u)) return;
            caller->Stop();
            static std::atomic<int> s_logged{ 0 };
            if (s_logged.fetch_add(1) < 32)
                SKSE::log::warn("Magelight: stopped a page navigating to {} - a view shows its own HTML files only", u);
        }
        void OnWindowObjectReady(ultralight::View* caller, uint64_t, bool is_main_frame,
            const ultralight::String& url) override
        {
            if (!is_main_frame) return;
            if (!IsLocalPageUrl(url.utf8().data())) return;
            MlView* v = nullptr;
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                v = FindViewByUlLocked(caller);
            }
            if (v) InstallBridgeShims(*v, true);
        }
        void OnDOMReady(ultralight::View* caller, uint64_t, bool is_main_frame,
            const ultralight::String&) override
        {
            if (!is_main_frame) return;
            ViewId id = 0;
            DomReadyFn cb = nullptr;
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                if (MlView* v = FindViewByUlLocked(caller)) {
                    id = v->id;
                    cb = v->onDomReady;
                    v->domReady = true;   // outbound calls held during the load flow now
                    v->prepainted = false;
                    v->paintableAt = GetTickCount64();
                }
            }
            SKSE::log::info("Magelight: DOM ready (view {})", id);
            if (cb) cb(id);
            Emit(HostEvent::ViewDomReady, id);
            bool reloaded = false;
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                if (MlView* v = FindViewLocked(id)) { reloaded = v->reloadedFlag; v->reloadedFlag = false; }
            }
            if (reloaded) Emit(HostEvent::ViewReloaded, id);
        }
        void OnFailLoading(ultralight::View* caller, uint64_t, bool is_main_frame,
            const ultralight::String& url, const ultralight::String& description,
            const ultralight::String& error_domain, int error_code) override
        {
            if (!is_main_frame) return;
            ViewId id = 0;
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                if (MlView* v = FindViewByUlLocked(caller)) { id = v->id; v->loadFailed = true; }
            }
            const std::string detail = std::string(url.utf8().data()) + ": " + description.utf8().data() +
                " (" + error_domain.utf8().data() + ":" + std::to_string(error_code) + ")";
            SKSE::log::error("Magelight: view {} FAILED to load - {}", id, detail);
            Emit(HostEvent::ViewLoadFailed, id, error_code, 0, detail.c_str());
        }
    };

    // One instance each: the listeners carry no per-view state, they look the
    // view up from the View* Ultralight hands them. Both are attached to every
    // View in MaterializeViews below, so they must be declared before it.
    static MlLoadListener s_loadListener;

    class MlViewListener final : public ultralight::ViewListener {
    public:
        // Render thread, from inside a view's update: the CSS cursor under the pointer.
        void OnChangeCursor(ultralight::View* caller, ultralight::Cursor cursor) override
        {
            // try_lock: no path is known to fire this under s_viewsMutex, but the
            // core is closed, and a missed cursor change only waits for the next
            // mouse move. Blocking here would be a deadlock in the making.
            PageCursor kind = PageCursor::Arrow;
            switch (cursor) {
            case ultralight::kCursor_Hand:
            case ultralight::kCursor_Grab:
            case ultralight::kCursor_Grabbing:
                kind = PageCursor::Clickable;
                break;
            case ultralight::kCursor_IBeam:
            case ultralight::kCursor_VerticalText:
                kind = PageCursor::Text;
                break;
            // Probed against the 1.4 core: cursor: none reports kCursor_None, and
            // cursor: url(x) reports kCursor_Custom only once its image loaded
            // (a missing one falls to the next keyword in the list).
            case ultralight::kCursor_None:
                kind = PageCursor::None;
                break;
            case ultralight::kCursor_Custom:
                kind = PageCursor::Custom;
                break;
            default:
                break;
            }
            std::unique_lock<std::mutex> lk(s_viewsMutex, std::try_to_lock);
            if (!lk) return;
            if (MlView* v = FindViewByUlLocked(caller)) v->pageCursor = static_cast<int>(kind);
        }
        // Render thread, synchronously inside View::CreateLocalInspectorView.
        ultralight::RefPtr<ultralight::View> OnCreateInspectorView(ultralight::View* caller, bool,
                                                                   const ultralight::String&) override
        {
            return HostInspectorView(caller);
        }
    public:
        void OnAddConsoleMessage(ultralight::View* caller,
            const ultralight::ConsoleMessage& msg) override
        {
            const auto level = msg.level();
            const int lvlNum = (level == ultralight::kMessageLevel_Error) ? 2
                : (level == ultralight::kMessageLevel_Warning) ? 1 : 0;
            // 0.31.0 consoleLog gating (invariant 9). A page logs whatever it
            // likes — one wrote its config, API key included — so by default only
            // warnings and errors reach the log. Resolved ONCE here, on the
            // render thread, and the whole decision happens under s_viewsMutex
            // because Admit mutates the view's budget.
            const int cfg = ConsoleLogLevel();
            const bool wanted = cfg == kConsoleLogAll || (cfg == kConsoleLogWarnings && lvlNum >= 1)
                || (cfg == kConsoleLogErrors && lvlNum == 2);
            ViewId id = 0;
            bool logIt = false;
            std::uint32_t dropped = 0;
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                MlView* v = FindViewByUlLocked(caller);
                if (v) id = v->id;
                if (wanted) {
                    // A page outside the registry (its View dropping after the
                    // erase) shares one budget rather than escaping the cap.
                    static ConsoleRate s_unownedRate;
                    logIt = (v ? v->consoleRate : s_unownedRate).Admit(GetTickCount64(), dropped);
                }
            }
            const char* lvl = (level == ultralight::kMessageLevel_Error) ? "ERROR"
                : (level == ultralight::kMessageLevel_Warning) ? "warn"
                : (level == ultralight::kMessageLevel_Info) ? "info"
                : (level == ultralight::kMessageLevel_Debug) ? "debug" : "log";
            // Every console argument, not just the first: console.log('x', obj)
            // used to reach the log as "x". Objects come out as JSON.
            std::string text;
            if (JSContextRef ctx = msg.num_arguments() ? msg.argument_context() : nullptr) {
                for (std::uint32_t i = 0; i < msg.num_arguments(); ++i) {
                    JSValueRef v = msg.argument_at(i);
                    JSStringRef js = nullptr;
                    if (v && JSValueIsObject(ctx, v)) js = JSValueCreateJSONString(ctx, v, 0, nullptr);
                    if (!js && v) js = JSValueToStringCopy(ctx, v, nullptr);
                    if (i) text += ' ';
                    text += js ? JSStringToStd(js) : "undefined";
                    if (js) JSStringRelease(js);
                }
            } else {
                text = msg.message().utf8().data();
            }
            const std::string source = msg.source_id().utf8().data();
            if (dropped)
                SKSE::log::info("Magelight: [view {} console] {} lines not logged (over {} a second)",
                    id, dropped, kConsoleLinesPerSec);
            if (logIt) {
                SKSE::log::info("Magelight: [view {} console/{}] {} ({}:{})", id, lvl,
                    CapLogText(text, kConsoleLineMaxBytes), CapLogText(source, 512), msg.line_number());
            }
            // The owning mod's event and the devMode banner get the whole message, whatever consoleLog says.
            Emit(HostEvent::ConsoleMessage, id, lvlNum, static_cast<int>(msg.line_number()), text.c_str());
            if (lvlNum == 2 && s_devMode.load() && id) ShowDevErrorOverlay(id, text, source, msg.line_number());
        }
    };

    static MlViewListener s_viewListener;

    // Staggered first loads (render thread only). A page's parse and first
    // script run land in the following frames' Renderer::Update, so loading
    // every registered view at world ready stacked them all into the first
    // frames after a save loads. The queue starts one waiting view per frame,
    // and the next only once the previous one reached DOM ready or failed (or
    // kStaggerMaxWaitMs passed) AND the last frame's Update ran under
    // loadBudgetMs. Both caps are WALL TIME, not frames, so a slow post-load
    // frame rate cannot stretch the queue: a batch older than
    // kStaggerBatchMaxMs starts everything still waiting, which keeps
    // consumers that wait for DOM ready before they show a view (SeverActions'
    // popups and wheel) close to the 0.30.x readiness.
    static constexpr double kStaggerMaxWaitMs = 100.0;
    static constexpr double kStaggerBatchMaxMs = 1000.0;
    struct LoadStagger {
        ViewId last = 0;                 // the latest view loaded; the queue waits for it to settle
        std::chrono::steady_clock::time_point lastLoadAt{};   // epoch = nothing loaded yet (never waits)
        bool   batch = false;            // views have been waiting since batchStart
        int    batchQueued = 0, batchAtOnce = 0, batchFrames = 0, batchAtCap = 0;
        double batchWorstUpdateMs = 0.0;
        std::chrono::steady_clock::time_point batchStart{};
    };
    static LoadStagger s_stagger;

    // s_viewsMutex must be held. Creates the View, installs the listeners and
    // starts the load. False = the View could not be created (a later frame retries).
    // Extracted from MaterializeViews so the stagger queue and the
    // load-immediately paths share ONE load body — the queue must not be a
    // second copy of the load, or a fix to one silently misses the other.
    static bool LoadViewLocked(MlView& v)
    {
        if (v.ul || v.w <= 0 || v.h <= 0) return false;
        if (v.dormant && !v.visible) return false;   // hibernated: wakes on ShowView(true)
        const bool waking = v.dormant;
        v.dormant = false;
        v.domReady = false;
        v.prepainted = false;
        v.paintableAt = 0;
        v.loadFailed = false;
        v.pageCursor = 0;   // a new View has reported nothing yet
        ultralight::ViewConfig vc;
        vc.is_accelerated = s_gpuActive;
        vc.is_transparent = true;
        vc.initial_device_scale = v.deviceScale;
        v.ul = s_ulRenderer->CreateView(
            static_cast<std::uint32_t>(v.w), static_cast<std::uint32_t>(v.h), vc,
            GetOrCreateSession(v.sessionName));
        if (!v.ul) {
            SKSE::log::error("Magelight: CreateView failed for view {}", v.id);
            return false;
        }
        v.ul->set_load_listener(&s_loadListener);
        v.ul->set_view_listener(&s_viewListener);
        v.ul->set_network_listener(&UlNetworkListener());
        // Prefer the on-disk page (the path the React frontend ships
        // through). Relative paths resolve against s_runtimeDir via the
        // platform file system; a drive-letter path loads as-is (SA's
        // HTML lives in SA's own mod folder). Inline banner fallback.
        const bool absolute = v.htmlPath.size() > 1 && v.htmlPath[1] == ':';
        const std::filesystem::path onDisk =
            absolute ? std::filesystem::path(v.htmlPath) : (s_runtimeDir / v.htmlPath);
        if (!v.htmlPath.empty() && std::filesystem::exists(onDisk)) {
            SKSE::log::info("Magelight: view {} loading from disk ({})", v.id, v.htmlPath);
            const std::string url =
                absolute ? PathToFileUrl(onDisk) : ("file:///" + v.htmlPath);
            v.ul->LoadURL(url.c_str());
        } else {
            SKSE::log::warn("Magelight: view {} page '{}' missing - inline banner fallback",
                v.id, v.htmlPath);
            // The banner is served with LoadHTML (no file system), so it gets its CSP here, as MlFileSystem does for a page.
            std::string banner = kBannerHTML;
            InjectCsp(banner, NetLevel::File);
            v.ul->LoadHTML(banner.c_str());
        }
        v.liveScale = v.deviceScale;
        if (v.rebuilt)
            SKSE::log::info("Magelight: view {} rebuilt at device scale {:.3f}", v.id, v.deviceScale);
        else if (waking) {
            SKSE::log::info("Magelight: view {} woke from hibernation", v.id);
            // If it is the UI-mode target, the focus marker ran against
            // a null View - re-run it so keys reach the new page.
            if (s_focused.load() && v.id == static_cast<ViewId>(s_uiModeView.load())) QueueInput(0, 1, 0);
        }
        v.rebuilt = false;
        return true;
    }

    // Render thread: give every view that should have a page its View. Without
    // staggering (Magelight.json "loadStagger": false, or a hidden
    // view's loadOnShow) they all go at once; with it, one per frame in
    // registration order, gated on the previous one settling.
    void MaterializeViews()
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        const bool stagger = s_loadStagger.load();
        const ViewId uiView = s_focused.load() ? static_cast<ViewId>(s_uiModeView.load()) : 0;
        const auto nowT = std::chrono::steady_clock::now();
        std::vector<MlView*> queued;
        int atOnce = 0;
        for (auto& vp : s_views) {
            MlView& v = *vp;
            if (v.ul || v.w <= 0 || v.h <= 0 || v.destroyPending) continue;
            if ((v.dormant || v.loadOnShow) && !v.visible) continue;   // wakes / first loads on ShowView(true)
            const bool now = !stagger || v.dormant || v.loadNow || v.loadOnShow || v.id == uiView;
            if (!now) { queued.push_back(&v); continue; }
            if (LoadViewLocked(v)) {
                ++atOnce;
                s_stagger.last = v.id;
                s_stagger.lastLoadAt = nowT;
            }
        }
        if (queued.empty() && !s_stagger.batch) return;
        if (!s_stagger.batch) {
            s_stagger.batch = true;
            s_stagger.batchQueued = 0;
            s_stagger.batchAtOnce = 0;
            s_stagger.batchFrames = 0;
            s_stagger.batchAtCap = 0;
            s_stagger.batchWorstUpdateMs = 0.0;
            s_stagger.batchStart = nowT;
        }
        ++s_stagger.batchFrames;
        s_stagger.batchAtOnce += atOnce;
        s_stagger.batchWorstUpdateMs = std::max(s_stagger.batchWorstUpdateMs, s_lastUlUpdateMs);
        const auto msSince = [&](std::chrono::steady_clock::time_point t) {
            return std::chrono::duration<double, std::milli>(nowT - t).count();
        };
        if (!queued.empty() && msSince(s_stagger.batchStart) >= kStaggerBatchMaxMs) {
            // The batch ran past its wall-time cap: start everything still waiting now.
            std::vector<MlView*> retry;
            for (MlView* v : queued) {
                if (!LoadViewLocked(*v)) { retry.push_back(v); continue; }   // CreateView failed: a later frame
                ++s_stagger.batchAtCap;
                s_stagger.last = v->id;
                s_stagger.lastLoadAt = nowT;
            }
            queued.swap(retry);
        } else if (!queued.empty() && atOnce == 0) {
            bool settled = true;   // the latest load reached DOM ready or failed, or its view is gone or released
            if (MlView* prev = FindViewLocked(s_stagger.last))
                settled = prev->domReady || prev->loadFailed || !prev->ul || prev->destroyPending;
            const bool due = msSince(s_stagger.lastLoadAt) >= kStaggerMaxWaitMs;
            if (due || (settled && s_lastUlUpdateMs < static_cast<double>(s_loadBudgetMs.load()))) {
                std::sort(queued.begin(), queued.end(), [](const MlView* a, const MlView* b) { return a->id < b->id; });
                for (auto it = queued.begin(); it != queued.end(); ++it) {
                    if (!LoadViewLocked(**it)) continue;   // CreateView failed: retried on a later frame, the next goes now
                    ++s_stagger.batchQueued;
                    s_stagger.last = (*it)->id;
                    s_stagger.lastLoadAt = nowT;
                    queued.erase(it);
                    break;
                }
            }
        }
        if (queued.empty() && !s_stagger.batchAtOnce && !s_stagger.batchQueued && !s_stagger.batchAtCap) {
            s_stagger.batch = false;
            return;
        }
        const bool done = queued.empty();
        if (!done && !s_stagger.batchQueued) return;
        SKSE::log::info("Magelight: first loads staggered over {} frames — {} immediate, {} queued one-per-frame, "
                        "{} released at the {}-ms cap; worst Update {:.2f} ms",
            s_stagger.batchFrames, s_stagger.batchAtOnce, s_stagger.batchQueued,
            s_stagger.batchAtCap, static_cast<int>(kStaggerBatchMaxMs), s_stagger.batchWorstUpdateMs);
        if (done) s_stagger.batch = false;
    }

    // Render thread, s_viewsMutex held: drop the View and its texture but keep the record, marked dormant, so
    // MaterializeViews / ShowView(true) load the page again in a new View. The View goes into `released`, which the
    // caller clears after the lock: WebKit's teardown runs page handlers that re-enter the locking listeners.
    static void ReleaseViewKeepRecordLocked(MlView& v, std::vector<ultralight::RefPtr<ultralight::View>>& released)
    {
        // overlay + host copy go BEFORE the texture (lifetime rule);
        // hibernating=true keeps the view's controller binding (0.28.4) and its VR placement (0.31.7)
        VR::ReleaseView(v.id, /*hibernating=*/true);
        if (v.srv) { v.srv->Release(); v.srv = nullptr; }
        if (v.tex) { v.tex->Release(); v.tex = nullptr; }
        v.texW = v.texH = 0;
        v.dormant = true;
        v.domReady = false;
        v.prepainted = false;
        v.paintableAt = 0;
        v.liveScale = 0.0f;
        released.push_back(std::move(v.ul));
        v.ul = nullptr;
    }

    // Render thread, before MaterializeViews: release the View + texture of
    // any view hidden longer than its hibernate budget. The record stays;
    // everything the page needs to come back (path, listeners, session,
    // layer, cutout) is on it. Never the UI-mode view, never an inspector.
    void HibernateIdleViews()
    {
        const std::uint64_t now = GetTickCount64();
        std::vector<ultralight::RefPtr<ultralight::View>> released;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            const ViewId uiView = static_cast<ViewId>(s_uiModeView.load());
            for (auto& vp : s_views) {
                MlView& v = *vp;
                if (!v.ul || v.visible || !v.hibernateMs || !v.hiddenSince || v.destroyPending || v.isInspector) continue;
                if (v.id == uiView && s_focused.load()) continue;
                if (now - v.hiddenSince < v.hibernateMs) continue;
                ReleaseViewKeepRecordLocked(v, released);
                SKSE::log::info("Magelight: view {} hibernated after {} ms hidden (texture released)", v.id, v.hibernateMs);
            }
        }
        released.clear();   // WebKit teardown outside the lock, like a destroy
    }

    // Render thread, after HibernateIdleViews and before MaterializeViews:
    // RebuildViewAtScale. The View is released as hibernation releases it and
    // the record loads again in a new View created with initial_device_scale =
    // the new deviceScale: a visible view in this same frame (MaterializeViews
    // loads a dormant visible view at once), a hidden one at once too unless
    // it hibernates or loads on show (then it stays dormant until shown).
    // Never set_device_scale on a laid-out page. A page hosting a Web Inspector
    // is not rebuilt (the inspector is bound to its View): it keeps its live scale.
    void ApplyPendingRebuilds()
    {
        std::vector<ultralight::RefPtr<ultralight::View>> released;
        std::vector<ViewId> rebuilt;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            for (auto& vp : s_views) {
                MlView& v = *vp;
                if (!v.rebuildPending) continue;
                v.rebuildPending = false;
                if (!v.ul || v.destroyPending) continue;   // released meanwhile: the next load uses deviceScale
                if (s_inspectorOf.count(v.id)) {
                    v.deviceScale = v.liveScale;   // hit tests, the IME caret and GetViewInfo keep the View's scale
                    SKSE::log::warn("Magelight: view {} not rebuilt - it hosts a Web Inspector; it stays at device "
                                    "scale {:.3f}", v.id, v.liveScale);
                    continue;
                }
                ReleaseViewKeepRecordLocked(v, released);
                v.scaleDirty = false;     // the new View is created at deviceScale
                v.boundsDirty = false;    // and at the current w/h
                v.reloadedFlag = true;    // ViewReloaded follows the new page's DOM ready
                v.rebuilt = true;
                const bool waitForShow = !v.visible && (v.hibernateMs || v.loadOnShow);
                if (!v.visible && !waitForShow) {   // never hibernates: reload now, hidden
                    v.dormant = false;
                    v.loadNow = true;
                }
                rebuilt.push_back(v.id);
                SKSE::log::info("Magelight: view {} released for a rebuild at device scale {:.3f}{}", v.id,
                    v.deviceScale, waitForShow ? " (hidden: it loads when shown)" : "");
            }
        }
        released.clear();   // WebKit teardown outside the lock, like a destroy
        // The old page's focused text field went with it: clear its keyboard claims as a '__textfocus' '0' would.
        for (const ViewId id : rebuilt) {
            VR::NoteTextFocus(id, false);
            if (IsUIModeActive() && id == static_cast<ViewId>(s_uiModeView.load()) &&
                id != static_cast<ViewId>(s_kbViewId.load())) {
                ImeOnTextFocus(false);
                ShowOwnKeyboard(false);
            }
        }
    }

    // Render thread: apply a pending SetViewBounds resize.
    void ApplyPendingResizes()
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        for (auto& vp : s_views) {
            MlView& v = *vp;
            if (!v.ul || !v.boundsDirty) continue;
            v.boundsDirty = false;
            if (v.domReady) {   // a resize repaints the whole page: prepaint it again once it settles
                v.prepainted = false;
                v.paintableAt = GetTickCount64();
            }
            if (v.scaleDirty) {   // 0.26.9 SetViewScale, applied here on the render thread
                v.scaleDirty = false;
                v.ul->set_device_scale(v.deviceScale > 0.f ? v.deviceScale : 1.f);
                v.liveScale = v.deviceScale > 0.f ? v.deviceScale : 1.f;
            }
            if (v.w > 0 && v.h > 0) {
                v.ul->Resize(static_cast<std::uint32_t>(v.w), static_cast<std::uint32_t>(v.h));
                // CPU-path texture no longer matches - recreate lazily.
                if (v.srv) { v.srv->Release(); v.srv = nullptr; }
                if (v.tex) { v.tex->Release(); v.tex = nullptr; }
                v.texW = v.texH = 0;
            }
        }
    }

    // Render thread: reloads, navigations and destroys queued by the API.
    // Destroy erases the registry entry and releases the Ultralight view -
    // the listeners key on View*, so erase BEFORE the RefPtr drops (WebKit
    // may recycle the address). Runs outside Update()/Render(), where no
    // listener can be mid-call.
    void ApplyPendingLifecycle()
    {
        // Collect under the lock, ACT OUTSIDE IT: LoadURL/Reload on a live
        // page and dropping a View's last RefPtr run WebKit synchronously
        // (beforeunload/unload handlers, cancelled provisional loads), which
        // re-enters the console / load-failed listeners - and those take
        // s_viewsMutex. Same shape as DrainBridgeQueues.
        struct Work { ultralight::RefPtr<ultralight::View> ul; ViewId id; std::string url; bool reload; };
        std::vector<Work> work;
        std::vector<std::pair<ViewId, ultralight::RefPtr<ultralight::View>>> destroyed;
        // EvalJS requests still queued on a view being destroyed: their fn is
        // the EvalCtx-owning trampoline, so if we drop them the EvalCtx leaks
        // and the mod's callback never fires. Flush them (with a failure)
        // OUTSIDE the lock (review 2026-09-05, finding 2).
        std::vector<std::pair<ViewId, PendingEval>> orphanEvals;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            for (auto it = s_views.begin(); it != s_views.end();) {
                MlView& v = **it;
                if (v.destroyPending) {
                    const ViewId id = v.id;
                    VR::ReleaseView(id);   // overlay destroyed before the View RefPtr drops
                    if (v.srv) { v.srv->Release(); v.srv = nullptr; }
                    if (v.tex) { v.tex->Release(); v.tex = nullptr; }
                    for (auto& e : v.evals) orphanEvals.emplace_back(id, std::move(e));
                    v.evals.clear();
                    destroyed.emplace_back(id, std::move(v.ul));   // released after the erase, outside the lock
                    it = s_views.erase(it);
                    continue;
                }
                if (v.ul && !v.navigateUrl.empty()) {
                    work.push_back({ v.ul, v.id, v.navigateUrl, false });
                    v.htmlPath = v.navigateUrl.rfind("file:///", 0) == 0 ? v.navigateUrl.substr(8) : v.navigateUrl;
                    v.navigateUrl.clear();
                    v.reloadedFlag = true;
                } else if (v.ul && v.reloadPending) {
                    work.push_back({ v.ul, v.id, {}, true });
                    v.reloadedFlag = true;
                }
                v.reloadPending = false;
                ++it;
            }
        }
        for (auto& w : work) {
            if (w.reload) {
                SKSE::log::info("Magelight: view {} reloading", w.id);
                w.ul->Reload();
            } else {
                SKSE::log::info("Magelight: view {} navigating to {}", w.id, w.url);
                w.ul->LoadURL(w.url.c_str());
            }
        }
        for (auto& [id, e] : orphanEvals)
            if (e.fn) e.fn(id, "", "Magelight: view destroyed before its eval ran", e.user);
        for (auto& [id, ul] : destroyed) {
            ul = nullptr;   // the registry entry is already gone; a recycled View* can't alias a live view
            SKSE::log::info("Magelight: view {} destroyed", id);
            Emit(HostEvent::ViewDestroyed, id);
        }
    }

    // ── Host settings (Magelight.json) ──────────────────────────────
    // Definitions for the externs in host/State.h. LoadHostSettings
    // (Magelight.cpp) writes them; the render path reads them. None of
    // these is static any more — that is the point of the split.

    std::atomic<bool> s_forceCpu{ false };
    std::string s_fontHinting = "normal";
    double      s_fontGamma   = 1.8;
    std::string s_cursorFile = "cursor.png";
    float       s_cursorHeight = 36.0f;   // was 44 — a tad too big on flat at 1080p (owner, 2026-09-13); VR has its own vr.cursorScale
    float       s_cursorHotX = 0.044f, s_cursorHotY = 0.01f;
    bool        s_cursorCustom = false;   // custom art loaded (render thread)
    int         s_cursorImgW = 0, s_cursorImgH = 0;
    // 0.31.0 per-view cursors (read by PickCursorAt in src/host/Render.cpp).
    std::atomic<bool> s_cursorForce{ false };   // the host's own cursor always wins
    std::atomic<bool> s_modCursors{ true };    // false drops mod images AND tints
    bool s_imageProbe = false;


    // Milestone-3 playground: hover styling, click counters, a text field, and
    // a scrollable list — one widget per input path (mouse move/down/up, key,
    // char, wheel). The clock keeps proving the JS engine ticks.
    //
    // Not static, and not constexpr: host/State.h declares it `extern const
    // char* const`, so a static definition here would be C4211 (redefined
    // extern to static) and would give this a different object from the one
    // MaterializeViews reads. const alone is enough — the string literal
    // already has static storage duration.
    const char* const kBannerHTML = R"HTML(
<!DOCTYPE html>
<html><head><style>
  html, body { margin:0; background:transparent; overflow:hidden;
               font-family:Georgia,serif; color:#3a2d17; }
  .card {
    box-sizing:border-box; width:100%; height:100%;
    background:linear-gradient(160deg,#efe3c8,#e2d2ae);
    border:2px solid #8a6d3b; border-radius:10px;
    padding:16px 20px; box-shadow:inset 0 0 40px rgba(120,90,40,.25);
  }
  h1 { margin:0 0 2px; font-size:24px; }
  .sub { font-style:italic; color:#6b5527; font-size:13px; margin-bottom:10px; }
  .row { display:flex; gap:10px; align-items:center; margin-top:10px; }
  button {
    font:inherit; font-size:14px; padding:6px 14px; cursor:pointer;
    background:#e8d9b5; color:#3a2d17; border:1px solid #8a6d3b; border-radius:6px;
  }
  button:hover { background:#8a6d3b; color:#f4ecd8; }
  button:active { transform:translateY(1px); }
  input[type=text] {
    font:inherit; font-size:14px; flex:1; padding:6px 10px;
    background:#f7efdb; border:1px solid #8a6d3b; border-radius:6px; color:#3a2d17;
  }
  #list { margin-top:10px; height:110px; overflow-y:auto; border:1px solid #b39a67;
          border-radius:6px; background:rgba(255,250,235,.55); padding:4px 10px; font-size:13px; }
  #list div { padding:2px 0; border-bottom:1px dotted #c9b184; }
  .foot { display:flex; justify-content:space-between; margin-top:10px; font-size:12px;
          color:#6b5527; }
  .ok { color:#3f6d2a; font-weight:bold; }
  #clock { font-variant-numeric:tabular-nums; }
</style></head>
<body><div class="card">
  <h1>&#10022; Magelight UI &mdash; input playground</h1>
  <div class="sub">GPU-rendered by Ultralight, input routed by our own host. ESC or Page Up closes.</div>
  <div class="row">
    <button id="btn">Click me</button>
    <span id="clicks">0 clicks</span>
    <span id="hover" style="margin-left:auto">hover: &mdash;</span>
  </div>
  <div class="row">
    <input id="txt" type="text" placeholder="type here to test the keyboard&hellip;">
  </div>
  <div class="row"><span id="echo" style="font-size:13px;color:#6b5527">echo: &mdash;</span></div>
  <div id="list"></div>
  <div class="foot">
    <span>Renderer: <span class="ok">GPU</span> &middot; JS: <span class="ok" id="js">warming&hellip;</span></span>
    <span id="clock">--:--:--</span>
  </div>
</div>
<script>
  var clicks = 0;
  var btn = document.getElementById('btn');
  btn.addEventListener('click', function(){
    clicks++;
    document.getElementById('clicks').textContent = clicks + ' click' + (clicks===1?'':'s');
  });
  btn.addEventListener('mouseenter', function(){ document.getElementById('hover').textContent = 'hover: button'; });
  btn.addEventListener('mouseleave', function(){ document.getElementById('hover').textContent = 'hover: —'; });
  document.getElementById('txt').addEventListener('input', function(e){
    document.getElementById('echo').textContent = 'echo: ' + e.target.value;
  });
  var list = document.getElementById('list');
  for (var i = 1; i <= 40; i++) {
    var d = document.createElement('div');
    d.textContent = 'Scroll test row ' + i + ' — wheel over this box';
    list.appendChild(d);
  }
  document.getElementById('js').textContent = 'executing';
  setInterval(function(){
    document.getElementById('clock').textContent = new Date().toLocaleTimeString();
  }, 1000);
</script>
</body></html>
)HTML";

}  // namespace Magelight
