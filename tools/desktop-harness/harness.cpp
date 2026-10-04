// Magelight desktop harness: OUR GPU driver (MagelightGPU.dll) + stock Ultralight
// runtime, no AppCore App. Mirrors the in-game frame pump (Update / RefreshDisplay
// / Render / MgGpu_DrawCommandList / composite the view RT onto a swapchain).
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <Ultralight/Ultralight.h>
#include <AppCore/Platform.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
using namespace ultralight;

typedef void (*MgGpuLogFn)(const char*);
typedef void* (*MgCreateFn)(ID3D11Device*, ID3D11DeviceContext*, MgGpuLogFn);
typedef void* (*MgGetDrvFn)(void*);
typedef int   (*MgHasFn)(void*);
typedef void  (*MgDrawFn)(void*);
typedef void* (*MgSrvFn)(void*, unsigned);
typedef int   (*MgSamplesFn)(void*, int);   // optional (MSAA)

static FILE* g_log = nullptr;
static void Log(const char* m) { if (g_log) { fprintf(g_log, "%s\n", m); fflush(g_log); } }

static const char* kShaderSrc =
    "struct VSIn  { float2 pos : POS; float2 uv : TEX; };\n"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut vs_main(VSIn i) { VSOut o; o.pos = float4(i.pos, 0.0, 1.0); o.uv = i.uv; return o; }\n"
    "Texture2D    tex0 : register(t0);\n"
    "SamplerState smp0 : register(s0);\n"
    "float4 ps_main(VSOut i) : SV_Target { return tex0.Sample(smp0, i.uv); }\n";
