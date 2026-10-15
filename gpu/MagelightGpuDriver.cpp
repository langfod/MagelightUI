// MagelightGPU — Direct3D 11 GPUDriver for Ultralight, adapted from
// ultralight-ux/AppCore v1.3.0 (LGPL-2.1; see gpu/LICENSE and gpu/README.md).
// Verified byte-equivalent to AppCore v1.4.0b (tag 52d4b918) for every method
// this file carries — upstream's ONLY 1.3→1.4.0b driver change is a
// _DEBUG→UL_DEBUG macro in device-creation code our adaptation removed.
//
// Carried forward to the 1.4.1 SDK WITHOUT re-baselining: the GPUDriver
// interface is unchanged, so no driver method needed a touch. Three things the
// bump did change, each marked "MG:" below: the compiled shader headers were
// rebuilt and renamed by upstream's new CMake shader build, the cbuffer grew an
// Integer4[2] and an int4 ClipData, and 1.4.1 added three filter programs.
//
// MG: re-baselined to the Ultralight 2.0.0-beta.2 SDK (the public GPUDriver
// interface CHANGED — this is no longer a carry-forward): CreateTexture gains
// usage flags, UpdateTexture gains a dirty rect, two Photon shader programs and
// a _2f_2ui instance vertex format joined the set, draws now carry blend factors
// in GPUState, and texture slots went from 2 to 4 (bound at BOTH shader stages).
// The reference D3D11 driver ships Zlib-licensed in the SDK's platform/ folder;
// this file keeps our host-specific shape (the game's device/context, the C ABI,
// SetSampleCount MSAA, external textures) and mirrors the reference where the two
// agree. Upstream sources consolidated here: src/common/GPUDriverImpl.*,
// src/win/d3d11/GPUContextD3D11.*, src/win/d3d11/GPUDriverD3D11.*.
// Modifications are marked "MG:".

#include <cstdio>
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#pragma warning(push)
#pragma warning(disable : 4100)
#include <Ultralight/Matrix.h>
#include <Ultralight/platform/GPUDriver.h>
#pragma warning(pop)

#include "MagelightGpuApi.h"

// MG: shader blobs are the SDK's own generated D3D11 set, copied verbatim from
// extern/ul-sdk-2.0.0-beta.2-win-x64-free/platform/shaders/generated/headers/d3d11
// (Ultralight 2.0.0-beta.2). The 2.0 build adds the two Photon programs (analytic
// vector rendering) and re-compiles every other program; none are hand-edited.
#include "shaders/fill_ps.h"
#include "shaders/fill_path_ps.h"
#include "shaders/filter_basic_ps.h"
#include "shaders/filter_blur_ps.h"
#include "shaders/filter_dropshadow_ps.h"
#include "shaders/vertex_path_vs.h"
#include "shaders/vertex_quad_vs.h"
// MG: 2.0's Photon programs (fill_photon uses the quad layout; fill_photon_grid
// its own _2f_2ui instance layout).
#include "shaders/fill_photon_ps.h"
#include "shaders/fill_photon_grid_ps.h"
#include "shaders/vertex_photon_grid_vs.h"

using Microsoft::WRL::ComPtr;

namespace {

    // MG: host-provided logger replaces upstream's MessageBoxW.
    MgGpuLogFn g_log = nullptr;
    void Log(const char* msg)
    {
        if (g_log) g_log(msg);
    }

// MG: 2.0 moved blend factors into GPUState (per draw). Map them to D3D11;
    // SrcAlphaSaturate has no D3D11 factor and Subtract/RevSubtract have no
    // D3D11 equation — both are rare CSS modes, so they degrade to ONE/ADD.
    static D3D11_BLEND BlendFactorToD3d11(ultralight::BlendFactor f)
    {
        switch (f) {
        case ultralight::BlendFactor::Zero: return D3D11_BLEND_ZERO;
        case ultralight::BlendFactor::One: return D3D11_BLEND_ONE;
        case ultralight::BlendFactor::SrcColor: return D3D11_BLEND_SRC_COLOR;
        case ultralight::BlendFactor::InvSrcColor: return D3D11_BLEND_INV_SRC_COLOR;
        case ultralight::BlendFactor::SrcAlpha: return D3D11_BLEND_SRC_ALPHA;
        case ultralight::BlendFactor::InvSrcAlpha: return D3D11_BLEND_INV_SRC_ALPHA;
        case ultralight::BlendFactor::DestColor: return D3D11_BLEND_DEST_COLOR;
        case ultralight::BlendFactor::InvDestColor: return D3D11_BLEND_INV_DEST_COLOR;
        case ultralight::BlendFactor::DestAlpha: return D3D11_BLEND_DEST_ALPHA;
        case ultralight::BlendFactor::InvDestAlpha: return D3D11_BLEND_INV_DEST_ALPHA;
        default:  // SrcAlphaSaturate
            return D3D11_BLEND_ONE;
        }
    }

// "<what> failed (0x...)", plus the device-removed reason when the device is gone.
    void LogFailure(const char* what, HRESULT hr, ID3D11Device* device)
    {
        char line[160];
        const HRESULT removed = device ? device->GetDeviceRemovedReason() : S_OK;
        if (FAILED(removed))
            std::snprintf(line, sizeof(line), "MgGpu: %s failed (0x%08X) - the device was removed (0x%08X)", what,
                static_cast<unsigned>(hr), static_cast<unsigned>(removed));
        else
            std::snprintf(line, sizeof(line), "MgGpu: %s failed (0x%08X)", what, static_cast<unsigned>(hr));
        Log(line);
    }

// MG: mirrors the SDK's cbuffer (platform/shaders/hlsl/common.hlsli) field for
    // field, in order. The 2.0 header keeps the 1.4.1 layout (Integer4[2], ClipData,
    // Clip[8]) — verified against the reference driver's own Uniforms mirror. 1.4.1 inserts Integer4[2] ahead of Scalar4 and
    // widens the clip count from a bare uint to an int4, so this struct grew AND
    // moved: a memcpy of the old layout lands every uniform after Transform in
    // the wrong register.
    //
    // Integer4 and ClipData are XMINT4, NOT XMFLOAT4 — the shaders declare them
    // int4, and both are 16 bytes so a float mirror has the identical LAYOUT and
    // still passes any offsetof/sizeof check. But a float written through
    // XMFLOAT4 stores its BIT PATTERN: clip_size 1 becomes 0x3F800000, which
    // clip.hlsli's applyClip reads as the loop bound, i.e. ~1.07e9 iterations
    // per pixel. That hangs the GPU (whole game, not just the view) the first
    // time a page uses a clip region or a filter. Upstream's GPUDriverD3D11.cpp
    // uses XMINT4 for both; so do we.
    struct Uniforms {
        DirectX::XMFLOAT4 State;
        DirectX::XMMATRIX Transform;
        DirectX::XMINT4 Integer4[2];
        DirectX::XMFLOAT4 Scalar4[2];
        DirectX::XMFLOAT4 Vector[8];
        DirectX::XMINT4 ClipData;
        DirectX::XMMATRIX Clip[8];
    };

}  // namespace

