// MagelightGPU — Direct3D 11 GPUDriver for Ultralight, adapted from
// ultralight-ux/AppCore v1.3.0 (LGPL-2.1; see gpu/LICENSE and gpu/README.md).
// Verified byte-equivalent to AppCore v1.4.0b (tag 52d4b918) for every method
// this file carries — upstream's ONLY 1.3→1.4.0b driver change is a
// _DEBUG→UL_DEBUG macro in device-creation code our adaptation removed; the
// four compiled shader headers are byte-identical to the 1.4 SDK's. (Do NOT
// re-baseline from AppCore MASTER: that's the post-1.4 line — filter shaders,
// blend-state cache, d3d12 — matching a future SDK, not 1.4.0b.)
// Upstream sources consolidated here: src/common/GPUDriverImpl.*,
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

#include <Ultralight/Matrix.h>
#include <Ultralight/platform/GPUDriver.h>

#include "MagelightGpuApi.h"

#include "shaders/fill_fxc.h"
#include "shaders/fill_path_fxc.h"
#include "shaders/v2f_c4f_t2f_fxc.h"
#include "shaders/v2f_c4f_t2f_t2f_d28f_fxc.h"

using Microsoft::WRL::ComPtr;

namespace {

    // MG: host-provided logger replaces upstream's MessageBoxW.
    MgGpuLogFn g_log = nullptr;
    void Log(const char* msg)
    {
        if (g_log) g_log(msg);
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

    struct Uniforms {
        DirectX::XMFLOAT4 State;
        DirectX::XMMATRIX Transform;
        DirectX::XMFLOAT4 Scalar4[2];
        DirectX::XMFLOAT4 Vector[8];
        std::uint32_t ClipSize;
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
                batch_count_++;
            }
            command_list_.clear();
        }

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

