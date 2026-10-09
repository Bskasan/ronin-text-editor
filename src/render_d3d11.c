// render_d3d11.c — D3D11 renderer: one instanced-quad pipeline, flip-model swap chain,
// glyph atlas with ClearType through dual-source blending.

#define COBJMACROS
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#if TEAL_DEV
#include <dxgidebug.h>
#endif

#include "quad_vs.h" // build\gen, from shaders\quad.hlsl
#include "quad_ps.h"

#define R_MAX_INSTANCES 65536

typedef struct RInstance {
    f32 rect[4];
    f32 uv[4];
    Color color;
    u32 kind;
} RInstance;

_Static_assert(sizeof(RInstance) == 40, "instance layout must match the input layout");

enum { R_KIND_SOLID = 0, R_KIND_GLYPH = 1 };

struct Renderer {
    HWND hwnd;
    i32 width, height;
    b32 minimized;
    b32 occluded;
    b32 wants_redraw;
    b32 in_frame;
    Rect clip; // quads are cut to it (r_set_clip); the whole target after r_begin_frame
    u32 present_interval;

    // Results of r_create_device. It may run on a worker thread, where it must not log, so the
    // main thread reports these afterwards.
    HRESULT create_hr_debug; // the debug-layer attempt (dev builds)
    HRESULT create_hr;       // the final attempt
    b32 create_warp;
    b32 debug_layer;
    D3D_FEATURE_LEVEL feature_level;
    i64 create_us;

    ID3D11Device *device;
    ID3D11DeviceContext *context;
    IDXGISwapChain1 *swap_chain;
    ID3D11RenderTargetView *rtv;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11InputLayout *input_layout;
    ID3D11Buffer *instance_buffer;
    ID3D11Buffer *constant_buffer;
    ID3D11BlendState *blend;
    ID3D11RasterizerState *raster;

    RInstance *instances; // CPU staging, R_MAX_INSTANCES
    i32 instance_count;

    // Glyph atlas. The CPU copy belongs to the font module.
    u8 *atlas_pixels;
    i32 atlas_w, atlas_h;
    ID3D11Texture2D *atlas_texture;
    ID3D11ShaderResourceView *atlas_srv;
    i32 dirty_x0, dirty_y0, dirty_x1, dirty_y1; // empty when x0 >= x1

#if TEAL_DEV
    ID3D11InfoQueue *info_queue;
    u32 message_count;
    b32 capture_requested;
    ID3D11Texture2D *capture_texture;
    i32 capture_width, capture_height;
    RDevFrameStats stats; // the last r_end_frame
#endif
};

#define R_RELEASE(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

// ---------------------------------------------------------------------------
// Dev: debug-layer messages

#if TEAL_DEV
static void r_drain_messages(Renderer *r) {
    if (!r->info_queue) return;
    UINT64 count = ID3D11InfoQueue_GetNumStoredMessages(r->info_queue);
    for (UINT64 i = 0; i < count; i++) {
        SIZE_T size = 0;
        ID3D11InfoQueue_GetMessage(r->info_queue, i, NULL, &size);
        u8 buffer[4096];
        if (size > sizeof(buffer)) {
            LOG("d3d11: message too long (%U bytes)", (u64)size);
            r->message_count++;
            continue;
        }
        D3D11_MESSAGE *m = (D3D11_MESSAGE *)buffer;
        if (FAILED(ID3D11InfoQueue_GetMessage(r->info_queue, i, m, &size))) continue;
        static const char *severity[] = { "CORRUPTION", "ERROR", "WARNING", "INFO", "MESSAGE" };
        const char *sev = (u32)m->Severity < ARRAY_COUNT(severity) ? severity[m->Severity] : "?";
        LOG("d3d11 %s #%d: %S", sev, (i32)m->ID, str8((u8 *)m->pDescription, (i64)m->DescriptionByteLength - 1));
        if (m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) r->message_count++;
    }
    ID3D11InfoQueue_ClearStoredMessages(r->info_queue);
}
#endif

// ---------------------------------------------------------------------------
// Creation / destruction

Renderer *r_alloc(Arena *perm) {
    Renderer *r = PUSH_STRUCT(perm, Renderer);
    r->instances = PUSH_ARRAY(perm, RInstance, R_MAX_INSTANCES);
    r->present_interval = 1;
    return r;
}