namespace ultralight {

    // ── GPUDriverImpl (upstream src/common, verbatim behaviour) ─────────────
    class GPUDriverImpl : public GPUDriver {
    public:
        GPUDriverImpl() : batch_count_(0) {}
        virtual ~GPUDriverImpl() {}

        virtual const char* name() = 0;
        virtual void BindTexture(uint8_t texture_unit, uint32_t texture_id) = 0;
        virtual void BindRenderBuffer(uint32_t render_buffer_id) = 0;
        virtual void ClearRenderBuffer(uint32_t render_buffer_id) = 0;
        virtual void DrawGeometry(uint32_t geometry_id, uint32_t indices_count,
                                  uint32_t indices_offset, const GPUState& state) = 0;

        bool HasCommandsPending() { return !command_list_.empty(); }

        void DrawCommandList()
        {
            if (command_list_.empty()) return;
            batch_count_ = 0;
            for (auto& cmd : command_list_) {
                if (cmd.command_type == CommandType::DrawGeometry)
                    DrawGeometry(cmd.geometry_id, cmd.indices_count, cmd.indices_offset, cmd.gpu_state);
                else if (cmd.command_type == CommandType::ClearRenderBuffer)
                    ClearRenderBuffer(cmd.gpu_state.render_buffer_id);
                // MG: 2.0 emits Flush before drawing into a target whose texture was
                // sampled earlier in the list; D3D11 immediate context: submit pending work.
                else if (cmd.command_type == CommandType::Flush)
                    Flush();
                batch_count_++;
            }
            command_list_.clear();
        }

        // MG: 2.0's flush hook (the reference driver declares it on its own class;
        // the public GPUDriver base has no such method, so this is ours to define).
        virtual void Flush() = 0;

        int batch_count() const { return batch_count_; }

        void BeginSynchronize() override {}
        void EndSynchronize() override {}
        uint32_t NextTextureId() override { return next_texture_id_++; }
        uint32_t NextRenderBufferId() override { return next_render_buffer_id_++; }
        uint32_t NextGeometryId() override { return next_geometry_id_++; }

        void UpdateCommandList(const CommandList& list) override
        {
            if (list.size) {
                command_list_.resize(list.size);
                std::memcpy(&command_list_[0], list.commands, sizeof(Command) * list.size);
            }
        }

    protected:
        uint32_t next_texture_id_ = 1;
        uint32_t next_render_buffer_id_ = 1;  // 0 reserved for default RT
        uint32_t next_geometry_id_ = 1;
        std::vector<Command> command_list_;
        int batch_count_;
    };

    // ── GPUContext (upstream GPUContextD3D11, adapted) ──────────────────────
    // MG: wraps the GAME's device/context instead of creating its own; no
    // swap chains; the MSAA sample count is the host's (SetSampleCount),
    // where upstream fixes it at compile time.
    class GPUContextD3D11 {
    public:
        GPUContextD3D11(ID3D11Device* device, ID3D11DeviceContext* context)
            : device_(device), immediate_context_(context)
        {
            D3D11_RENDER_TARGET_BLEND_DESC rt_blend_desc;
            ZeroMemory(&rt_blend_desc, sizeof(rt_blend_desc));
            rt_blend_desc.BlendEnable = true;
            rt_blend_desc.SrcBlend = D3D11_BLEND_ONE;
            rt_blend_desc.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            rt_blend_desc.BlendOp = D3D11_BLEND_OP_ADD;
            rt_blend_desc.SrcBlendAlpha = D3D11_BLEND_INV_DEST_ALPHA;
            rt_blend_desc.DestBlendAlpha = D3D11_BLEND_ONE;
            rt_blend_desc.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt_blend_desc.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

            D3D11_BLEND_DESC blend_desc;
            ZeroMemory(&blend_desc, sizeof(blend_desc));
            blend_desc.RenderTarget[0] = rt_blend_desc;
            device_->CreateBlendState(&blend_desc, blend_state_.GetAddressOf());

            ZeroMemory(&rt_blend_desc, sizeof(rt_blend_desc));
            rt_blend_desc.BlendEnable = false;
            rt_blend_desc.SrcBlend = D3D11_BLEND_ONE;
            rt_blend_desc.DestBlend = D3D11_BLEND_ZERO;
            rt_blend_desc.BlendOp = D3D11_BLEND_OP_ADD;
            rt_blend_desc.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt_blend_desc.DestBlendAlpha = D3D11_BLEND_ZERO;
            rt_blend_desc.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt_blend_desc.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

            ZeroMemory(&blend_desc, sizeof(blend_desc));
            blend_desc.RenderTarget[0] = rt_blend_desc;
            device_->CreateBlendState(&blend_desc, disabled_blend_state_.GetAddressOf());

            CreateRasterizerStates();
        }

        ID3D11Device* device() { return device_; }

        // MG: the MSAA sample count of render targets created from now on (1 = off). A count the device cannot
        // do for BGRA8 render targets steps down (8 -> 4 -> 2 -> 1). Returns the count in effect.
        UINT SetSampleCount(UINT samples)
        {
            UINT n = samples >= 8 ? 8 : samples >= 4 ? 4 : samples >= 2 ? 2 : 1;
            while (n > 1) {
                UINT quality = 0;
                if (SUCCEEDED(device_->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, n, &quality))
                    && quality > 0)
                    break;
                n /= 2;
            }
            if (n != sample_count_) {
                sample_count_ = n;
                CreateRasterizerStates();
            }
            return sample_count_;
        }
        UINT sample_count() const { return sample_count_; }
        ID3D11DeviceContext* immediate_context() { return immediate_context_; }

        void EnableBlend() { immediate_context_->OMSetBlendState(blend_state_.Get(), nullptr, 0xffffffff); }
        void DisableBlend() { immediate_context_->OMSetBlendState(disabled_blend_state_.Get(), nullptr, 0xffffffff); }