        void CreateTexture(uint32_t texture_id, RefPtr<Bitmap> bitmap) override
        {
            auto i = textures_.find(texture_id);
            if (i != textures_.end()) {
                Log("MgGpu: CreateTexture, texture id already exists");
                return;
            }
            if (!(bitmap->format() == BitmapFormat::BGRA8_UNORM_SRGB
                  || bitmap->format() == BitmapFormat::A8_UNORM)) {
                Log("MgGpu: CreateTexture, unsupported format");
            }

            D3D11_TEXTURE2D_DESC desc;
            ZeroMemory(&desc, sizeof(desc));
            desc.Width = bitmap->width();
            desc.Height = bitmap->height();
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = bitmap->format() == BitmapFormat::BGRA8_UNORM_SRGB
                ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

            auto& entry = textures_[texture_id];
            HRESULT hr;

            if (bitmap->IsEmpty()) {
                // Render-target texture (a View's target, or one of Ultralight's layers). MG: with MSAA on,
                // Ultralight draws into msaa_texture and `texture` is its resolve target, so everything that
                // samples a driver texture (BindTexture, the host through GetTextureSRV) reads single-sample
                // pixels; upstream keeps a separate resolve texture and an unused multisample SRV instead.
                desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.CPUAccessFlags = 0;
                hr = context_->device()->CreateTexture2D(&desc, nullptr, entry.texture.GetAddressOf());
                const UINT samples = context_->sample_count();
                if (SUCCEEDED(hr) && samples > 1) {
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

        void UpdateTexture(uint32_t texture_id, RefPtr<Bitmap> bitmap) override
        {
            auto i = textures_.find(texture_id);
            if (i == textures_.end()) {
                Log("MgGpu: UpdateTexture, texture id doesn't exist");
                return;
            }
            auto& entry = i->second;
            D3D11_MAPPED_SUBRESOURCE res;
            context_->immediate_context()->Map(entry.texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &res);
            if (res.RowPitch == bitmap->row_bytes()) {
                std::memcpy(res.pData, bitmap->LockPixels(), bitmap->size());
                bitmap->UnlockPixels();
            } else {
                RefPtr<Bitmap> mapped_bitmap
                    = Bitmap::Create(bitmap->width(), bitmap->height(), bitmap->format(), res.RowPitch,
                                     res.pData, res.RowPitch * bitmap->height(), false);
                IntRect dest_rect = { 0, 0, (int)bitmap->width(), (int)bitmap->height() };
                mapped_bitmap->DrawBitmap(dest_rect, dest_rect, bitmap, false);
            }
            context_->immediate_context()->Unmap(entry.texture.Get(), 0);
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

            D3D11_RENDER_TARGET_VIEW_DESC rtv_desc;
            ZeroMemory(&rtv_desc, sizeof(rtv_desc));
            rtv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
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

        const char* name() override { return "Magelight D3D11"; }

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
        }

        void BindRenderBuffer(uint32_t render_buffer_id) override
        {
            ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
            context_->immediate_context()->PSSetShaderResources(0, 1, nullSRV);
            context_->immediate_context()->PSSetShaderResources(1, 1, nullSRV);
            context_->immediate_context()->PSSetShaderResources(2, 1, nullSRV);

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
            if (state.texture_1_id) BindTexture(0, state.texture_1_id);
            if (state.texture_2_id) BindTexture(1, state.texture_2_id);
            UpdateConstantBuffer(state);
            BindGeometry(geometry_id);

            auto* ctx = context_->immediate_context();
            auto sampler = GetSamplerState();
            ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
            BindShader(state.shader_type);

            if (state.enable_blend) context_->EnableBlend();
            else context_->DisableBlend();

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

            const D3D11_INPUT_ELEMENT_DESC layout_2f_4ub_2f[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };
            auto& shader_fill_path = shaders_[ShaderType::FillPath];
            LoadCompiledVertexShader(v2f_c4f_t2f_fxc, v2f_c4f_t2f_fxc_len,
                                     shader_fill_path.first.GetAddressOf(), layout_2f_4ub_2f,
                                     ARRAYSIZE(layout_2f_4ub_2f), vertex_layout_2f_4ub_2f_.GetAddressOf());
            LoadCompiledPixelShader(fill_path_fxc, fill_path_fxc_len, shader_fill_path.second.GetAddressOf());

            const D3D11_INPUT_ELEMENT_DESC layout_2f_4ub_2f_2f_28f[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
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
            auto& shader_fill = shaders_[ShaderType::Fill];
            LoadCompiledVertexShader(v2f_c4f_t2f_t2f_d28f_fxc, v2f_c4f_t2f_t2f_d28f_fxc_len,
                                     shader_fill.first.GetAddressOf(), layout_2f_4ub_2f_2f_28f,
                                     ARRAYSIZE(layout_2f_4ub_2f_2f_28f),
                                     vertex_layout_2f_4ub_2f_2f_28f_.GetAddressOf());
            LoadCompiledPixelShader(fill_fxc, fill_fxc_len, shader_fill.second.GetAddressOf());
        }

        void BindShader(ShaderType shader_type)
        {
            LoadShaders();
            auto& shader = shaders_[shader_type];
            context_->immediate_context()->VSSetShader(shader.first.Get(), nullptr, 0);
            context_->immediate_context()->PSSetShader(shader.second.Get(), nullptr, 0);
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
            UINT stride = geometry.format == VertexBufferFormat::_2f_4ub_2f
                ? sizeof(ultralight::Vertex_2f_4ub_2f) : sizeof(ultralight::Vertex_2f_4ub_2f_2f_28f);
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

            Uniforms uniforms;
            uniforms.State = { 0.0f, (float)state.viewport_width, (float)state.viewport_height, 1.0f };
            uniforms.Transform = DirectX::XMMATRIX(model_view_projection.GetMatrix4x4().data);
            uniforms.Scalar4[0] = { state.uniform_scalar[0], state.uniform_scalar[1],
                                    state.uniform_scalar[2], state.uniform_scalar[3] };
            uniforms.Scalar4[1] = { state.uniform_scalar[4], state.uniform_scalar[5],
                                    state.uniform_scalar[6], state.uniform_scalar[7] };
            for (size_t i = 0; i < 8; ++i)
                uniforms.Vector[i] = DirectX::XMFLOAT4(state.uniform_vector[i].x, state.uniform_vector[i].y,
                                                       state.uniform_vector[i].z, state.uniform_vector[i].w);
            uniforms.ClipSize = state.clip_size;
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