// May run on a worker thread: no logging, no arenas, no os_*; results go into r only.
b32 r_create_device(Renderer *r) {
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;
    HRESULT hr = E_FAIL;
    r->create_hr_debug = S_OK;
    r->create_warp = 0;
    r->debug_layer = 0;

#if TEAL_D3D_DEBUG
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags | D3D11_CREATE_DEVICE_DEBUG,
                           levels, ARRAY_COUNT(levels), D3D11_SDK_VERSION, &r->device, &r->feature_level, &r->context);
    r->create_hr_debug = hr;
    r->debug_layer = SUCCEEDED(hr);
#endif
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
                               levels, ARRAY_COUNT(levels), D3D11_SDK_VERSION, &r->device, &r->feature_level, &r->context);
    }
    if (FAILED(hr)) {
        r->create_warp = 1;
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, flags,
                               levels, ARRAY_COUNT(levels), D3D11_SDK_VERSION, &r->device, &r->feature_level, &r->context);
    }
    r->create_hr = hr;

#if TEAL_DEV
    if (r->debug_layer &&
        SUCCEEDED(ID3D11Device_QueryInterface(r->device, &IID_ID3D11InfoQueue, (void **)&r->info_queue))) {
        // Breaking without a debugger attached would just crash the process and lose the
        // message, so only break under a debugger; otherwise the message is logged and counted.
        if (IsDebuggerPresent()) {
            ID3D11InfoQueue_SetBreakOnSeverity(r->info_queue, D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            ID3D11InfoQueue_SetBreakOnSeverity(r->info_queue, D3D11_MESSAGE_SEVERITY_ERROR, TRUE);
        }
    }
#endif

    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    r->create_us = (t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart;
    return SUCCEEDED(hr);
}

static void r_log_device_result(Renderer *r) {
#if TEAL_DEV
    if (FAILED(r->create_hr_debug)) {
        LOG("d3d11: debug device creation failed (0x%x), retried without debug layer", (u32)r->create_hr_debug);
    }
    if (r->create_warp) LOG("d3d11: hardware device creation failed, fell back to WARP");
    if (FAILED(r->create_hr)) {
        LOG("d3d11: device creation failed (0x%x)", (u32)r->create_hr);
    } else {
        LOG("d3d11: device created in %D us, feature level 0x%x, debug layer %s", r->create_us,
            (u32)r->feature_level, r->debug_layer ? "on" : "off");
    }
#else
    (void)r;
#endif
}

static b32 r_create_swap_chain(Renderer *r) {
    IDXGIDevice1 *dxgi_device = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory2 *factory = NULL;
    b32 ok = 0;

    if (FAILED(ID3D11Device_QueryInterface(r->device, &IID_IDXGIDevice1, (void **)&dxgi_device))) goto done;
    IDXGIDevice1_SetMaximumFrameLatency(dxgi_device, 1);
    if (FAILED(IDXGIDevice1_GetAdapter(dxgi_device, &adapter))) goto done;
    if (FAILED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory))) goto done;

    DXGI_SWAP_CHAIN_DESC1 desc = {
        .Width = (UINT)MAX(r->width, 1),
        .Height = (UINT)MAX(r->height, 1),
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .SampleDesc = { 1, 0 },
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = 2,
        .Scaling = DXGI_SCALING_NONE,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .AlphaMode = DXGI_ALPHA_MODE_IGNORE,
    };
    HRESULT hr = IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)r->device, r->hwnd, &desc,
                                                      NULL, NULL, &r->swap_chain);
    if (FAILED(hr)) {
        LOG("dxgi: CreateSwapChainForHwnd failed (0x%x)", (u32)hr);
        goto done;
    }
    IDXGIFactory2_MakeWindowAssociation(factory, r->hwnd, DXGI_MWA_NO_ALT_ENTER);
    ok = 1;

done:
    R_RELEASE(factory);
    R_RELEASE(adapter);
    R_RELEASE(dxgi_device);
    return ok;
}

static b32 r_create_target(Renderer *r) {
    ID3D11Texture2D *back_buffer = NULL;
    HRESULT hr = IDXGISwapChain1_GetBuffer(r->swap_chain, 0, &IID_ID3D11Texture2D, (void **)&back_buffer);
    if (FAILED(hr)) return 0;
    hr = ID3D11Device_CreateRenderTargetView(r->device, (ID3D11Resource *)back_buffer, NULL, &r->rtv);
    R_RELEASE(back_buffer);
    return SUCCEEDED(hr);
}