        // MG: 2.0 per-draw blend state, cached by (src, dst, equation); sets it on
        // the immediate context (the old Enable/DisableBlend pair is gone for draws).
        void SetBlendState(uint8_t src_factor, uint8_t dst_factor, uint8_t equation)
        {
            const uint32_t key = (uint32_t(src_factor) << 16) | (uint32_t(dst_factor) << 8) | uint32_t(equation);
            auto it = blend_states_.find(key);
            if (it == blend_states_.end()) {
                D3D11_BLEND_OP op;
                switch (equation) {
                case 0:  op = D3D11_BLEND_OP_ADD; break;           // Add
                case 2:  op = D3D11_BLEND_OP_REV_SUBTRACT; break; // RevSubtract
                case 3:  op = D3D11_BLEND_OP_MIN; break;
                case 4:  op = D3D11_BLEND_OP_MAX; break;
                default: op = D3D11_BLEND_OP_ADD; break;          // Subtract: no D3D11 equivalent
                }
                D3D11_RENDER_TARGET_BLEND_DESC rt{};
                rt.BlendEnable = true;
                rt.SrcBlend = BlendFactorToD3d11((ultralight::BlendFactor)src_factor);
                rt.DestBlend = BlendFactorToD3d11((ultralight::BlendFactor)dst_factor);
                rt.BlendOp = op;
                rt.SrcBlendAlpha = rt.SrcBlend;
                rt.DestBlendAlpha = rt.DestBlend;
                rt.BlendOpAlpha = op;
                rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
                D3D11_BLEND_DESC desc{};
                desc.RenderTarget[0] = rt;
                ComPtr<ID3D11BlendState> state;
                if (FAILED(device_->CreateBlendState(&desc, state.GetAddressOf()))) {
                    Log("MgGpu: SetBlendState failed");
                    return;
                }
                it = blend_states_.emplace(key, std::move(state)).first;
            }
            immediate_context_->OMSetBlendState(it->second.Get(), nullptr, 0xffffffff);
        }

        void EnableScissor() { immediate_context_->RSSetState(scissored_rasterizer_state_.Get()); }
        void DisableScissor() { immediate_context_->RSSetState(rasterizer_state_.Get()); }

    private:
        void CreateRasterizerStates()
        {
            rasterizer_state_.Reset();
            scissored_rasterizer_state_.Reset();
            D3D11_RASTERIZER_DESC rasterizer_desc;
            ZeroMemory(&rasterizer_desc, sizeof(rasterizer_desc));
            rasterizer_desc.FillMode = D3D11_FILL_SOLID;
            rasterizer_desc.CullMode = D3D11_CULL_NONE;
            rasterizer_desc.DepthClipEnable = false;
            rasterizer_desc.ScissorEnable = false;
            rasterizer_desc.MultisampleEnable = sample_count_ > 1;
            device_->CreateRasterizerState(&rasterizer_desc, rasterizer_state_.GetAddressOf());

            rasterizer_desc.ScissorEnable = true;
            device_->CreateRasterizerState(&rasterizer_desc, scissored_rasterizer_state_.GetAddressOf());
        }

        ID3D11Device* device_ = nullptr;                 // MG: not owned
        ID3D11DeviceContext* immediate_context_ = nullptr;  // MG: not owned
        UINT sample_count_ = 1;
        // MG: cached per-key blend states (keyed by src/dst/equation).
        std::map<uint32_t, ComPtr<ID3D11BlendState>> blend_states_;

