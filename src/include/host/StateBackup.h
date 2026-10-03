#pragma once
// The D3D state save/restore pair — stage 4 of the split.
//
// The host draws inside somebody else’s frame. Every draw we make — the
// overlay composite, the VR copy pass, the fullscreen UI-pass carrier — happens on the
// game’s own ID3D11DeviceContext, between its own draws, inside its Present.
// D3D11 has no command list and no scoping: setting a shader, a render target or
// a blend state MUTATES the context, and the game will draw the next frame with
// whatever we left behind.
//
// So we bracket every one of our draws: Capture() reads the entire pipeline state
// the game had set, we draw, Restore() puts every one of those values back. The
// game’s renderer must never notice we were here.
//
// This is a HEADER because it is the one piece of D3D code both halves of the
// render path need — Frame.cpp draws through it and the UI-pass composite
// publishes through it, and they are separate files.
//
// host/Render.h gives us the shared context and the includes above it.
#include "host/Render.h"

namespace Magelight {

    struct StateBackup {
        D3D11_PRIMITIVE_TOPOLOGY topo{};
        ID3D11InputLayout* layout = nullptr;
        ID3D11Buffer* vb = nullptr; UINT vbStride = 0, vbOffset = 0;
        ID3D11VertexShader* vs = nullptr;
        ID3D11PixelShader* ps = nullptr;
        ID3D11GeometryShader* gs = nullptr;
        ID3D11HullShader* hs = nullptr;
        ID3D11DomainShader* ds = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        ID3D11SamplerState* sampler = nullptr;
        ID3D11Buffer* psCb = nullptr;
        ID3D11BlendState* blend = nullptr; FLOAT blendFactor[4]{}; UINT sampleMask = 0;
        ID3D11DepthStencilState* depth = nullptr; UINT stencilRef = 0;
        ID3D11RasterizerState* rs = nullptr;
        ID3D11RenderTargetView* rtvs[8]{}; ID3D11DepthStencilView* dsv = nullptr;
        ID3D11ComputeShader* cs = nullptr;
        ID3D11Buffer* soTargets[4]{};
        ID3D11Predicate* predicate = nullptr; BOOL predicateValue = FALSE;
        UINT vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};

        void Capture(ID3D11DeviceContext* c)
        {
            c->IAGetPrimitiveTopology(&topo);
            c->IAGetInputLayout(&layout);
            c->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
            c->VSGetShader(&vs, nullptr, nullptr);
            c->PSGetShader(&ps, nullptr, nullptr);
            c->GSGetShader(&gs, nullptr, nullptr);
            c->HSGetShader(&hs, nullptr, nullptr);
            c->DSGetShader(&ds, nullptr, nullptr);
            c->PSGetShaderResources(0, 1, &srv);
            c->PSGetSamplers(0, 1, &sampler);
            c->PSGetConstantBuffers(0, 1, &psCb);
            c->OMGetBlendState(&blend, blendFactor, &sampleMask);
            c->OMGetDepthStencilState(&depth, &stencilRef);
            c->RSGetState(&rs);
            c->OMGetRenderTargets(8, rtvs, &dsv);
            c->RSGetViewports(&vpCount, vps);
            c->CSGetShader(&cs, nullptr, nullptr);
            c->SOGetTargets(4, soTargets);
            c->GetPredication(&predicate, &predicateValue);
        }
        // Put the pipeline in the state Ultralight's reference driver takes
        // for granted (AppCore owns a pristine context; we borrow the game's
        // mid-frame). The driver rebinds VS/PS/IA/RT/blend/rasterizer per
        // draw but never touches GS/HS/DS, depth-stencil, stream-output or
        // predication ΓÇö any of which the game may leave armed. Field
        // symptom (2026-09-01): path-tessellated borders (a box whose sides
        // differ) drew as huge wedge triangles in-game while the same page
        // rendered correctly through AppCore on the same SDK.
        static void Neutralize(ID3D11DeviceContext* c)
        {
            c->GSSetShader(nullptr, nullptr, 0);
            c->HSSetShader(nullptr, nullptr, 0);
            c->DSSetShader(nullptr, nullptr, 0);
            c->CSSetShader(nullptr, nullptr, 0);
            c->OMSetDepthStencilState(nullptr, 0);
            ID3D11Buffer* noSO[4] = {};
            UINT          soOff[4] = {};
            c->SOSetTargets(4, noSO, soOff);
            c->SetPredication(nullptr, FALSE);
        }
        void Restore(ID3D11DeviceContext* c)
        {
            c->IASetPrimitiveTopology(topo);
            c->IASetInputLayout(layout);
            c->IASetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
            c->VSSetShader(vs, nullptr, 0);
            c->PSSetShader(ps, nullptr, 0);
            c->GSSetShader(gs, nullptr, 0);
            c->HSSetShader(hs, nullptr, 0);
            c->DSSetShader(ds, nullptr, 0);
            c->PSSetShaderResources(0, 1, &srv);
            c->PSSetSamplers(0, 1, &sampler);
            c->PSSetConstantBuffers(0, 1, &psCb);
            c->OMSetBlendState(blend, blendFactor, sampleMask);
            c->OMSetDepthStencilState(depth, stencilRef);
            c->RSSetState(rs);
            c->CSSetShader(cs, nullptr, 0);
            UINT soOff[4] = {};
            c->SOSetTargets(4, soTargets, soOff);
            c->SetPredication(predicate, predicateValue);
            c->OMSetRenderTargets(8, rtvs, dsv);
            if (vpCount) c->RSSetViewports(vpCount, vps);
            // Release the refs the Get* calls added.
            if (layout) layout->Release();
            if (vb) vb->Release();
            if (vs) vs->Release();
            if (ps) ps->Release();
            if (gs) gs->Release();
            if (hs) hs->Release();
            if (ds) ds->Release();
            if (srv) srv->Release();
            if (sampler) sampler->Release();
            if (psCb) psCb->Release();
            if (blend) blend->Release();
            if (depth) depth->Release();
            if (rs) rs->Release();
            for (auto* r : rtvs) if (r) r->Release();
            if (dsv) dsv->Release();
            if (cs) cs->Release();
            for (auto* t : soTargets) if (t) t->Release();
            if (predicate) predicate->Release();
        }
    };
}  // namespace Magelight