// Dual-source blending: the pixel shader outputs the color in SV_Target0 and per-channel
// weights in SV_Target1 (alpha for solid quads, coverage.rgb * alpha for glyphs), so
// ClearType blends each channel separately: out = color * w + dst * (1 - w).
static b32 r_create_blend_state(Renderer *r) {
    D3D11_BLEND_DESC desc = {0};
    desc.RenderTarget[0].BlendEnable = TRUE;
    desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC1_COLOR;
    desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC1_COLOR;
    desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC1_ALPHA;
    desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC1_ALPHA;
    desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return SUCCEEDED(ID3D11Device_CreateBlendState(r->device, &desc, &r->blend));
}

static b32 r_create_pipeline(Renderer *r) {
    if (FAILED(ID3D11Device_CreateVertexShader(r->device, quad_vs_bytes, sizeof(quad_vs_bytes), NULL, &r->vs))) return 0;
    if (FAILED(ID3D11Device_CreatePixelShader(r->device, quad_ps_bytes, sizeof(quad_ps_bytes), NULL, &r->ps))) return 0;

    D3D11_INPUT_ELEMENT_DESC elements[] = {
        { "RECT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(RInstance, rect),  D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "UV",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(RInstance, uv),    D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM,     0, offsetof(RInstance, color), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "KIND",  0, DXGI_FORMAT_R32_UINT,           0, offsetof(RInstance, kind),  D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };
    if (FAILED(ID3D11Device_CreateInputLayout(r->device, elements, ARRAY_COUNT(elements),
                                              quad_vs_bytes, sizeof(quad_vs_bytes), &r->input_layout))) return 0;

    D3D11_BUFFER_DESC ib = {
        .ByteWidth = sizeof(RInstance) * R_MAX_INSTANCES,
        .Usage = D3D11_USAGE_DYNAMIC,
        .BindFlags = D3D11_BIND_VERTEX_BUFFER,
        .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
    };
    if (FAILED(ID3D11Device_CreateBuffer(r->device, &ib, NULL, &r->instance_buffer))) return 0;

    D3D11_BUFFER_DESC cb = {
        .ByteWidth = 16,
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
    };
    if (FAILED(ID3D11Device_CreateBuffer(r->device, &cb, NULL, &r->constant_buffer))) return 0;

    if (!r_create_blend_state(r)) return 0;

    D3D11_RASTERIZER_DESC rs = {
        .FillMode = D3D11_FILL_SOLID,
        .CullMode = D3D11_CULL_NONE,
        .DepthClipEnable = TRUE,
    };
    if (FAILED(ID3D11Device_CreateRasterizerState(r->device, &rs, &r->raster))) return 0;
    return 1;
}

// (Re)creates the atlas texture for the bound CPU copy and schedules a full upload.
static b32 r_create_atlas_texture(Renderer *r) {
    R_RELEASE(r->atlas_srv);
    R_RELEASE(r->atlas_texture);
    if (!r->atlas_pixels) return 1;
    D3D11_TEXTURE2D_DESC desc = {
        .Width = (UINT)r->atlas_w,
        .Height = (UINT)r->atlas_h,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = { 1, 0 },
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE,
    };
    if (FAILED(ID3D11Device_CreateTexture2D(r->device, &desc, NULL, &r->atlas_texture))) return 0;
    if (FAILED(ID3D11Device_CreateShaderResourceView(r->device, (ID3D11Resource *)r->atlas_texture, NULL, &r->atlas_srv))) return 0;
    r_atlas_mark_dirty(r, 0, 0, r->atlas_w, r->atlas_h);
    return 1;
}

// Everything after the device: swap chain, render target, pipeline, atlas texture.
static b32 r_create_device_resources(Renderer *r) {
    return r_create_swap_chain(r) && r_create_target(r) && r_create_pipeline(r) && r_create_atlas_texture(r);
}

b32 r_finish_create(Renderer *r, void *native_window, i32 width, i32 height) {
    r->hwnd = (HWND)native_window;
    r->width = width;
    r->height = height;
    r_log_device_result(r);
    if (!r->device || !r_create_device_resources(r)) return 0;
    return 1;
}

// Releases everything except the device and its debug interfaces.
static void r_release_resources(Renderer *r) {
    if (r->context) {
        ID3D11DeviceContext_ClearState(r->context);
        ID3D11DeviceContext_Flush(r->context);
    }
#if TEAL_DEV
    R_RELEASE(r->capture_texture);
#endif
    R_RELEASE(r->atlas_srv);
    R_RELEASE(r->atlas_texture);
    R_RELEASE(r->raster);
    R_RELEASE(r->blend);
    R_RELEASE(r->constant_buffer);
    R_RELEASE(r->instance_buffer);
    R_RELEASE(r->input_layout);
    R_RELEASE(r->ps);
    R_RELEASE(r->vs);
    R_RELEASE(r->rtv);
    R_RELEASE(r->swap_chain);
    R_RELEASE(r->context);
}

// Returns the device's reference count after our final Release (0 = nothing leaked).
static ULONG r_release_all(Renderer *r) {
    r_release_resources(r);
#if TEAL_DEV
    r_drain_messages(r);
    R_RELEASE(r->info_queue);
#endif
    ULONG refs = 0;
    if (r->device) {
        refs = ID3D11Device_Release(r->device);
        r->device = NULL;
    }
    return refs;
}

u32 r_shutdown(Renderer *r) {
    ULONG device_refs = r_release_all(r);
    u32 leaks = (u32)device_refs;
#if TEAL_DEV
    LOG("d3d11: device refcount after final release: %u", (u32)device_refs);
    // Everything is released now, so any object DXGI still tracks is a leak.
    IDXGIDebug1 *dxgi_debug = NULL;
    IDXGIInfoQueue *dxgi_queue = NULL;
    if (SUCCEEDED(DXGIGetDebugInterface1(0, &IID_IDXGIInfoQueue, (void **)&dxgi_queue)) &&
        SUCCEEDED(DXGIGetDebugInterface1(0, &IID_IDXGIDebug1, (void **)&dxgi_debug))) {
        IDXGIInfoQueue_ClearStoredMessages(dxgi_queue, DXGI_DEBUG_ALL);
        IDXGIDebug1_ReportLiveObjects(dxgi_debug, DXGI_DEBUG_ALL,
                                      DXGI_DEBUG_RLO_DETAIL | DXGI_DEBUG_RLO_IGNORE_INTERNAL);
        UINT64 count = IDXGIInfoQueue_GetNumStoredMessages(dxgi_queue, DXGI_DEBUG_ALL);
        for (UINT64 i = 0; i < count; i++) {
            SIZE_T size = 0;
            IDXGIInfoQueue_GetMessage(dxgi_queue, DXGI_DEBUG_ALL, i, NULL, &size);
            u8 buffer[4096];
            if (size > sizeof(buffer)) continue;
            DXGI_INFO_QUEUE_MESSAGE *m = (DXGI_INFO_QUEUE_MESSAGE *)buffer;
            if (SUCCEEDED(IDXGIInfoQueue_GetMessage(dxgi_queue, DXGI_DEBUG_ALL, i, m, &size))) {
                LOG("dxgi live object: %S", str8((u8 *)m->pDescription, (i64)m->DescriptionByteLength - 1));
            }
        }
        LOG("dxgi: %U live-object report message(s)", (u64)count);
        leaks += (u32)count;
    } else {
        LOG("dxgi: debug interface unavailable, live-object report skipped");
    }
    R_RELEASE(dxgi_debug);
    R_RELEASE(dxgi_queue);
#endif
    return leaks;
}

// ---------------------------------------------------------------------------
// Device loss

static void r_handle_device_lost(Renderer *r, HRESULT hr) {
    (void)hr; // only logged
    LOG("d3d11: device lost (0x%x, reason 0x%x), recreating", (u32)hr,
        r->device ? (u32)ID3D11Device_GetDeviceRemovedReason(r->device) : 0u);
    r_release_all(r);
    r_create_device(r);
    r_log_device_result(r);
    if (!r->device || !r_create_device_resources(r)) {
        os_fatal(STR8_LIT("Could not recreate the Direct3D device after it was lost."));
    }
    r->wants_redraw = 1;
}

// ---------------------------------------------------------------------------
// Atlas

void r_atlas_bind(Renderer *r, u8 *pixels, i32 width, i32 height) {
    r->atlas_pixels = pixels;
    r->atlas_w = width;
    r->atlas_h = height;
    r->dirty_x0 = r->dirty_x1 = 0;
    if (!r_create_atlas_texture(r)) os_fatal(STR8_LIT("Could not create the glyph atlas texture."));
}

void r_atlas_mark_dirty(Renderer *r, i32 x0, i32 y0, i32 x1, i32 y1) {
    if (x0 >= x1 || y0 >= y1) return;
    if (r->dirty_x0 >= r->dirty_x1) {
        r->dirty_x0 = x0;
        r->dirty_y0 = y0;
        r->dirty_x1 = x1;
        r->dirty_y1 = y1;
    } else {
        r->dirty_x0 = MIN(r->dirty_x0, x0);
        r->dirty_y0 = MIN(r->dirty_y0, y0);
        r->dirty_x1 = MAX(r->dirty_x1, x1);
        r->dirty_y1 = MAX(r->dirty_y1, y1);
    }
}

static void r_upload_atlas(Renderer *r) {
    if (r->dirty_x0 >= r->dirty_x1 || !r->atlas_texture) return;
    D3D11_BOX box = { (UINT)r->dirty_x0, (UINT)r->dirty_y0, 0, (UINT)r->dirty_x1, (UINT)r->dirty_y1, 1 };
#if TEAL_DEV
    r->stats.upload_bytes += (u64)(r->dirty_x1 - r->dirty_x0) * (u64)(r->dirty_y1 - r->dirty_y0) * 4;
#endif
    u8 *src = r->atlas_pixels + ((i64)r->dirty_y0 * r->atlas_w + r->dirty_x0) * 4;
    ID3D11DeviceContext_UpdateSubresource(r->context, (ID3D11Resource *)r->atlas_texture, 0, &box, src,
                                          (UINT)r->atlas_w * 4, 0);
    r->dirty_x0 = r->dirty_x1 = 0;
}

// ---------------------------------------------------------------------------
// Frame

void r_resize(Renderer *r, i32 width, i32 height) {
    r->minimized = (width <= 0 || height <= 0);
    if (r->minimized || (width == r->width && height == r->height)) return;
    r->width = width;
    r->height = height;

    ID3D11DeviceContext_OMSetRenderTargets(r->context, 0, NULL, NULL);
    R_RELEASE(r->rtv);
    HRESULT hr = IDXGISwapChain1_ResizeBuffers(r->swap_chain, 0, (UINT)width, (UINT)height, DXGI_FORMAT_UNKNOWN, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        r_handle_device_lost(r, hr);
        return;
    }
    if (FAILED(hr) || !r_create_target(r)) os_fatal(STR8_LIT("Could not resize the swap chain."));
}

b32 r_wants_redraw(Renderer *r) {
    return r->wants_redraw;
}

void r_begin_frame(Renderer *r, Color clear) {
    r->wants_redraw = 0;
    r->instance_count = 0;
    r->clip = (Rect){ 0, 0, (f32)r->width, (f32)r->height };
    r->in_frame = !r->minimized;
    if (!r->in_frame) return;

    // The flip model unbinds the back buffer on every Present, and nothing else is
    // guaranteed to survive either, so bind the whole pipeline every frame.
    ID3D11DeviceContext *c = r->context;
    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &r->rtv, NULL);
    D3D11_VIEWPORT viewport = { 0, 0, (f32)r->width, (f32)r->height, 0, 1 };
    ID3D11DeviceContext_RSSetViewports(c, 1, &viewport);
    ID3D11DeviceContext_RSSetState(c, r->raster);
    ID3D11DeviceContext_OMSetBlendState(c, r->blend, NULL, 0xFFFFFFFF);

    f32 globals[4] = { (f32)r->width, (f32)r->height, 0, 0 };
    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)r->constant_buffer, 0, NULL, globals, 0, 0);

    UINT stride = sizeof(RInstance), offset = 0;
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_IASetInputLayout(c, r->input_layout);
    ID3D11DeviceContext_IASetVertexBuffers(c, 0, 1, &r->instance_buffer, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(c, r->vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(c, 0, 1, &r->constant_buffer);
    ID3D11DeviceContext_PSSetShader(c, r->ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &r->atlas_srv);

    f32 rgba[4] = { clear.r / 255.0f, clear.g / 255.0f, clear.b / 255.0f, clear.a / 255.0f };
    ID3D11DeviceContext_ClearRenderTargetView(c, r->rtv, rgba);
}

void r_flush(Renderer *r) {
    r_upload_atlas(r);
    if (r->instance_count == 0 || !r->in_frame) {
        r->instance_count = 0;
        return;
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ID3D11DeviceContext_Map(r->context, (ID3D11Resource *)r->instance_buffer, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, r->instances, sizeof(RInstance) * (size_t)r->instance_count);
        ID3D11DeviceContext_Unmap(r->context, (ID3D11Resource *)r->instance_buffer, 0);
        ID3D11DeviceContext_DrawInstanced(r->context, 4, (UINT)r->instance_count, 0, 0);
    }
    r->instance_count = 0;
}

void r_set_clip(Renderer *r, Rect clip) {
    r->clip = clip;
}

void r_push_rect(Renderer *r, Rect rect, Color color) {
    if (!r->in_frame) return;
    rect.x0 = MAX(rect.x0, r->clip.x0);
    rect.y0 = MAX(rect.y0, r->clip.y0);
    rect.x1 = MIN(rect.x1, r->clip.x1);
    rect.y1 = MIN(rect.y1, r->clip.y1);
    if (rect.x0 >= rect.x1 || rect.y0 >= rect.y1) return;
    if (r->instance_count == R_MAX_INSTANCES) r_flush(r);
    RInstance *inst = &r->instances[r->instance_count++];
    inst->rect[0] = rect.x0;
    inst->rect[1] = rect.y0;
    inst->rect[2] = rect.x1;
    inst->rect[3] = rect.y1;
    inst->uv[0] = inst->uv[1] = inst->uv[2] = inst->uv[3] = 0;
    inst->color = color;
    inst->kind = R_KIND_SOLID;
}

void r_push_glyph(Renderer *r, Rect dst, Rect atlas_texels, Color color) {
    if (!r->in_frame) return;
    // The quad is 1:1 with its texels: cutting it cuts its texel rectangle by the same amounts.
    f32 cut_x0 = MAX(r->clip.x0 - dst.x0, 0), cut_y0 = MAX(r->clip.y0 - dst.y0, 0);
    f32 cut_x1 = MAX(dst.x1 - r->clip.x1, 0), cut_y1 = MAX(dst.y1 - r->clip.y1, 0);
    if (cut_x0 + cut_x1 >= dst.x1 - dst.x0 || cut_y0 + cut_y1 >= dst.y1 - dst.y0) return;
    dst = (Rect){ dst.x0 + cut_x0, dst.y0 + cut_y0, dst.x1 - cut_x1, dst.y1 - cut_y1 };
    atlas_texels = (Rect){ atlas_texels.x0 + cut_x0, atlas_texels.y0 + cut_y0, atlas_texels.x1 - cut_x1, atlas_texels.y1 - cut_y1 };
    if (r->instance_count == R_MAX_INSTANCES) r_flush(r);
    RInstance *inst = &r->instances[r->instance_count++];
    inst->rect[0] = dst.x0;
    inst->rect[1] = dst.y0;
    inst->rect[2] = dst.x1;
    inst->rect[3] = dst.y1;
    inst->uv[0] = atlas_texels.x0;
    inst->uv[1] = atlas_texels.y0;
    inst->uv[2] = atlas_texels.x1;
    inst->uv[3] = atlas_texels.y1;
    inst->color = color;
    inst->kind = R_KIND_GLYPH;
}

void r_end_frame(Renderer *r) {
    if (!r->in_frame) return;
#if TEAL_DEV
    u64 t0 = os_time_us();
#endif
    r_flush(r);
    r->in_frame = 0;
#if TEAL_DEV
    u64 t1 = os_time_us();
    os_dev_stage("frame: flushed (atlas upload, draw calls)");
#endif

#if TEAL_DEV
    // FLIP_DISCARD leaves the back buffer undefined after Present, so copy it now.
    if (r->capture_requested) {
        r->capture_requested = 0;
        ID3D11Texture2D *back_buffer = NULL;
        if (SUCCEEDED(IDXGISwapChain1_GetBuffer(r->swap_chain, 0, &IID_ID3D11Texture2D, (void **)&back_buffer))) {
            D3D11_TEXTURE2D_DESC desc;
            ID3D11Texture2D_GetDesc(back_buffer, &desc);
            if (!r->capture_texture || r->capture_width != (i32)desc.Width || r->capture_height != (i32)desc.Height) {
                R_RELEASE(r->capture_texture);
                desc.Usage = D3D11_USAGE_STAGING;
                desc.BindFlags = 0;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                desc.MiscFlags = 0;
                if (SUCCEEDED(ID3D11Device_CreateTexture2D(r->device, &desc, NULL, &r->capture_texture))) {
                    r->capture_width = (i32)desc.Width;
                    r->capture_height = (i32)desc.Height;
                }
            }
            if (r->capture_texture) {
                ID3D11DeviceContext_CopyResource(r->context, (ID3D11Resource *)r->capture_texture,
                                                 (ID3D11Resource *)back_buffer);
            }
            R_RELEASE(back_buffer);
        }
    }
#endif

    HRESULT hr = IDXGISwapChain1_Present(r->swap_chain, r->present_interval, 0);
#if TEAL_DEV
    u64 t2 = os_time_us();
    os_dev_stage("frame: presented");
    r->stats.flush_us = t1 - t0;
    r->stats.present_us = t2 - t1;
#endif
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        r_handle_device_lost(r, hr);
    } else {
        // Occluded: nothing to do. Rendering is on demand, so there is no loop to throttle.
        r->occluded = (hr == DXGI_STATUS_OCCLUDED);
    }

#if TEAL_DEV
    r_drain_messages(r);
#endif
}