struct Vtx { float x, y, u, v; };

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv) {
    g_log = fopen("harness.log", "w");
    const int W = argc > 2 ? atoi(argv[2]) : 2560, Hh = argc > 3 ? atoi(argv[3]) : 1440; const int WW = 1280, WH = 720;
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"MlHarness";
    RegisterClassW(&wc);
    RECT r{ 0, 0, WW, WH }; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, L"MlHarness", L"mlharness", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 200,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Width = WW; sd.BufferDesc.Height = WH;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; IDXGISwapChain* sc = nullptr;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx))) { Log("device failed"); return 1; }
    Log("A device"); ID3D11Texture2D* back = nullptr; sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    ID3D11RenderTargetView* rtv = nullptr; dev->CreateRenderTargetView(back, nullptr, &rtv);

    ID3DBlob* vsb = nullptr; ID3DBlob* psb = nullptr; ID3DBlob* err = nullptr;
    D3DCompile(kShaderSrc, strlen(kShaderSrc), nullptr, nullptr, nullptr, "vs_main", "vs_4_0", 0, 0, &vsb, &err);
    D3DCompile(kShaderSrc, strlen(kShaderSrc), nullptr, nullptr, nullptr, "ps_main", "ps_4_0", 0, 0, &psb, &err);
    if (!vsb || !psb) { Log("shader compile failed"); return 4; }
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11InputLayout* il = nullptr;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
    D3D11_INPUT_ELEMENT_DESC ild[] = {
        { "POS", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEX", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
    dev->CreateInputLayout(ild, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il);
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(Vtx) * 4; bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* vb = nullptr; dev->CreateBuffer(&bd, nullptr, &vb);
    D3D11_SAMPLER_DESC sdesc{}; sdesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sdesc.AddressU = sdesc.AddressV = sdesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ID3D11SamplerState* samp = nullptr; dev->CreateSamplerState(&sdesc, &samp);
    D3D11_BLEND_DESC bld{}; bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD; bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11BlendState* blend = nullptr; dev->CreateBlendState(&bld, &blend);

    Log("C shaders"); Config cfg; cfg.cache_path = "./cache"; cfg.resource_path_prefix = "resources/"; cfg.face_winding = FaceWinding::Clockwise;
    Platform::instance().set_config(cfg);
    Platform::instance().set_font_loader(GetPlatformFontLoader());
    Platform::instance().set_file_system(GetPlatformFileSystem("./assets/"));
    Log("D platform"); HMODULE gpu = LoadLibraryA("MagelightGPU.dll");
    if (!gpu) { Log("MagelightGPU.dll load failed"); return 2; }
    auto mgCreate = (MgCreateFn)GetProcAddress(gpu, "MgGpu_Create");
    auto mgDrv = (MgGetDrvFn)GetProcAddress(gpu, "MgGpu_GetGPUDriver");
    auto mgHas = (MgHasFn)GetProcAddress(gpu, "MgGpu_HasCommandsPending");
    auto mgDraw = (MgDrawFn)GetProcAddress(gpu, "MgGpu_DrawCommandList");
    auto mgSrv = (MgSrvFn)GetProcAddress(gpu, "MgGpu_GetTextureSRV");
    void* h = mgCreate(dev, ctx, &Log);
    if (!h) { Log("MgGpu_Create failed"); return 3; }
    // MSAA as the host sets it (Magelight.json "msaa", default 4); MG_MSAA overrides it here (1 = off).
    if (auto mgSamples = (MgSamplesFn)GetProcAddress(gpu, "MgGpu_SetSampleCount")) {
        const char* env = getenv("MG_MSAA");
        char line[64]; snprintf(line, sizeof line, "MSAA %dx", mgSamples(h, env ? atoi(env) : 4)); Log(line);
    } else Log("driver predates MSAA");
    Log("E gpu created"); Platform::instance().set_gpu_driver((GPUDriver*)mgDrv(h)); Log("E2 driver set");
    RefPtr<Renderer> renderer = Renderer::Create();
    Log("F renderer"); ViewConfig vc; vc.is_accelerated = true; vc.is_transparent = true;
    RefPtr<View> view = renderer->CreateView(W, Hh, vc, nullptr);
    Log("G view"); view->LoadURL(argc > 1 ? argv[1] : "file:///sa/index.html");
    Log("running");

    MSG msg{}; int frame = 0;
    for (;;) {
        ++frame;
        if (frame == 150) view->EvaluateScript("(function(){var it=Array.from(document.querySelectorAll(\".sa-rail__item\")).find(function(e){return e.textContent.trim().indexOf(\"Inventory\")===0;}); if(it) it.click(); return it?\"ok\":\"no-item\";})()");
        if (frame == 260 || frame == 320) view->EvaluateScript("window.receivePageData(JSON.stringify({page:'inventory',actors:[{formId:20,name:'Prisoner',carryWeight:{current:12,max:300}}],selectedActorFormId:20,selectedActorName:'Prisoner',goldCount:0,categories:{},totalItems:3,totalWeight:12,totalValue:0,stats:{level:1,race:'Nord',sex:'Male',className:'Spellsword',health:100,magicka:100,stamina:100,perkCount:1,perkPoints:0,skills:{oneHanded:20,twoHanded:25,archery:15,block:20,heavyArmor:15,smithing:20,destruction:15,restoration:15,conjuration:15,alteration:15,enchanting:15,illusion:15,lightArmor:20,sneak:15,lockpicking:15,pickpocket:15,speech:20,alchemy:15}}}))");
        if (frame == 400) { FILE* f = fopen("inject.js", "rb"); if (f) { std::string js; char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) js.append(buf, n); fclose(f); view->EvaluateScript(js.c_str()); Log("inject.js evaluated"); } }
        if (frame == 230) view->EvaluateScript("(function(){var c=Array.from(document.querySelectorAll(\".sa-rail__child\")).find(function(e){return e.textContent.trim()===\"Stats\";}); if(c) c.click(); return c?\"ok\":\"no-child\";})()");
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) return 0; TranslateMessage(&msg); DispatchMessageW(&msg); }
        renderer->Update();
        renderer->RefreshDisplay(0);
        renderer->Render();
        if (mgHas(h)) mgDraw(h);
        const float clear[4] = { 0.10f, 0.09f, 0.07f, 1.0f };
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, clear);
        D3D11_VIEWPORT vp{ 0, 0, (float)WW, (float)WH, 0, 1 }; ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(nullptr); ctx->OMSetDepthStencilState(nullptr, 0);
        const float bf[4] = { 0, 0, 0, 0 }; ctx->OMSetBlendState(blend, bf, 0xFFFFFFFF);
        RenderTarget rt = view->render_target();
        auto* srv = (ID3D11ShaderResourceView*)mgSrv(h, rt.texture_id);
        if (srv) {
            Vtx q[4] = { { -1, 1, rt.uv_coords.left, rt.uv_coords.top }, { 1, 1, rt.uv_coords.right, rt.uv_coords.top },
                         { -1, -1, rt.uv_coords.left, rt.uv_coords.bottom }, { 1, -1, rt.uv_coords.right, rt.uv_coords.bottom } };
            D3D11_MAPPED_SUBRESOURCE m{}; ctx->Map(vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m); memcpy(m.pData, q, sizeof(q)); ctx->Unmap(vb, 0);
            UINT stride = sizeof(Vtx), off = 0;
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP); ctx->IASetInputLayout(il);
            ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
            ctx->VSSetShader(vs, nullptr, 0); ctx->PSSetShader(ps, nullptr, 0);
            ctx->GSSetShader(nullptr, nullptr, 0);
            ctx->PSSetShaderResources(0, 1, &srv); ctx->PSSetSamplers(0, 1, &samp);
            ctx->Draw(4, 0);
            ID3D11ShaderResourceView* nul = nullptr; ctx->PSSetShaderResources(0, 1, &nul);
        }
        sc->Present(1, 0);
    }
}