        ComPtr<ID3D11BlendState> blend_state_;
        ComPtr<ID3D11BlendState> disabled_blend_state_;
        ComPtr<ID3D11RasterizerState> rasterizer_state_;
        ComPtr<ID3D11RasterizerState> scissored_rasterizer_state_;
    };

    // ── GPUDriverD3D11 (upstream, adapted) ──────────────────────────────────
    class GPUDriverD3D11 : public GPUDriverImpl {
    public:
        GPUDriverD3D11(GPUContextD3D11* context) : context_(context) {}
        virtual ~GPUDriverD3D11() {}

        // MG: 2.0 signature — `flags` carries kGPUTextureFlag_RenderTarget /
        // _Antialiased / _Immutable (see GPUDriver.h).
        void CreateTexture(uint32_t texture_id, RefPtr<Bitmap> bitmap, uint32_t flags) override
        {
            auto i = textures_.find(texture_id);
            if (i != textures_.end()) {
                Log("MgGpu: CreateTexture, texture id already exists");
                return;
            }
            const bool is_rt = (flags & kGPUTextureFlag_RenderTarget) != 0;
            const bool immutable = (flags & kGPUTextureFlag_Immutable) != 0
                || bitmap->format() == BitmapFormat::BC1_UNORM
                || bitmap->format() == BitmapFormat::BC2_UNORM
                || bitmap->format() == BitmapFormat::BC3_UNORM
                || bitmap->format() == BitmapFormat::BC7_UNORM;
            const auto fmt = bitmap->format();
            if (!(fmt == BitmapFormat::BGRA8_UNORM_SRGB || fmt == BitmapFormat::A8_UNORM
                  || fmt == BitmapFormat::RG8_UNORM || fmt == BitmapFormat::RGBA16F
                  || fmt == BitmapFormat::RGBA16UI  || fmt == BitmapFormat::RGBA32F
                  || fmt == BitmapFormat::BC1_UNORM  || fmt == BitmapFormat::BC2_UNORM
                  || fmt == BitmapFormat::BC3_UNORM  || fmt == BitmapFormat::BC7_UNORM)) {
                Log("MgGpu: CreateTexture, unsupported format");
                return;
            }

            D3D11_TEXTURE2D_DESC desc;
            ZeroMemory(&desc, sizeof(desc));
            desc.Width = bitmap->width();
            desc.Height = bitmap->height();
            desc.MipLevels = desc.ArraySize = 1;
            // MG: A8 render targets must be backed by R8 (A8 is not universally
            // renderable); the contract samples them from the RED channel.
            switch (fmt) {
            case BitmapFormat::BGRA8_UNORM_SRGB:
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
            case BitmapFormat::A8_UNORM:
                desc.Format = is_rt ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_A8_UNORM; break;
            case BitmapFormat::RG8_UNORM:  desc.Format = DXGI_FORMAT_R8G8_UNORM; break;
            case BitmapFormat::RGBA16F:    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
            case BitmapFormat::RGBA16UI:   desc.Format = DXGI_FORMAT_R16G16B16A16_UINT; break;
            case BitmapFormat::RGBA32F:    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
            case BitmapFormat::BC1_UNORM:  desc.Format = DXGI_FORMAT_BC1_UNORM; break;
            case BitmapFormat::BC2_UNORM:  desc.Format = DXGI_FORMAT_BC2_UNORM; break;
            case BitmapFormat::BC3_UNORM:  desc.Format = DXGI_FORMAT_BC3_UNORM; break;
            default:                       desc.Format = DXGI_FORMAT_BC7_UNORM; break;
            }
            desc.SampleDesc.Count = 1;
            // MG: sampled textures are USAGE_DEFAULT so UpdateTexture can write a
            // dirty box via UpdateSubresource (the old DYNAMIC/WRITE_DISCARD map
            // invalidated the whole resource on every update).
            desc.Usage = immutable ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
            desc.CPUAccessFlags = 0;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            auto& entry = textures_[texture_id];
            entry.is_immutable = immutable && !is_rt;
            HRESULT hr;

            if (is_rt) {
                // Render-target texture (a View's target, or one of Ultralight's layers). MG: with MSAA on,
                // Ultralight draws into msaa_texture and `texture` is its resolve target, so everything that
                // samples a driver texture (BindTexture, the host through GetTextureSRV) reads single-sample
                // pixels; upstream keeps a separate resolve texture and an unused multisample SRV instead.
                desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                desc.Usage = D3D11_USAGE_DEFAULT;
                hr = context_->device()->CreateTexture2D(&desc, nullptr, entry.texture.GetAddressOf());
                const UINT samples = context_->sample_count();
                // MG: single-channel (A8→R8) render targets are never multisampled.
                if (SUCCEEDED(hr) && samples > 1 && desc.Format != DXGI_FORMAT_R8_UNORM
                    && (flags & kGPUTextureFlag_Antialiased)) {
                    D3D11_TEXTURE2D_DESC ms = desc;
                    ms.SampleDesc.Count = samples;
                    ms.SampleDesc.Quality = 0;
                    ms.BindFlags = D3D11_BIND_RENDER_TARGET;
                    const HRESULT msHr = context_->device()->CreateTexture2D(&ms, nullptr, entry.msaa_texture.GetAddressOf());
                    if (FAILED(msHr)) LogFailure("CreateTexture (MSAA target, drawn without MSAA)", msHr, context_->device());
                }
            } else {
                D3D11_SUBRESOURCE_DATA tex_data;
                ZeroMemory(&tex_data, sizeof(tex_data));
                tex_data.pSysMem = bitmap->LockPixels();
                tex_data.SysMemPitch = static_cast<UINT>(bitmap->row_bytes());
                tex_data.SysMemSlicePitch = static_cast<UINT>(bitmap->size());
                hr = context_->device()->CreateTexture2D(&desc, &tex_data, entry.texture.GetAddressOf());
                bitmap->UnlockPixels();
            }
            if (FAILED(hr)) LogFailure("CreateTexture", hr, context_->device());

            D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
            ZeroMemory(&srv_desc, sizeof(srv_desc));
            srv_desc.Format = desc.Format;
            srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv_desc.Texture2D.MostDetailedMip = 0;
            srv_desc.Texture2D.MipLevels = 1;
            hr = context_->device()->CreateShaderResourceView(entry.texture.Get(), &srv_desc,
                                                              entry.texture_srv.GetAddressOf());
            if (FAILED(hr)) LogFailure("CreateTexture SRV", hr, context_->device());
        }

        // MG: 2.0 signature — `dirty_rect` is a hint; we upload the whole surface.
        void UpdateTexture(uint32_t texture_id, RefPtr<Bitmap> bitmap, const IntRect& dirty_rect) override
        {
            auto i = textures_.find(texture_id);
            if (i == textures_.end()) {
                Log("MgGpu: UpdateTexture, texture id doesn't exist");
                return;
            }
            auto& entry = i->second;
            if (entry.is_immutable) {
                // Immutable storage cannot be written; the engine never updates one.
                Log("MgGpu: UpdateTexture, texture is immutable");
                return;
            }
            // MG: USAGE_DEFAULT now, so Map(WRITE_DISCARD) is unavailable —
            // UpdateSubresource takes the rows directly (any pitch).
            const auto* src = static_cast<const uint8_t*>(bitmap->LockPixels());
            context_->immediate_context()->UpdateSubresource(
                entry.texture.Get(), 0, nullptr, src, bitmap->row_bytes(), 0);
            bitmap->UnlockPixels();
        }

        void DestroyTexture(uint32_t texture_id) override { textures_.erase(texture_id); }

        void CreateRenderBuffer(uint32_t render_buffer_id, const RenderBuffer& buffer) override
        {
            if (render_buffer_id == 0) {
                Log("MgGpu: CreateRenderBuffer, id 0 is reserved");
                return;
            }
            if (render_targets_.find(render_buffer_id) != render_targets_.end()) {
                Log("MgGpu: CreateRenderBuffer, id already exists");
                return;
            }
            auto tex_entry = textures_.find(buffer.texture_id);
            if (tex_entry == textures_.end()) {
                Log("MgGpu: CreateRenderBuffer, texture id doesn't exist");
                return;
            }

            // MG: RTV format must match the backing texture (2.0 added R8 and
            // data-page render targets; a hard-coded BGRA view fails on them).
            D3D11_RENDER_TARGET_VIEW_DESC rtv_desc;
            ZeroMemory(&rtv_desc, sizeof(rtv_desc));
            D3D11_TEXTURE2D_DESC texDesc{};
            tex_entry->second.texture->GetDesc(&texDesc);
            switch (texDesc.Format) {
            case DXGI_FORMAT_R8_UNORM:   rtv_desc.Format = DXGI_FORMAT_R8_UNORM; break;
            default:                     rtv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
            }
            const bool msaa = tex_entry->second.msaa_texture != nullptr;
            rtv_desc.ViewDimension = msaa ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;

            auto& rt_entry = render_targets_[render_buffer_id];
            HRESULT hr = context_->device()->CreateRenderTargetView(
                msaa ? tex_entry->second.msaa_texture.Get() : tex_entry->second.texture.Get(), &rtv_desc,
                rt_entry.render_target_view.GetAddressOf());
            if (FAILED(hr) && msaa) {
                // MG: the target draws without MSAA, the same state as a multisample texture that failed to create.
                LogFailure("CreateRenderBuffer (MSAA view, drawn without MSAA)", hr, context_->device());
                tex_entry->second.msaa_texture.Reset();
                tex_entry->second.needs_resolve = false;
                rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                hr = context_->device()->CreateRenderTargetView(tex_entry->second.texture.Get(), &rtv_desc,
                                                                rt_entry.render_target_view.GetAddressOf());
            }
            rt_entry.render_target_texture_id = buffer.texture_id;
            if (FAILED(hr)) Log("MgGpu: CreateRenderBuffer RTV failed");
        }

        void DestroyRenderBuffer(uint32_t render_buffer_id) override
        {
            auto i = render_targets_.find(render_buffer_id);
            if (i != render_targets_.end()) {
                i->second.render_target_view.Reset();
                render_targets_.erase(i);
            }
        }

        void CreateGeometry(uint32_t geometry_id, const VertexBuffer& vertices,
                            const IndexBuffer& indices) override
        {
            BindVertexLayout(vertices.format);
            if (geometry_.find(geometry_id) != geometry_.end()) return;

            GeometryEntry geometry;
            geometry.format = vertices.format;

            D3D11_BUFFER_DESC vertex_desc;
            ZeroMemory(&vertex_desc, sizeof(vertex_desc));
            vertex_desc.Usage = D3D11_USAGE_DYNAMIC;
            vertex_desc.ByteWidth = vertices.size;
            vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            vertex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            D3D11_SUBRESOURCE_DATA vertex_data;
            ZeroMemory(&vertex_data, sizeof(vertex_data));
            vertex_data.pSysMem = vertices.data;
            if (FAILED(context_->device()->CreateBuffer(&vertex_desc, &vertex_data,
                    geometry.vertexBuffer.GetAddressOf())))
                return;

            D3D11_BUFFER_DESC index_desc;
            ZeroMemory(&index_desc, sizeof(index_desc));
            index_desc.Usage = D3D11_USAGE_DYNAMIC;
            index_desc.ByteWidth = indices.size;
            index_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
            index_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            D3D11_SUBRESOURCE_DATA index_data;
            ZeroMemory(&index_data, sizeof(index_data));
            index_data.pSysMem = indices.data;
            if (FAILED(context_->device()->CreateBuffer(&index_desc, &index_data,
                    geometry.indexBuffer.GetAddressOf())))
                return;

            geometry_.insert({ geometry_id, std::move(geometry) });
        }

        void UpdateGeometry(uint32_t geometry_id, const VertexBuffer& vertices,
                            const IndexBuffer& indices) override
        {
            auto i = geometry_.find(geometry_id);
            if (i == geometry_.end()) {
                Log("MgGpu: UpdateGeometry, geometry id doesn't exist");
                return;
            }
            auto& entry = i->second;
            D3D11_MAPPED_SUBRESOURCE res;
            context_->immediate_context()->Map(entry.vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &res);
            std::memcpy(res.pData, vertices.data, vertices.size);
            context_->immediate_context()->Unmap(entry.vertexBuffer.Get(), 0);
            context_->immediate_context()->Map(entry.indexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &res);
            std::memcpy(res.pData, indices.data, indices.size);
            context_->immediate_context()->Unmap(entry.indexBuffer.Get(), 0);
        }

        void DestroyGeometry(uint32_t geometry_id) override
        {
            auto i = geometry_.find(geometry_id);
            if (i != geometry_.end()) {
                i->second.vertexBuffer.Reset();
                i->second.indexBuffer.Reset();
                geometry_.erase(i);
            }
        }

        // MG: 2.0 — submit pending work (the immediate context orders by spec,
        // but some driver stacks lose the ordering on rapid target alternation).
        void Flush() override { context_->immediate_context()->Flush(); }

        const char* name() override { return "Magelight D3D11"; }

        // MG: 2.0 — mirror every bind to the VERTEX stage too: FillPhotonGrid's
        // vertex shader reads the data pages; extra VS-stage SRVs are inert for
        // shaders that declare no textures there.
        void BindTexture(uint8_t texture_unit, uint32_t texture_id) override
        {
            auto i = textures_.find(texture_id);
            if (i == textures_.end()) {
                Log("MgGpu: BindTexture, texture id doesn't exist");
                return;
            }
            Resolve(i->second);
            context_->immediate_context()->PSSetShaderResources(
                texture_unit, 1, i->second.texture_srv.GetAddressOf());
            context_->immediate_context()->VSSetShaderResources(
                texture_unit, 1, i->second.texture_srv.GetAddressOf());
        }

        void BindRenderBuffer(uint32_t render_buffer_id) override
        {
            // MG: 2.0 has four texture slots (the fourth is the Photon index page).
            ID3D11ShaderResourceView* nullSRV[4] = { nullptr, nullptr, nullptr, nullptr };
            context_->immediate_context()->PSSetShaderResources(0, 4, nullSRV);
            context_->immediate_context()->VSSetShaderResources(0, 4, nullSRV);

            ID3D11RenderTargetView* target = GetRenderTargetView(render_buffer_id);
            if (!target) {
                Log("MgGpu: BindRenderBuffer, render buffer id doesn't exist");
                return;
            }
            context_->immediate_context()->OMSetRenderTargets(1, &target, nullptr);
        }

        void ClearRenderBuffer(uint32_t render_buffer_id) override
        {
            float color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            ID3D11RenderTargetView* target = GetRenderTargetView(render_buffer_id);
            if (!target) {
                Log("MgGpu: ClearRenderBuffer, render buffer id doesn't exist");
                return;
            }
            context_->immediate_context()->ClearRenderTargetView(target, color);
        }

        void DrawGeometry(uint32_t geometry_id, uint32_t indices_count, uint32_t indices_offset,
                          const GPUState& state) override
        {
            BindRenderBuffer(state.render_buffer_id);
            SetViewport(state.viewport_width, state.viewport_height);
            // MG: 2.0 — four slots (t0–t3), each at BOTH shader stages; the Photon
            // programs read their data pages from t0/t2/t3 and FillPhotonGrid's VS
            // reads them too.
            const uint32_t slot_ids[4] = { state.texture_1_id, state.texture_2_id,
                                            state.texture_3_id, state.texture_4_id };
            for (UINT slot = 0; slot < 4; ++slot) {
                if (slot_ids[slot]) BindTexture(static_cast<uint8_t>(slot), slot_ids[slot]);
            }
            UpdateConstantBuffer(state);
            BindGeometry(geometry_id);

            auto* ctx = context_->immediate_context();
            // MG: the Photon data pages are read by texel loads in the shader (no
            // interpolation), so the linear sampler is fine for every program.
            ctx->PSSetSamplers(0, 1, GetSamplerState().GetAddressOf());
            BindShader(state.shader_type);

            // MG: 2.0 — per-draw blend factors from GPUState (the old two-state
            // Enable/DisableBlend pair is gone; the library sets One/InvSrcAlpha/Add
            // for normal premultiplied-alpha drawing).
            if (state.enable_blend)
                context_->SetBlendState(static_cast<uint8_t>(state.blend_src_factor),
                                        static_cast<uint8_t>(state.blend_dst_factor),
                                        static_cast<uint8_t>(state.blend_equation));
            else
                context_->DisableBlend();

            if (state.enable_scissor) {
                context_->EnableScissor();
                D3D11_RECT scissor_rect = { (LONG)state.scissor_rect.left, (LONG)state.scissor_rect.top,
                                            (LONG)state.scissor_rect.right, (LONG)state.scissor_rect.bottom };
                ctx->RSSetScissorRects(1, &scissor_rect);
            } else {
                context_->DisableScissor();
            }

            ctx->VSSetConstantBuffers(0, 1, constant_buffer_.GetAddressOf());
            ctx->PSSetConstantBuffers(0, 1, constant_buffer_.GetAddressOf());
            ctx->DrawIndexed(indices_count, indices_offset, 0);
            batch_count_++;
        }

        // MG: host compositor support — the SRV for a View's target texture, resolved first when it is drawn
        // with MSAA. Render thread, after DrawCommandList.
        ID3D11ShaderResourceView* GetTextureSRV(uint32_t texture_id)
        {
            auto i = textures_.find(texture_id);
            if (i == textures_.end()) return nullptr;
            Resolve(i->second);
            return i->second.texture_srv.Get();
        }

        // MG: 2.0 optional capability report. BCn is core-required at feature
        // level 11_0 (every device we run on); MSAA mirrors the host's
        // SetSampleCount; partial redraw is a win on desktop targets.
        void GetDeviceCaps(GPUDeviceCaps& caps) override
        {
            const D3D_FEATURE_LEVEL fl = context_->device()->GetFeatureLevel();
            if (fl >= D3D_FEATURE_LEVEL_11_0)
                caps.compressed_formats = kGPUCompressedFormat_BC1_BC3 | kGPUCompressedFormat_BC7;
            caps.max_texture_dimension = fl >= D3D_FEATURE_LEVEL_11_0 ? 16384 : 8192;
            const UINT samples = context_->sample_count();
            if (samples > 1) {
                caps.supports_msaa = 1;
                caps.max_msaa_samples = samples;
            }
            caps.supports_partial_redraw = 1;
        }

        // MG: external textures (ImageSource). An SRV the host owns, entered
        // under a fresh id from the SAME counter Ultralight's CreateTexture
        // draws on — a hand-picked id would alias its next texture. BindTexture
        // then serves it like any driver texture; Ultralight never touches it.
        uint32_t RegisterExternal(ID3D11ShaderResourceView* srv)
        {
            if (!srv) return 0;
            const uint32_t id = NextTextureId();
            SetExternal(textures_[id], srv);
            return id;
        }
        bool SetExternalSRV(uint32_t texture_id, ID3D11ShaderResourceView* srv)
        {
            auto i = textures_.find(texture_id);
            if (i == textures_.end() || !srv) return false;
            SetExternal(i->second, srv);
            return true;
        }
        void UnregisterExternal(uint32_t texture_id) { textures_.erase(texture_id); }

    private:
        void LoadCompiledVertexShader(const unsigned char* data, unsigned int len,
                                      ID3D11VertexShader** vs,
                                      const D3D11_INPUT_ELEMENT_DESC* il, UINT ilCount,
                                      ID3D11InputLayout** layout)
        {
            if (FAILED(context_->device()->CreateVertexShader(data, len, nullptr, vs))) {
                Log("MgGpu: vertex shader creation failed");
                return;
            }
            if (FAILED(context_->device()->CreateInputLayout(il, ilCount, data, len, layout)))
                Log("MgGpu: input layout creation failed");
        }

        void LoadCompiledPixelShader(const unsigned char* data, unsigned int len, ID3D11PixelShader** ps)
        {
            if (FAILED(context_->device()->CreatePixelShader(data, len, nullptr, ps)))
                Log("MgGpu: pixel shader creation failed");
        }

        void LoadShaders()
        {
            if (!shaders_.empty()) return;

            // MG: 1.4.1's VS_INPUT_* declares colour as float4 and relies on the
            // input format doing the normalization ("D3D11 will normalize UNORM
            // format automatically"). The 1.4.0b shaders took uint4 and divided
            // by 255 themselves, which is why this used to be _UINT. Binding it
            // as UNORM with the new blobs is the pairing the SDK was compiled for;
            // the old pairing with new blobs renders every quad black.
            const D3D11_INPUT_ELEMENT_DESC layout_2f_4ub_2f[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };

            // MG: 2.0's FillPhotonGrid instance layout (pos + two uint slots).
            const D3D11_INPUT_ELEMENT_DESC layout_2f_2ui[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32_UINT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };
            auto& shader_fill_path = shaders_[ShaderType::FillPath];
            LoadCompiledVertexShader(vertex_path_vs_data, vertex_path_vs_size,
                                     shader_fill_path.first.GetAddressOf(), layout_2f_4ub_2f,
                                     ARRAYSIZE(layout_2f_4ub_2f), vertex_layout_2f_4ub_2f_.GetAddressOf());
            LoadCompiledPixelShader(fill_path_ps_data, fill_path_ps_size, shader_fill_path.second.GetAddressOf());

            const D3D11_INPUT_ELEMENT_DESC layout_2f_4ub_2f_2f_28f[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 5, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 6, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 7, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };
            // MG: Fill and all three filter programs share vertex_quad and the
            // quad input layout (common.hlsli: "Used for ShaderType::Fill,
            // FilterBasic, FilterBlur, FilterDropShadow").
            auto& shader_fill = shaders_[ShaderType::Fill];
            LoadCompiledVertexShader(vertex_quad_vs_data, vertex_quad_vs_size,
                                     shader_fill.first.GetAddressOf(), layout_2f_4ub_2f_2f_28f,
                                     ARRAYSIZE(layout_2f_4ub_2f_2f_28f),
                                     vertex_layout_2f_4ub_2f_2f_28f_.GetAddressOf());
            LoadCompiledPixelShader(fill_ps_data, fill_ps_size, shader_fill.second.GetAddressOf());

            // MG: 1.4.1 adds three filter programs. Each needs its own entry in
            // shaders_ or BindShader's operator[] hands back an empty pair and
            // binds a NULL shader, which D3D11 treats as "draw nothing".
            {
                auto& s = shaders_[ShaderType::FilterBasic];
                s.first = shader_fill.first;      // same vertex_quad_vs blob
                LoadCompiledPixelShader(filter_basic_ps_data, filter_basic_ps_size, s.second.GetAddressOf());
            }
            {
                auto& s = shaders_[ShaderType::FilterBlur];
                s.first = shader_fill.first;
                LoadCompiledPixelShader(filter_blur_ps_data, filter_blur_ps_size, s.second.GetAddressOf());
            }
            {
                auto& s = shaders_[ShaderType::FilterDropShadow];
                s.first = shader_fill.first;
                LoadCompiledPixelShader(filter_dropshadow_ps_data, filter_dropshadow_ps_size,
                                        s.second.GetAddressOf());
            }

            // MG: 2.0's Photon programs — fill_photon reuses the quad VS/layout;
            // fill_photon_grid carries its own _2f_2ui instance VS.
            {
                auto& s = shaders_[ShaderType::FillPhoton];
                s.first = shader_fill.first;
                LoadCompiledPixelShader(fill_photon_ps_data, fill_photon_ps_size,
                                        s.second.GetAddressOf());
            }
            {
                auto& s = shaders_[ShaderType::FillPhotonGrid];
                LoadCompiledVertexShader(vertex_photon_grid_vs_data, vertex_photon_grid_vs_size,
                                         s.first.GetAddressOf(), layout_2f_2ui,
                                         ARRAYSIZE(layout_2f_2ui),
                                         vertex_layout_2f_2ui_.GetAddressOf());
                LoadCompiledPixelShader(fill_photon_grid_ps_data, fill_photon_grid_ps_size,
                                        s.second.GetAddressOf());
            }
        }

        void BindShader(ShaderType shader_type)
        {
            LoadShaders();
            // MG: operator[] silently default-constructs a missing entry, so an
            // unknown shader type would bind two nulls and draw nothing rather
            // than fail. Now that 1.4.1 can hand us types we have not compiled,
            // say so instead.
            auto it = shaders_.find(shader_type);
            if (it == shaders_.end() || !it->second.first || !it->second.second) {
                Log("MgGpu: no shader bound for shader type (draw skipped)");
                return;
            }
            context_->immediate_context()->VSSetShader(it->second.first.Get(), nullptr, 0);
            context_->immediate_context()->PSSetShader(it->second.second.Get(), nullptr, 0);
        }

        void BindVertexLayout(VertexBufferFormat format)
        {
            LoadShaders();
            switch (format) {
            case VertexBufferFormat::_2f_4ub_2f:
                context_->immediate_context()->IASetInputLayout(vertex_layout_2f_4ub_2f_.Get());
                break;
            case VertexBufferFormat::_2f_4ub_2f_2f_28f:
                context_->immediate_context()->IASetInputLayout(vertex_layout_2f_4ub_2f_2f_28f_.Get());
                break;
            // MG: 2.0's instanced Photon grid layout.
            case VertexBufferFormat::_2f_2ui:
                context_->immediate_context()->IASetInputLayout(vertex_layout_2f_2ui_.Get());
                break;
            }
        }

        void BindGeometry(uint32_t id)
        {
            auto i = geometry_.find(id);
            if (i == geometry_.end()) return;
            auto* ctx = context_->immediate_context();
            auto& geometry = i->second;
            // MG: strides from the SDK's vertex structs (identical layout to
            // upstream's local DirectX-typed mirrors).
            UINT stride;
            switch (geometry.format) {
            case VertexBufferFormat::_2f_4ub_2f:
                stride = sizeof(ultralight::Vertex_2f_4ub_2f); break;
            // MG: 2.0's instanced Photon grid layout.
            case VertexBufferFormat::_2f_2ui:
                stride = sizeof(ultralight::Vertex_2f_2ui); break;
            default:
                stride = sizeof(ultralight::Vertex_2f_4ub_2f_2f_28f); break;
            }
            UINT offset = 0;
            ctx->IASetVertexBuffers(0, 1, geometry.vertexBuffer.GetAddressOf(), &stride, &offset);
            ctx->IASetIndexBuffer(geometry.indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            BindVertexLayout(geometry.format);
        }

        ID3D11RenderTargetView* GetRenderTargetView(uint32_t render_buffer_id)
        {
            // MG: no swap-chain fallback — offscreen render buffers only.
            auto i = render_targets_.find(render_buffer_id);
            if (i == render_targets_.end()) return nullptr;
            // Bound to be drawn or cleared: an MSAA target needs a resolve before it is next sampled.
            auto t = textures_.find(i->second.render_target_texture_id);
            if (t != textures_.end() && t->second.msaa_texture) t->second.needs_resolve = true;
            return i->second.render_target_view.Get();
        }

        ComPtr<ID3D11SamplerState> GetSamplerState()
        {
            if (sampler_state_) return sampler_state_;
            D3D11_SAMPLER_DESC sampler_desc;
            ZeroMemory(&sampler_desc, sizeof(sampler_desc));
            sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
            sampler_desc.MinLOD = 0;
            if (FAILED(context_->device()->CreateSamplerState(&sampler_desc, &sampler_state_)))
                Log("MgGpu: sampler state creation failed");
            return sampler_state_;
        }

        ComPtr<ID3D11Buffer> GetConstantBuffer()
        {
            if (constant_buffer_) return constant_buffer_;
            D3D11_BUFFER_DESC desc;
            ZeroMemory(&desc, sizeof(desc));
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.ByteWidth = sizeof(Uniforms);
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(context_->device()->CreateBuffer(&desc, nullptr, constant_buffer_.GetAddressOf())))
                Log("MgGpu: constant buffer creation failed");
            return constant_buffer_;
        }

        void SetViewport(uint32_t width, uint32_t height)
        {
            D3D11_VIEWPORT vp;
            ZeroMemory(&vp, sizeof(vp));
            vp.Width = (float)width;
            vp.Height = (float)height;
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            context_->immediate_context()->RSSetViewports(1, &vp);
        }

        void UpdateConstantBuffer(const GPUState& state)
        {
            auto buffer = GetConstantBuffer();
            Matrix model_view_projection = ApplyProjection(
                state.transform, (float)state.viewport_width, (float)state.viewport_height);

            // MG: zeroed, not just default-initialized: Clip[] past the active count is
            // never written, and MSAN (and /RTC in a debug build) flags handing
            // indeterminate stack bytes to a GPU copy.
            Uniforms uniforms{};
            uniforms.State = { 0.0f, (float)state.viewport_width, (float)state.viewport_height, 1.0f };
            uniforms.Transform = DirectX::XMMATRIX(model_view_projection.GetMatrix4x4().data);
            // MG: 1.4.1's filter_blur reads Integer(0)/Integer(1) as its pass
            // index and alpha-only flag. XMINT4, matching the shader's int4 —
            // see the struct comment on why a float mirror is not equivalent.
            uniforms.Integer4[0] = { (int)state.uniform_integer[0], (int)state.uniform_integer[1],
                                      (int)state.uniform_integer[2], (int)state.uniform_integer[3] };
            uniforms.Integer4[1] = { (int)state.uniform_integer[4], (int)state.uniform_integer[5],
                                      (int)state.uniform_integer[6], (int)state.uniform_integer[7] };
            uniforms.Scalar4[0] = { state.uniform_scalar[0], state.uniform_scalar[1],
                                    state.uniform_scalar[2], state.uniform_scalar[3] };
            uniforms.Scalar4[1] = { state.uniform_scalar[4], state.uniform_scalar[5],
                                    state.uniform_scalar[6], state.uniform_scalar[7] };
            for (size_t i = 0; i < 8; ++i)
                uniforms.Vector[i] = DirectX::XMFLOAT4(state.uniform_vector[i].x, state.uniform_vector[i].y,
                                                       state.uniform_vector[i].z, state.uniform_vector[i].w);
            // MG: the clip count moved into the x of an int4 (ClipData.x drives
            // applyClip's loop bound), and yzw are reserved by the SDK. It MUST
            // be written as an int: a float 1.0f here reads back as 1065353216
            // and every clipped pixel runs a billion-iteration loop.
            uniforms.ClipData = { (int)state.clip_size, 0, 0, 0 };
            for (size_t i = 0; i < state.clip_size; ++i)
                uniforms.Clip[i] = DirectX::XMMATRIX(state.clip[i].data);

            D3D11_MAPPED_SUBRESOURCE res;
            context_->immediate_context()->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &res);
            std::memcpy(res.pData, &uniforms, sizeof(Uniforms));
            context_->immediate_context()->Unmap(buffer.Get(), 0);
        }

        Matrix ApplyProjection(const Matrix4x4& transform, float screen_width, float screen_height)
        {
            Matrix transform_mat;
            transform_mat.Set(transform);
            Matrix result;
            result.SetOrthographicProjection(screen_width, screen_height, false);
            result.Transform(transform_mat);
            return result;
        }

        GPUContextD3D11* context_;
        ComPtr<ID3D11InputLayout> vertex_layout_2f_4ub_2f_;
        ComPtr<ID3D11InputLayout> vertex_layout_2f_4ub_2f_2f_28f_;
        // MG: 2.0's instanced Photon grid layout.
        ComPtr<ID3D11InputLayout> vertex_layout_2f_2ui_;
        ComPtr<ID3D11SamplerState> sampler_state_;
        ComPtr<ID3D11Buffer> constant_buffer_;

        struct GeometryEntry {
            VertexBufferFormat format;
            ComPtr<ID3D11Buffer> vertexBuffer;
            ComPtr<ID3D11Buffer> indexBuffer;
        };
        std::map<uint32_t, GeometryEntry> geometry_;

        struct TextureEntry {
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11ShaderResourceView> texture_srv;
            // MG: an MSAA render target's multisample surface (`texture` is its resolve target), and whether
            // it was drawn since the last resolve.
            ComPtr<ID3D11Texture2D> msaa_texture;
            bool needs_resolve = false;
            // MG: 2.0 — immutable (write-once / block-compressed) storage; UpdateTexture refuses it.
            bool is_immutable = false;
        };
        std::map<uint32_t, TextureEntry> textures_;

        void Resolve(TextureEntry& e)
        {
            if (!e.msaa_texture || !e.needs_resolve) return;
            context_->immediate_context()->ResolveSubresource(e.texture.Get(), 0, e.msaa_texture.Get(), 0,
                                                              DXGI_FORMAT_B8G8R8A8_UNORM);
            e.needs_resolve = false;
        }

        // MG: external-texture helper (declared after TextureEntry — a
        // parameter type is not in complete-class context, the body is).
        static void SetExternal(TextureEntry& e, ID3D11ShaderResourceView* srv)
        {
            e.msaa_texture.Reset();
            e.needs_resolve = false;
            e.texture_srv = srv;  // ComPtr assignment AddRefs
            ComPtr<ID3D11Resource> res;
            srv->GetResource(res.GetAddressOf());
            e.texture.Reset();
            if (res) res.As(&e.texture);  // null for a non-Texture2D resource; only the SRV is ever bound
        }

        struct RenderTargetEntry {
            ComPtr<ID3D11RenderTargetView> render_target_view;
            uint32_t render_target_texture_id = 0;
        };
        std::map<uint32_t, RenderTargetEntry> render_targets_;

        std::map<ShaderType, std::pair<ComPtr<ID3D11VertexShader>, ComPtr<ID3D11PixelShader>>> shaders_;
    };

}  // namespace ultralight