// ---------------------------------------------------------------------------
// Dev

#if TEAL_DEV
void r_request_capture(Renderer *r) {
    r->capture_requested = 1;
}

u8 *r_read_capture(Renderer *r, Arena *arena, i32 *width, i32 *height) {
    *width = *height = 0;
    if (!r->capture_texture) return NULL;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ID3D11DeviceContext_Map(r->context, (ID3D11Resource *)r->capture_texture, 0,
                                       D3D11_MAP_READ, 0, &mapped))) return NULL;
    i32 w = r->capture_width, h = r->capture_height;
    u8 *pixels = PUSH_ARRAY(arena, u8, (i64)w * h * 4);
    for (i32 y = 0; y < h; y++) {
        memcpy(pixels + (i64)y * w * 4, (u8 *)mapped.pData + (i64)y * mapped.RowPitch, (size_t)w * 4);
    }
    ID3D11DeviceContext_Unmap(r->context, (ID3D11Resource *)r->capture_texture, 0);
    *width = w;
    *height = h;
    return pixels;
}

b32 r_dev_debug_layer_active(Renderer *r) {
    return r->debug_layer;
}

u32 r_dev_message_count(Renderer *r) {
    return r->message_count;
}

void r_dev_set_present_interval(Renderer *r, u32 interval) {
    r->present_interval = interval;
}

// DWM composes the swap chain, or shows it directly (a hardware overlay plane / independent
// flip). Shown directly, the buffer on screen stays held until the next vertical blank, so with
// two buffers Present(0, 0) waits for that blank.
const char *r_dev_presentation_mode(Renderer *r) {
    IDXGISwapChainMedia *media = NULL;
    if (FAILED(IDXGISwapChain1_QueryInterface(r->swap_chain, &IID_IDXGISwapChainMedia, (void **)&media))) return "unknown";
    DXGI_FRAME_STATISTICS_MEDIA stats;
    HRESULT hr = IDXGISwapChainMedia_GetFrameStatisticsMedia(media, &stats);
    R_RELEASE(media);
    if (FAILED(hr)) return "unknown (no statistics yet)";
    switch (stats.CompositionMode) {
    case DXGI_FRAME_PRESENTATION_MODE_COMPOSED:            return "composed by DWM";
    case DXGI_FRAME_PRESENTATION_MODE_OVERLAY:             return "overlay (shown directly)";
    case DXGI_FRAME_PRESENTATION_MODE_NONE:                return "none";
    case DXGI_FRAME_PRESENTATION_MODE_COMPOSITION_FAILURE: return "composition failure";
    }
    return "?";
}

RDevFrameStats r_dev_take_frame_stats(Renderer *r) {
    RDevFrameStats s = r->stats;
    r->stats = (RDevFrameStats){ 0 };
    return s;
}
#endif