// ── C ABI (MG) ──────────────────────────────────────────────────────────────

namespace {
    struct MgGpuHandle {
        ultralight::GPUContextD3D11* context;
        ultralight::GPUDriverD3D11* driver;
    };
}

extern "C" {

    void* MgGpu_Create(ID3D11Device* device, ID3D11DeviceContext* context, MgGpuLogFn log)
    {
        if (!device || !context) return nullptr;
        g_log = log;
        auto* h = new MgGpuHandle();
        h->context = new ultralight::GPUContextD3D11(device, context);
        h->driver = new ultralight::GPUDriverD3D11(h->context);
        return h;
    }

    void MgGpu_Destroy(void* handle)
    {
        auto* h = static_cast<MgGpuHandle*>(handle);
        if (!h) return;
        delete h->driver;
        delete h->context;
        delete h;
    }

    void* MgGpu_GetGPUDriver(void* handle)
    {
        auto* h = static_cast<MgGpuHandle*>(handle);
        return h ? static_cast<ultralight::GPUDriver*>(h->driver) : nullptr;
    }

    int MgGpu_HasCommandsPending(void* handle)
    {
        auto* h = static_cast<MgGpuHandle*>(handle);
        return (h && h->driver->HasCommandsPending()) ? 1 : 0;
    }

    void MgGpu_DrawCommandList(void* handle)
    {
        auto* h = static_cast<MgGpuHandle*>(handle);
        if (h) h->driver->DrawCommandList();
    }

    int MgGpu_SetSampleCount(void* handle, int samples)
    {
        auto* h = static_cast<MgGpuHandle*>(handle);
        return h ? static_cast<int>(h->context->SetSampleCount(samples > 0 ? static_cast<UINT>(samples) : 1u)) : 0;
    }

    void* MgGpu_GetTextureSRV(void* handle, std::uint32_t texture_id)
    {
        auto* h = static_cast<MgGpuHandle*>(handle);
        return h ? h->driver->GetTextureSRV(texture_id) : nullptr;
    }

    std::uint32_t MgGpu_RegisterExternalTexture(void* handle, ID3D11ShaderResourceView* srv)
    {
        auto* h = static_cast<MgGpuHandle*>(handle);
        return h ? h->driver->RegisterExternal(srv) : 0;
    }

    int MgGpu_SetExternalTextureSRV(void* handle, std::uint32_t texture_id, ID3D11ShaderResourceView* srv)
    {
        auto* h = static_cast<MgGpuHandle*>(handle);
        return (h && h->driver->SetExternalSRV(texture_id, srv)) ? 1 : 0;
    }

    void MgGpu_UnregisterExternalTexture(void* handle, std::uint32_t texture_id)
    {
        auto* h = static_cast<MgGpuHandle*>(handle);
        if (h) h->driver->UnregisterExternal(texture_id);
    }

}  // extern "C"
