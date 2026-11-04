// DSRPathfinderHook.dll: the in-game renderer. The app loads it into Dark Souls Remastered
// (see inworld.cpp) so the trail is drawn into the game's own image: in step with every frame,
// in fullscreen too, and dimmed where walls are in front of it, using the game's depth buffer.
//
// It hooks IDXGISwapChain::Present (and the context call that binds depth buffers) by
// patching their vtable entries, reads the trail the app shares (trailshare.h), reads the
// camera matrix the app located, and draws the motes on top of the finished frame. It changes
// nothing else in the game, and goes idle when the app isn't running.

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../camera.h"
#include "../motes.h"
#include "../trailshare.h"

namespace {

// ---- Vtable hooks ----------------------------------------------------------------------------

// Vtable slots (from the d3d11.h / dxgi.h interface order).
enum {
    kPresent = 8, kResizeBuffers = 13, kPresent1 = 22,
    kOMSetRenderTargets = 33, kOMSetRenderTargetsAndUAVs = 34,
};

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using OMSetRTFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                           ID3D11DepthStencilView*);
using OMSetRTUAVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                              ID3D11DepthStencilView*, UINT, UINT,
                                              ID3D11UnorderedAccessView* const*, const UINT*);

PresentFn oPresent;
Present1Fn oPresent1;
ResizeFn oResize;
OMSetRTFn oOMSetRT;
OMSetRTUAVFn oOMSetRTUAV;

// Points vtable slot `idx` at `fn`, keeping the old entry in `orig`. Skips a slot that already
// points at `fn` (the same class's vtable reached twice), so `orig` never loops back to us.
template <class F>
void patch(void* object, int idx, F fn, F& orig) {
    void** vt = *reinterpret_cast<void***>(object);
    if (vt[idx] == reinterpret_cast<void*>(fn)) return;
    DWORD old;
    if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return;
    orig = reinterpret_cast<F>(vt[idx]);
    vt[idx] = reinterpret_cast<void*>(fn);
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
}

// ---- Finding the depth buffer ------------------------------------------------------------------
//
// Every depth buffer the game binds during a frame, and how often. At Present the busiest one
// shaped like the screen is the scene's. (Binds, not draws: Direct3D rewrites its own draw
// entries in the vtable, so hooks on those don't stay.) The game may record its scene on
// deferred contexts from worker threads, so binds come from any thread; the list is locked.

ID3D11DeviceContext* g_ctx;  // the game's immediate context
bool g_ownDraw;              // our own calls on it, not to be counted
OMSetRTFn oOMSetRTDeferred;  // deferred contexts, when their vtable is a different one
OMSetRTUAVFn oOMSetRTUAVDeferred;

struct DepthUse {
    ID3D11DepthStencilView* dsv;  // referenced while in the list
    UINT binds;
};
SRWLOCK g_depthLock = SRWLOCK_INIT;
DepthUse g_uses[32];
int g_useCount;
volatile LONG g_bindCalls, g_bindDeferred, g_bindDepth, g_seenBound, g_repatched;  // for the status line

void noteDepth(ID3D11DepthStencilView* dsv) {
    AcquireSRWLockExclusive(&g_depthLock);
    int i = 0;
    while (i < g_useCount && g_uses[i].dsv != dsv) ++i;
    if (i < g_useCount) {
        ++g_uses[i].binds;
    } else if (g_useCount < 32) {
        dsv->AddRef();
        g_uses[g_useCount++] = {dsv, 1};
    }
    ReleaseSRWLockExclusive(&g_depthLock);
}

// The depth buffer bound on `ctx` right now, however it got there: Direct3D can route some of
// the game's binds past our hooks, but not past its own state.
void noteBound(ID3D11DeviceContext* ctx) {
    ID3D11DepthStencilView* cur = nullptr;
    ctx->OMGetRenderTargets(0, nullptr, &cur);
    if (!cur) return;
    InterlockedIncrement(&g_seenBound);
    noteDepth(cur);
    cur->Release();
}

void bindDepth(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv) {
    if (ctx == g_ctx && g_ownDraw) return;
    InterlockedIncrement(&g_bindCalls);
    if (ctx != g_ctx) InterlockedIncrement(&g_bindDeferred);
    if (!dsv) return;
    InterlockedIncrement(&g_bindDepth);
    noteDepth(dsv);
}

// A new frame: forget the old counts.
void resetDepthUses() {
    AcquireSRWLockExclusive(&g_depthLock);
    for (int i = 0; i < g_useCount; ++i) g_uses[i].dsv->Release();
    g_useCount = 0;
    ReleaseSRWLockExclusive(&g_depthLock);
}

void STDMETHODCALLTYPE hkOMSetRT(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtv,
                                 ID3D11DepthStencilView* dsv) {
    if (!g_ownDraw) noteBound(ctx);  // what this call replaces
    oOMSetRT(ctx, n, rtv, dsv);
    bindDepth(ctx, dsv);
}
void STDMETHODCALLTYPE hkOMSetRTUAV(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtv,
                                    ID3D11DepthStencilView* dsv, UINT slot, UINT nu,
                                    ID3D11UnorderedAccessView* const* uav, const UINT* counts) {
    if (!g_ownDraw) noteBound(ctx);
    oOMSetRTUAV(ctx, n, rtv, dsv, slot, nu, uav, counts);
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) bindDepth(ctx, dsv);
}
void STDMETHODCALLTYPE hkOMSetRTDeferred(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtv,
                                         ID3D11DepthStencilView* dsv) {
    oOMSetRTDeferred(ctx, n, rtv, dsv);
    bindDepth(ctx, dsv);
}
void STDMETHODCALLTYPE hkOMSetRTUAVDeferred(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtv,
                                            ID3D11DepthStencilView* dsv, UINT slot, UINT nu,
                                            ID3D11UnorderedAccessView* const* uav, const UINT* counts) {
    oOMSetRTUAVDeferred(ctx, n, rtv, dsv, slot, nu, uav, counts);
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) bindDepth(ctx, dsv);
}

// ---- Inline hooks ------------------------------------------------------------------------------
//
// Every context carries its own vtable (on the heap, inside the object), so patching one never
// reaches the game's deferred contexts. Their entries all lead to the same code in d3d11.dll,
// though, so that code is hooked instead: its first instructions become a jump to us, and a
// trampoline runs those instructions and jumps back for the original.

// Length of the instruction at `p` if it's one we can move safely (pushes, stack-relative movs
// and stack adjustments, as functions begin with), else 0.
int movableLength(const unsigned char* p) {
    if (p[0] >= 0x50 && p[0] <= 0x57) return 1;                                     // push reg
    if (p[0] >= 0x40 && p[0] <= 0x4F && p[1] >= 0x50 && p[1] <= 0x57) return 2;     // push r8-r15
    if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x89 && (p[2] & 0xC7) == 0x44 && p[3] == 0x24)
        return 5;                                                                   // mov [rsp+d8], reg
    if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC) return 4;                     // sub rsp, imm8
    if (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xEC) return 7;                     // sub rsp, imm32
    return 0;
}

// Memory for jump stubs within 2 GB of `close`, so a 5-byte relative jump reaches it.
unsigned char* allocNear(void* close, size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity, target = reinterpret_cast<uintptr_t>(close);
    uintptr_t lo = target > (1ull << 31) ? target - (1ull << 31) + gran : gran;
    for (uintptr_t at = (target - (1ull << 30)) & ~(gran - 1); at > lo; at -= gran) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof mbi)) break;
        if (mbi.State != MEM_FREE) continue;
        if (void* p = VirtualAlloc(reinterpret_cast<void*>(at), size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
            return static_cast<unsigned char*>(p);
    }
    return nullptr;
}

// Writes `len` bytes at `dst` with every other thread of the game paused, and none of them in
// the middle of those bytes, so nothing runs half-patched code.
bool writeWithThreadsPaused(unsigned char* dst, const unsigned char* src, size_t len) {
    std::vector<HANDLE> paused;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    paused.reserve(4096);  // no allocating while threads are paused: one might hold the heap lock
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    bool clear = true;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) == static_cast<DWORD>(-1)) {
            CloseHandle(t);
            continue;
        }
        if (paused.size() == paused.capacity()) {
            ResumeThread(t);
            CloseHandle(t);
            clear = false;
            break;
        }
        paused.push_back(t);
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(t, &c) && c.Rip >= reinterpret_cast<DWORD64>(dst) && c.Rip < reinterpret_cast<DWORD64>(dst) + len)
            clear = false;
    }
    CloseHandle(snap);
    DWORD old;
    bool ok = clear && VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &old);
    if (ok) {
        std::memcpy(dst, src, len);
        VirtualProtect(dst, len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), dst, len);
    }
    for (HANDLE t : paused) {
        ResumeThread(t);
        CloseHandle(t);
    }
    return ok;
}

// Hooks the code at `target`: calls go to `hook`, and `original` runs the function as it was.
template <class F>
bool detour(void* target, F hook, F& original) {
    auto* code = static_cast<unsigned char*>(target);
    if (code[0] == 0xE9) return false;  // already jumps somewhere: someone else's hook, leave it
    int moved = 0;
    while (moved < 5) {  // whole instructions covering the 5-byte jump
        int n = movableLength(code + moved);
        if (!n) return false;
        moved += n;
    }
    // [0] jump to the hook (14 bytes), [16] trampoline: the moved bytes, then a jump back.
    unsigned char* stub = allocNear(target, 64);
    if (!stub) return false;
    auto absJump = [](unsigned char* at, const void* to) {
        at[0] = 0xFF, at[1] = 0x25, at[2] = at[3] = at[4] = at[5] = 0;  // jmp [rip+0]
        std::memcpy(at + 6, &to, 8);
    };
    absJump(stub, reinterpret_cast<void*>(hook));
    std::memcpy(stub + 16, code, moved);
    absJump(stub + 16 + moved, code + moved);
    original = reinterpret_cast<F>(stub + 16);
    unsigned char jump[16] = {0xE9};
    int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(stub) - reinterpret_cast<intptr_t>(code + 5));
    std::memcpy(jump + 1, &rel, 4);
    for (int i = 5; i < moved; ++i) jump[i] = 0x90;  // nop out the rest of a moved instruction
    return writeWithThreadsPaused(code, jump, moved);
}

volatile LONG g_inlineHooks;  // for the status line

// Hooks the bind calls of the game's contexts: the immediate one through its vtable, and every
// deferred context through the d3d11.dll code their vtables lead to (found through a deferred
// context made and dropped here).
void hookContexts(ID3D11Device* dev, ID3D11DeviceContext* immediate) {
    patch(immediate, kOMSetRenderTargets, hkOMSetRT, oOMSetRT);
    patch(immediate, kOMSetRenderTargetsAndUAVs, hkOMSetRTUAV, oOMSetRTUAV);
    if (oOMSetRTDeferred) return;  // done for an earlier device
    ID3D11DeviceContext* deferred = nullptr;
    if (FAILED(dev->CreateDeferredContext(0, &deferred)) || !deferred) return;
    void** vt = *reinterpret_cast<void***>(deferred);
    void* setRT = vt[kOMSetRenderTargets];
    void* setRTUAV = vt[kOMSetRenderTargetsAndUAVs];
    deferred->Release();
    if (setRT != reinterpret_cast<void*>(oOMSetRT) && detour(setRT, hkOMSetRTDeferred, oOMSetRTDeferred))
        InterlockedIncrement(&g_inlineHooks);
    if (setRTUAV != reinterpret_cast<void*>(oOMSetRTUAV) && detour(setRTUAV, hkOMSetRTUAVDeferred, oOMSetRTUAVDeferred))
        InterlockedIncrement(&g_inlineHooks);
}

// Direct3D keeps the context's vtable on the heap and puts its own entries back into it (it
// does every frame for some calls): hook again if that happened to ours.
void keepContextHooks(ID3D11DeviceContext* immediate) {
    void** vt = *reinterpret_cast<void***>(immediate);
    if (vt[kOMSetRenderTargets] == reinterpret_cast<void*>(hkOMSetRT) &&
        vt[kOMSetRenderTargetsAndUAVs] == reinterpret_cast<void*>(hkOMSetRTUAV))
        return;
    InterlockedIncrement(&g_repatched);
    patch(immediate, kOMSetRenderTargets, hkOMSetRT, oOMSetRT);
    patch(immediate, kOMSetRenderTargetsAndUAVs, hkOMSetRTUAV, oOMSetRTUAV);
}

// Typeless family of a depth format, and how to read it in a shader.
bool depthFormats(DXGI_FORMAT f, DXGI_FORMAT& typeless, DXGI_FORMAT& srv) {
    switch (f) {
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
            typeless = DXGI_FORMAT_R24G8_TYPELESS;
            srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            return true;
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT:
            typeless = DXGI_FORMAT_R32_TYPELESS;
            srv = DXGI_FORMAT_R32_FLOAT;
            return true;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
            typeless = DXGI_FORMAT_R32G8X24_TYPELESS;
            srv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            return true;
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_TYPELESS:
            typeless = DXGI_FORMAT_R16_TYPELESS;
            srv = DXGI_FORMAT_R16_UNORM;
            return true;
        default: return false;
    }
}

// Runs `fn` on a thread of our own with a large stack, waiting for it. The game presents frames
// from a thread with a small stack, and compiling shaders or creating GPU objects (in Direct3D
// and the graphics driver) can need far more than it has left: done there, it overflows and the
// game crashes. The game's thread waits meanwhile, so the device is never used from two at once.
template <class F>
void onBigStack(F&& fn) {
    struct Job {
        F* fn;
        static DWORD WINAPI run(void* p) {
            (*static_cast<Job*>(p)->fn)();
            return 0;
        }
    } job{&fn};
    HANDLE t = CreateThread(nullptr, 8 << 20, &Job::run, &job, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!t) return;  // nothing is done rather than risk the small stack
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

// ---- The renderer ------------------------------------------------------------------------------

// Three passes, drawn in this order:
//   ribbon: a band of light along the route in world space. With the depth buffer it shows only
//           where something hides the path (a faint ghost through walls); without it, it's the
//           whole trail.
//   decal:  the stream painted onto the ground. Each pixel near the route rebuilds its position
//           in the world from the depth buffer and lights up if it lies on the path, so the light
//           follows stairs, slopes and uneven floors, and anything in front covers it.
//   motes:  round lights (every other style, and the sparks rising off the stream).
const char kShader[] = R"(
cbuffer C : register(b0) {
    float2 toNdc;      // 2 / backbuffer size
    float2 depthScale; // depth buffer size / backbuffer size
    float4 lin;        // depth = lin.y / (stored - lin.x); lin.z = depth on; lin.w = dim behind walls
    float4 eyeT;       // camera position; time in seconds
    float4 rightS;     // camera right; trail strength
    float4 upF;        // camera up; route distance at your feet
    float4 fwdE;       // camera forward; route distance at the destination
    float4 proj;       // x and y projection scale
    float4 vp;         // the game's image within the backbuffer: centre x, y, half width, height
};
Texture2D<float> sceneDepth : register(t0);

// Distance to the scene at a pixel, in metres (1e9 for the sky).
float sceneMetres(float2 px) {
    float z = sceneDepth.Load(int3(px * depthScale, 0));
    float den = z - lin.x;
    if (abs(den) < 1e-7) return 1e9;
    float s = lin.y / den;
    return s > 0 ? s : 1e9;
}
bool hidden(float2 px, float depth) { return sceneMetres(px) < depth - 0.3 - 0.03 * depth; }

float hash(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
float vnoise(float2 p) {
    float2 i = floor(p), f = frac(p), u = f * f * (3 - 2 * f);
    return lerp(lerp(hash(i), hash(i + float2(1, 0)), u.x), lerp(hash(i + float2(0, 1)), hash(i + float2(1, 1)), u.x), u.y);
}
float fbm(float2 p) { return 0.55 * vnoise(p) + 0.3 * vnoise(p * 2.03 + 7.1) + 0.15 * vnoise(p * 4.1 + 3.3); }

// The stream's light at route distance m, x metres across the path (premultiplied): a band of
// light about 70 cm wide with brighter edges, like a lit path laid on the ground. Wisps flow
// through it, waves of light run along it to the destination and chevrons drift forward. Its
// alpha tints the ground under it, so it reads on bright, busy floors too.
float4 pathLight(float m, float x) {
    const float W = 0.35;  // half width
    float t = eyeT.w, ax = abs(x);
    float body = 1 - smoothstep(W - 0.14, W, ax);
    float rim = exp(-pow((ax - (W - 0.06)) / 0.035, 2));
    float spill = exp(-pow(max(ax - W, 0) / 0.15, 2)) * (1 - body);
    float flow = fbm(float2(m * 0.6 - t * 1.2, x * 2.2 + 0.4 * sin(m * 0.25 + t * 0.7)));
    float w = 0.5 + 0.5 * cos(6.2832 * (m - t * 3.5) / 7.0);
    float pulse = w * w * w;
    float ch = frac((m - ax * 0.9) * 0.35 - t * 0.45);
    float chev = smoothstep(0.0, 0.05, ch) * (1 - smoothstep(0.10, 0.18, ch)) * body;
    float I = body * (0.12 + 0.16 * flow + 0.30 * pulse) + rim * (0.30 + 0.35 * pulse) + spill * 0.08 + chev * 0.22;
    float d = m - upF.w;
    // Clear around your character (it starts a couple of metres ahead), fading out far away.
    float f = smoothstep(1.5, 4.0, d) * (d < 30 ? 1 : saturate(1 - (d - 30) / 60)) *
              (1 - smoothstep(fwdE.w - 0.6, fwdE.w, m)) * rightS.w;
    float3 col = lerp(float3(1.0, 0.52, 0.20), float3(1.0, 0.90, 0.70), saturate(pulse * 0.6 + rim * 0.5));
    float a = saturate(body * (0.18 + 0.10 * pulse) + rim * 0.22);
    return float4(col * I * f, a * f);
}

// ---- motes: screen-space discs, p.x = brightness, p.y = depth, p.z = 0 disc / 1 ember tail
struct MV { float2 pos : POSITION; float2 uv : TEXCOORD0; float3 p : TEXCOORD1; };
struct MP { float4 pos : SV_Position; float2 uv : TEXCOORD0; float3 p : TEXCOORD1; };
MP vsMote(MV v) {
    MP o;
    o.pos = float4(v.pos.x * toNdc.x - 1, 1 - v.pos.y * toNdc.y, 0, 1);
    o.uv = v.uv;
    o.p = v.p;
    return o;
}
float4 psMote(MP i) : SV_Target {
    float3 c;
    float a;
    if (i.p.z < 0.5) {
        // Bonfire colours: a pale core in an ember glow and a wide faint halo. uv is in radii.
        float d = length(i.uv);
        float core = 0.98 * (1 - smoothstep(0.75, 1.05, d));
        float glow = 0.43 * exp(-d * d / 1.6);
        float halo = 0.19 * exp(-d * d / 5.5);
        c = float3(0.902, 0.667, 0.361) * halo; a = halo;
        c = float3(0.941, 0.761, 0.463) * glow + c * (1 - glow); a = glow + a * (1 - glow);
        c = float3(0.992, 0.925, 0.753) * core + c * (1 - core); a = core + a * (1 - core);
    } else {
        // uv.x runs 0 at the tail to 1 at the head, uv.y across.
        a = 0.47 * i.uv.x * (1 - smoothstep(0.4, 1.0, abs(i.uv.y)));
        c = float3(0.965, 0.824, 0.549) * a;
    }
    float k = saturate(i.p.x);
    if (lin.z > 0.5 && hidden(i.pos.xy, i.p.y)) k *= lin.w;
    return float4(c * k, a * k);  // premultiplied
}

// ---- ribbon: world positions, uv = (route distance, metres across)
struct RV { float3 w : POSITION; float2 uv : TEXCOORD0; };
struct RP { float4 pos : SV_Position; float2 uv : TEXCOORD0; float depth : TEXCOORD1; };
RP vsRibbon(RV v) {
    float3 d = v.w - eyeT.xyz;
    float3 c = float3(dot(d, rightS.xyz), dot(d, upF.xyz), dot(d, fwdE.xyz));
    RP o;
    // The projection onto the game's image (vp), written so the GPU clips at the near plane.
    o.pos = float4((vp.x * toNdc.x - 1) * c.z + vp.z * proj.x * toNdc.x * c.x,
                   (1 - vp.y * toNdc.y) * c.z + vp.w * proj.y * toNdc.y * c.y, c.z - 0.1, c.z);
    o.uv = v.uv;
    o.depth = c.z;
    return o;
}
float4 psRibbon(RP i) : SV_Target {
    float4 c = pathLight(i.uv.x, i.uv.y);
    if (lin.z > 0.5) {
        if (!hidden(i.pos.xy, i.depth)) discard;  // the decal paints it there
        c *= lin.w;
    }
    return c;
}

// ---- decal: a screen rectangle around one piece of the route (a to b, with its neighbours
// pr and nx) or, with e.z = 1, around the destination. e.xy = route distance at a and b.
struct DV { float2 pos : POSITION; float3 a : TEXCOORD0; float3 b : TEXCOORD1; float3 pr : TEXCOORD2; float3 nx : TEXCOORD3; float3 e : TEXCOORD4; };
struct DP { float4 pos : SV_Position; nointerpolation float3 a : TEXCOORD0; nointerpolation float3 b : TEXCOORD1;
            nointerpolation float3 pr : TEXCOORD2; nointerpolation float3 nx : TEXCOORD3; nointerpolation float3 e : TEXCOORD4; };
DP vsDecal(DV v) {
    DP o;
    o.pos = float4(v.pos.x * toNdc.x - 1, 1 - v.pos.y * toNdc.y, 0, 1);
    o.a = v.a; o.b = v.b; o.pr = v.pr; o.nx = v.nx; o.e = v.e;
    return o;
}
float segDist(float2 p, float2 a, float2 b, out float t) {
    float2 ab = b - a;
    float l = dot(ab, ab);
    t = l > 1e-6 ? saturate(dot(p - a, ab) / l) : 0;
    return length(p - (a + ab * t));
}
float4 psDecal(DP i) : SV_Target {
    float d = sceneMetres(i.pos.xy);
    float3 v = float3((i.pos.x - vp.x) / vp.z / proj.x * d, (vp.y - i.pos.y) / vp.w / proj.y * d, d);
    float3 X = eyeT.xyz + rightS.xyz * v.x + upF.xyz * v.y + fwdE.xyz * v.z;
    // Light on floors and stairs, not up the walls beside them.
    float3 n = cross(ddx(X), ddy(X));
    float flat = smoothstep(0.35, 0.75, abs(n.y) / max(length(n), 1e-6));
    if (d > 5e8) discard;
    float t = eyeT.w;
    if (i.e.z > 0.5) {
        // The destination: a flickering ring with waves running out from its centre.
        float2 dd = X.xz - i.a.xz;
        float r = length(dd), dy = X.y - i.a.y;
        float vert = (1 - smoothstep(0.5, 0.9, dy)) * (1 - smoothstep(0.9, 1.4, -dy));
        float ring = exp(-pow((r - 0.7 - 0.04 * sin(t * 2.0)) / 0.05, 2));
        ring *= 0.7 + 0.3 * fbm(float2(atan2(dd.y, dd.x) * 3 + t * 1.5, t * 0.5));
        float wave = frac(r * 0.7 - t * 0.45);
        float waves = smoothstep(0, 0.05, wave) * (1 - smoothstep(0.08, 0.2, wave)) * (1 - smoothstep(0.4, 1.5, r));
        float fill = exp(-r * r * 3.0) * 0.35;
        float I = (ring * 0.7 + waves * 0.3 + fill * 0.6) * vert * flat * max(rightS.w, 0.6);
        if (I < 0.003) discard;
        float3 col = lerp(float3(1.0, 0.50, 0.18), float3(1.0, 0.93, 0.78), saturate(ring));
        return float4(col * I, I * 0.45);
    }
    float tt, tp, tn;
    float dist = segDist(X.xz, i.a.xz, i.b.xz, tt);
    float dp = segDist(X.xz, i.pr.xz, i.a.xz, tp), dn = segDist(X.xz, i.b.xz, i.nx.xz, tn);
    // Each spot belongs to the nearest piece of the route, so the joins are seamless.
    if (dist > 0.6 || !(dist < dp && dist <= dn)) discard;
    float dy = X.y - lerp(i.a.y, i.b.y, tt);
    float vert = (1 - smoothstep(0.35, 0.6, dy)) * (1 - smoothstep(0.6, 0.9, -dy));
    float2 ab = i.b.xz - i.a.xz;
    float side = (ab.x * (X.z - i.a.z) - ab.y * (X.x - i.a.x)) < 0 ? -1 : 1;
    return pathLight(lerp(i.e.x, i.e.y, tt), dist * side) * vert * flat;
}
)";

struct MoteVertex {
    float x, y, u, v, bright, depth, kind;
};
struct RibbonVertex {
    float x, y, z, m, across;
};
struct DecalVertex {
    float x, y, a[3], b[3], pr[3], nx[3], e[3];
};
struct Constants {
    float toNdc[2], depthScale[2], lin[4];
    float eyeT[4], rightS[4], upF[4], fwdE[4], proj[4], vp[4];
};

// One pipeline: shaders, input layout and a dynamic vertex buffer.
struct Pass {
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, capacity = 0;

    template <class T>
    static void drop(T*& p) {
        if (p) p->Release();
        p = nullptr;
    }
    void drop() {
        drop(vs);
        drop(ps);
        drop(layout);
        drop(vb);
    }
    bool create(ID3D11Device* dev, const char* vsName, const char* psName, const D3D11_INPUT_ELEMENT_DESC* el,
                UINT elCount, UINT vertexSize, UINT maxVertices) {
        ID3DBlob *vsb = nullptr, *psb = nullptr;
        bool ok = SUCCEEDED(D3DCompile(kShader, sizeof kShader - 1, "trail", nullptr, nullptr, vsName, "vs_4_0", 0, 0, &vsb, nullptr)) &&
                  SUCCEEDED(D3DCompile(kShader, sizeof kShader - 1, "trail", nullptr, nullptr, psName, "ps_4_0", 0, 0, &psb, nullptr)) &&
                  SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs)) &&
                  SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps)) &&
                  SUCCEEDED(dev->CreateInputLayout(el, elCount, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout));
        drop(vsb);
        drop(psb);
        if (!ok) return false;
        D3D11_BUFFER_DESC bd{};
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.ByteWidth = vertexSize * maxVertices;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        stride = vertexSize;
        capacity = maxVertices;
        return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &vb));
    }
    // Uploads and draws `count` vertices.
    void draw(ID3D11DeviceContext* ctx, const void* data, size_t count) {
        if (!count) return;
        count = std::min<size_t>(count, capacity);
        D3D11_MAPPED_SUBRESOURCE map;
        if (FAILED(ctx->Map(vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
        std::memcpy(map.pData, data, count * stride);
        ctx->Unmap(vb, 0);
        UINT offset = 0;
        ctx->IASetInputLayout(layout);
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->Draw(static_cast<UINT>(count), 0);
    }
};

struct Renderer {
    ID3D11Device* dev = nullptr;
    Pass motes, ribbon, decal;
    ID3D11Buffer* cb = nullptr;
    ID3D11BlendState* blend = nullptr;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11DepthStencilState* noDepth = nullptr;
    // The backbuffer's view, and our copy of the scene depth.
    ID3D11Texture2D* rtTex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11Texture2D* depthTex = nullptr;
    ID3D11ShaderResourceView* depthSrv = nullptr;
    D3D11_TEXTURE2D_DESC depthDesc{};
    bool failed = false;

    template <class T>
    static void drop(T*& p) {
        if (p) p->Release();
        p = nullptr;
    }
    void dropTargets() {
        drop(rtv);
        rtTex = nullptr;
    }
    void dropAll() {
        dropTargets();
        drop(lastDsv);
        drop(probeTex);
        drop(depthSrv);
        drop(depthTex);
        motes.drop();
        ribbon.drop();
        decal.drop();
        drop(cb);
        drop(blend);
        drop(raster);
        drop(noDepth);
        drop(dev);
        failed = false;
    }

    bool init(ID3D11Device* d) {
        if (dev == d) return !failed;
        dropAll();
        dev = d;
        dev->AddRef();
        failed = true;
        const auto V = D3D11_INPUT_PER_VERTEX_DATA;
        D3D11_INPUT_ELEMENT_DESC moteEl[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, V, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, V, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 16, V, 0},
        };
        D3D11_INPUT_ELEMENT_DESC ribbonEl[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, V, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, V, 0},
        };
        D3D11_INPUT_ELEMENT_DESC decalEl[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, V, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 8, V, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20, V, 0},
            {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, V, 0},
            {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, V, 0},
            {"TEXCOORD", 4, DXGI_FORMAT_R32G32B32_FLOAT, 0, 56, V, 0},
        };
        if (!motes.create(dev, "vsMote", "psMote", moteEl, 3, sizeof(MoteVertex), 12 * 1400) ||
            !ribbon.create(dev, "vsRibbon", "psRibbon", ribbonEl, 2, sizeof(RibbonVertex), 6 * 400) ||
            !decal.create(dev, "vsDecal", "psDecal", decalEl, 6, sizeof(DecalVertex), 6 * 120))
            return false;

        D3D11_BUFFER_DESC bd{};
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.ByteWidth = sizeof(Constants);
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb))) return false;

        D3D11_BLEND_DESC bl{};
        auto& rt = bl.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                   D3D11_COLOR_WRITE_ENABLE_BLUE;  // leave the game's alpha alone
        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID;
        rs.CullMode = D3D11_CULL_NONE;
        rs.DepthClipEnable = TRUE;
        D3D11_DEPTH_STENCIL_DESC ds{};
        if (FAILED(dev->CreateBlendState(&bl, &blend)) || FAILED(dev->CreateRasterizerState(&rs, &raster)) ||
            FAILED(dev->CreateDepthStencilState(&ds, &noDepth)))
            return false;
        failed = false;
        return true;
    }

    bool target(IDXGISwapChain* sc) {
        ID3D11Texture2D* bb = nullptr;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return false;
        bb->Release();  // the swap chain keeps it alive
        if (bb == rtTex && rtv) return true;
        dropTargets();
        D3D11_TEXTURE2D_DESC td;
        bb->GetDesc(&td);
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rd.Format = td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS   ? DXGI_FORMAT_R8G8B8A8_UNORM
                    : td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM
                    : td.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS ? DXGI_FORMAT_R10G10B10A2_UNORM
                                                                    : td.Format;
        HRESULT hr = E_FAIL;
        onBigStack([&] { hr = dev->CreateRenderTargetView(bb, &rd, &rtv); });
        if (FAILED(hr)) return false;
        rtTex = bb;
        return true;
    }

    ID3D11DepthStencilView* lastDsv = nullptr;  // the scene's, as last found (referenced)
    ID3D11Texture2D* probeTex = nullptr;         // 1x1, for reading one depth value back

    // The stored depth (0..1) at pixel (x, y) of our depth copy. Stalls on the GPU: rarely.
    bool probeDepth(ID3D11DeviceContext* ctx, float x, float y, float& out) {
        if (!depthTex || x < 0 || y < 0 || x >= depthDesc.Width || y >= depthDesc.Height) return false;
        if (probeTex) {
            D3D11_TEXTURE2D_DESC pd;
            probeTex->GetDesc(&pd);
            if (pd.Format != depthDesc.Format) drop(probeTex);
        }
        if (!probeTex) {
            D3D11_TEXTURE2D_DESC td = depthDesc;
            td.Width = td.Height = 1;
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            HRESULT hr = E_FAIL;
            onBigStack([&] { hr = dev->CreateTexture2D(&td, nullptr, &probeTex); });
            if (FAILED(hr)) return false;
        }
        D3D11_BOX box{static_cast<UINT>(x), static_cast<UINT>(y), 0, static_cast<UINT>(x) + 1, static_cast<UINT>(y) + 1, 1};
        ctx->CopySubresourceRegion(probeTex, 0, 0, 0, 0, depthTex, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(probeTex, 0, D3D11_MAP_READ, 0, &m))) return false;
        uint32_t v;
        std::memcpy(&v, m.pData, 4);
        ctx->Unmap(probeTex, 0);
        switch (depthDesc.Format) {
            case DXGI_FORMAT_R24G8_TYPELESS: out = (v & 0xFFFFFF) / 16777215.0f; break;
            case DXGI_FORMAT_R16_TYPELESS: out = (v & 0xFFFF) / 65535.0f; break;
            default: std::memcpy(&out, &v, 4); break;  // R32 float first
        }
        return true;
    }

    // Copies the scene's depth buffer: the busiest one this frame shaped like the screen and made
    // by the game's device, or the last one found when nothing qualifying was bound this frame.
    bool copyDepth(ID3D11DeviceContext* ctx, UINT bbW, UINT bbH, char* info, size_t infoLen) {
        int best = -1;
        UINT bestBinds = 0;
        D3D11_TEXTURE2D_DESC bestDesc{};
        ID3D11Resource* bestRes = nullptr;
        AcquireSRWLockExclusive(&g_depthLock);
        const int seen = g_useCount;
        DepthUse uses[32];
        for (int i = 0; i < g_useCount; ++i) {
            uses[i] = g_uses[i];
            uses[i].dsv->AddRef();
        }
        ReleaseSRWLockExclusive(&g_depthLock);
        // The last one found competes too, so it's kept while the game doesn't rebind it.
        int n = seen;
        if (lastDsv) {
            int i = 0;
            while (i < n && uses[i].dsv != lastDsv) ++i;
            if (i == n && n < 32) {
                lastDsv->AddRef();
                uses[n++] = {lastDsv, 0};
            }
        }
        for (int i = 0; i < n; ++i) {
            if (best >= 0 && uses[i].binds <= bestBinds) continue;
            ID3D11Device* owner = nullptr;
            uses[i].dsv->GetDevice(&owner);
            if (owner) owner->Release();
            if (owner != dev) continue;  // an overlay's own device, not the game's
            ID3D11Resource* res = nullptr;
            uses[i].dsv->GetResource(&res);
            ID3D11Texture2D* tex = nullptr;
            if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
                D3D11_TEXTURE2D_DESC td;
                tex->GetDesc(&td);
                tex->Release();
                float aspect = static_cast<float>(td.Width) / td.Height, want = static_cast<float>(bbW) / bbH;
                DXGI_FORMAT tl, sv;
                if (td.SampleDesc.Count == 1 && td.ArraySize == 1 && td.Width * 2 >= bbW &&
                    std::fabs(aspect - want) < 0.02f * want && depthFormats(td.Format, tl, sv)) {
                    best = i;
                    bestBinds = uses[i].binds;
                    bestDesc = td;
                    if (bestRes) bestRes->Release();
                    bestRes = res;
                    res = nullptr;
                }
            }
            if (res) res->Release();
        }
        if (best >= 0 && uses[best].dsv != lastDsv) {
            if (lastDsv) lastDsv->Release();
            lastDsv = uses[best].dsv;
            lastDsv->AddRef();
        }
        for (int i = 0; i < n; ++i) uses[i].dsv->Release();
        if (best < 0) {
            snprintf(info, infoLen,
                     "no depth: %d seen; binds %ld (%ld deferred, %ld depth, %ld bound), inline %ld",
                     seen, g_bindCalls, g_bindDeferred, g_bindDepth, g_seenBound, g_inlineHooks);
            return false;
        }
        DXGI_FORMAT typeless, srvFormat;
        depthFormats(bestDesc.Format, typeless, srvFormat);
        if (!depthTex || depthDesc.Width != bestDesc.Width || depthDesc.Height != bestDesc.Height ||
            depthDesc.Format != typeless) {
            drop(depthSrv);
            drop(depthTex);
            D3D11_TEXTURE2D_DESC td{};
            td.Width = bestDesc.Width;
            td.Height = bestDesc.Height;
            td.MipLevels = td.ArraySize = 1;
            td.Format = typeless;
            td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = srvFormat;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            bool made = false;
            onBigStack([&] {
                made = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &depthTex)) &&
                       SUCCEEDED(dev->CreateShaderResourceView(depthTex, &sd, &depthSrv));
            });
            if (!made) {
                drop(depthTex);
                bestRes->Release();
                snprintf(info, infoLen, "couldn't copy the depth buffer");
                return false;
            }
            depthDesc = td;
        }
        ctx->CopySubresourceRegion(depthTex, 0, 0, 0, 0, bestRes, 0, nullptr);
        bestRes->Release();
        snprintf(info, infoLen, "depth %ux%u fmt %d x%u; binds %ld (%ld deferred, %ld depth), inline %ld",
                 bestDesc.Width, bestDesc.Height, static_cast<int>(bestDesc.Format), bestBinds, g_bindCalls, g_bindDeferred,
                 g_bindDepth, g_inlineHooks);
        return true;
    }
};

Renderer g_r;

// Everything of the game's pipeline state we touch, put back after drawing.
struct SavedState {
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4]{};
    UINT sampleMask = 0;
    ID3D11DepthStencilState* depth = nullptr;
    UINT stencilRef = 0;
    ID3D11RasterizerState* raster = nullptr;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_PRIMITIVE_TOPOLOGY topo{};
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, offset = 0;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* ds = nullptr;
    ID3D11Buffer* vsCb = nullptr;
    ID3D11Buffer* psCb = nullptr;
    ID3D11ShaderResourceView* psSrv = nullptr;

    void save(ID3D11DeviceContext* c) {
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
        c->OMGetBlendState(&blend, blendFactor, &sampleMask);
        c->OMGetDepthStencilState(&depth, &stencilRef);
        c->RSGetState(&raster);
        c->RSGetViewports(&vpCount, vp);
        c->IAGetPrimitiveTopology(&topo);
        c->IAGetInputLayout(&layout);
        c->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
        c->VSGetShader(&vs, nullptr, nullptr);
        c->PSGetShader(&ps, nullptr, nullptr);
        c->GSGetShader(&gs, nullptr, nullptr);
        c->HSGetShader(&hs, nullptr, nullptr);
        c->DSGetShader(&ds, nullptr, nullptr);
        c->VSGetConstantBuffers(0, 1, &vsCb);
        c->PSGetConstantBuffers(0, 1, &psCb);
        c->PSGetShaderResources(0, 1, &psSrv);
    }
    template <class T>
    static void rel(T*& p) {
        if (p) p->Release();
        p = nullptr;
    }
    void restore(ID3D11DeviceContext* c) {
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
        c->OMSetBlendState(blend, blendFactor, sampleMask);
        c->OMSetDepthStencilState(depth, stencilRef);
        c->RSSetState(raster);
        c->RSSetViewports(vpCount, vp);
        c->IASetPrimitiveTopology(topo);
        c->IASetInputLayout(layout);
        c->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        c->VSSetShader(vs, nullptr, 0);
        c->PSSetShader(ps, nullptr, 0);
        c->GSSetShader(gs, nullptr, 0);
        c->HSSetShader(hs, nullptr, 0);
        c->DSSetShader(ds, nullptr, 0);
        c->VSSetConstantBuffers(0, 1, &vsCb);
        c->PSSetConstantBuffers(0, 1, &psCb);
        c->PSSetShaderResources(0, 1, &psSrv);
        for (auto*& r : rtv) rel(r);
        rel(dsv);
        rel(blend);
        rel(depth);
        rel(raster);
        rel(layout);
        rel(vb);
        rel(vs);
        rel(ps);
        rel(gs);
        rel(hs);
        rel(ds);
        rel(vsCb);
        rel(psCb);
        rel(psSrv);
    }
};

// ---- Each frame ---------------------------------------------------------------------------------

HANDLE g_mapping;
trailshare::Block* g_block;
uint64_t g_nextOpenMs;
IDXGISwapChain* g_sc;  // the swap chain we draw on (the game's), not referenced
uint64_t g_scMs;
trailshare::Frame g_frame;
std::vector<Mote> g_motes;
std::vector<MoteVertex> g_verts;
std::vector<RibbonVertex> g_ribbon;
std::vector<DecalVertex> g_decal;
uint64_t g_infoMs;
bool g_inPresent;
LONG g_token;

bool openBlock(uint64_t now) {
    if (g_block) return true;
    if (now < g_nextOpenMs) return false;
    g_nextOpenMs = now + 1000;
    g_mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, trailshare::kMappingName);
    if (!g_mapping) return false;
    g_block = static_cast<trailshare::Block*>(MapViewOfFile(g_mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
    if (!g_block) return false;
    // Take over from an older copy of the renderer still loaded from an earlier version.
    g_token = static_cast<LONG>((GetTickCount64() ^ GetCurrentThreadId() << 16) | 1);
    InterlockedExchange(&g_block->owner, g_token);
    return true;
}

// Reads the game's memory safely: the camera may be freed between frames.
bool readOwn(uint64_t addr, void* out, size_t len) {
    SIZE_T got = 0;
    return addr && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, len, &got) && got == len;
}

float clockSec() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<float>(static_cast<double>(now.QuadPart % (freq.QuadPart * 3600)) / freq.QuadPart);
}

void setInfo(const char* text) {
    if (!g_block) return;
    strncpy_s(g_block->hookInfo, text, _TRUNCATE);
}

void onPresent(IDXGISwapChain* sc) {
    const uint64_t now = GetTickCount64();
    // Draw on one swap chain: the first that presents, until it stops.
    if (sc != g_sc) {
        if (g_sc && now - g_scMs < 2000) return;
        g_sc = sc;
        g_r.dropTargets();
    }
    g_scMs = now;

    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev)))) return;
    if (dev != g_r.dev) {
        if (g_ctx) g_ctx->Release();
        dev->GetImmediateContext(&g_ctx);
        resetDepthUses();
        // The context calls that tell us which depth buffer the scene uses.
        onBigStack([&] { hookContexts(dev, g_ctx); });
    } else {
        keepContextHooks(g_ctx);
    }
    bool ready = false;
    if (dev == g_r.dev) ready = !g_r.failed;
    else onBigStack([&] { ready = g_r.init(dev); });  // shader compiling: see onBigStack
    dev->Release();
    ID3D11DeviceContext* ctx = g_ctx;

    struct Reset {
        ~Reset() { resetDepthUses(); }
    } atEnd;
    if (!ready || !openBlock(now)) return;
    if (g_block->magic != trailshare::kMagic || g_block->version != trailshare::kVersion ||
        g_block->owner != g_token)
        return;
    g_block->hookVersion = trailshare::kVersion;
    // Alive, even if this one frame can't draw (mid-write, camera slot in use): the app must not
    // switch to its own trail for a frame or two, which shows as a flash of another style.
    g_block->hookMs = now;
    if (now - g_block->appMs > trailshare::kAppTimeoutMs) return;
    trailshare::Frame& f = g_frame;
    if (!trailshare::read(*g_block, f)) {
        f.draw = 0;  // mid-write; next frame
        return;
    }

    // The camera, read now so the trail moves exactly with this frame.
    float m[16], proj[16];
    Camera cam;
    if (!readOwn(f.camAddr, m, sizeof m) || !cameraFromMatrix(m, static_cast<uint8_t>(f.camKind), f.camSign, cam))
        return;
    cam.xs = f.xs;
    cam.ys = f.ys;
    // The projection: read now, or the app's last valid copy (the game reuses the slot in a frame).
    const char* projFrom = "game";
    bool haveProj = readOwn(f.projAddr, proj, sizeof proj) && projectionScale(proj, cam.xs, cam.ys);
    if (!haveProj && f.projValid) {
        std::memcpy(proj, f.proj, sizeof proj);
        haveProj = projectionScale(proj, cam.xs, cam.ys);
        projFrom = "app";
    }
    // Near and far don't change during play: once known, keep using them for the depth.
    static float lastProj[16];
    static bool haveLast = false;
    if (haveProj) {
        std::memcpy(lastProj, proj, sizeof proj);
        haveLast = true;
    } else if (haveLast) {
        std::memcpy(proj, lastProj, sizeof proj);
        haveProj = true;  // cam.xs and ys stay the app's current ones
        projFrom = "kept";
    }
    if (!f.draw || f.count < 2) return;

    DXGI_SWAP_CHAIN_DESC scd;
    if (FAILED(sc->GetDesc(&scd)) || !g_r.target(sc)) return;
    const UINT w = scd.BufferDesc.Width, h = scd.BufferDesc.Height;
    if (!w || !h) return;

    // Depth buffer (for dimming what's behind walls) and how to turn its values into metres.
    char info[160] = "occlusion off";
    float lin[4] = {0, 0, 0, 0.22f};
    noteBound(ctx);  // whatever the frame ended with
    if (f.occlusion && haveProj) {
        bool rowVec = std::fabs(std::fabs(proj[11]) - 1) < 1e-4f;
        lin[0] = rowVec ? proj[10] * proj[11] : proj[10] * proj[14];
        lin[1] = rowVec ? proj[14] : proj[11];
        if (g_r.copyDepth(ctx, w, h, info, sizeof info)) lin[2] = 1;
    } else if (f.occlusion) {
        snprintf(info, sizeof info, "occlusion needs the game's projection");
    }

    // Projection onto the game's image, the same way as the overlay window does it (overlay.cpp).
    std::vector<Vec3> trail(f.count);
    for (uint32_t i = 0; i < f.count; ++i) trail[i] = {f.points[i][0], f.points[i][1], f.points[i][2]};
    const float t = clockSec();
    const float aspect = cam.ys / cam.xs;
    float vw = static_cast<float>(w), vh = static_cast<float>(h);
    if (vw / vh > aspect) vw = vh * aspect;
    else vh = vw / aspect;
    const float cx = w / 2.0f, cy = h / 2.0f, hw = vw / 2, hh = vh / 2;
    const float s = std::clamp(h / 1080.0f, 0.6f, 2.5f), focal = hh * cam.ys, kNear = 0.4f;
    auto screen = [&](const Vec3& v, float& x, float& y) {
        x = cx + hw * cam.xs * v.x / v.z;
        y = cy - hh * cam.ys * v.y / v.z;
    };
    const bool stream = f.style == kMoteStream, depthOn = lin[2] > 0;
    RouteSampler route(trail, f.trailBase);
    const float feet = route.feet(), end = route.end();
    const float reach = std::min(end, feet + 90);  // the light has faded out by then

    // The stream: a ribbon (ghost behind walls, or the whole trail without depth) and, with
    // depth, the decal pieces painted onto the ground.
    g_ribbon.clear();
    g_decal.clear();
    if (stream) {
        Vec3 prevL{}, prevR{};
        float prevM = 0;
        bool have = false;
        for (float at = feet + 0.5f;; at += 0.5f) {
            at = std::min(at, reach);
            Vec3 p, dir;
            if (!route.sample(at, p, dir)) break;
            Vec3 side{dir.z, 0, -dir.x};
            float sl = length(side);
            side = sl > 1e-3f ? side * (0.55f / sl) : Vec3{0.55f, 0, 0};
            p = p + Vec3{0, 0.1f, 0};
            Vec3 L = p - side, R = p + side;
            if (have && g_ribbon.size() + 6 <= g_r.ribbon.capacity) {
                RibbonVertex a{prevL.x, prevL.y, prevL.z, prevM, -0.55f}, b{prevR.x, prevR.y, prevR.z, prevM, 0.55f},
                    c{L.x, L.y, L.z, at, -0.55f}, d{R.x, R.y, R.z, at, 0.55f};
                g_ribbon.insert(g_ribbon.end(), {a, b, c, b, d, c});
            }
            prevL = L;
            prevR = R;
            prevM = at;
            have = true;
            if (at >= reach) break;
        }

        if (depthOn) {
            // Screen rectangle around a box in the world; the whole image when it reaches behind you.
            auto rect = [&](const Vec3& lo, const Vec3& hi, float r[4]) {
                r[0] = r[1] = 1e9f;
                r[2] = r[3] = -1e9f;
                for (int k = 0; k < 8; ++k) {
                    Vec3 c{k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z};
                    Vec3 v = cam.toView(c);
                    if (v.z < 0.3f) {
                        r[0] = 0, r[1] = 0, r[2] = static_cast<float>(w), r[3] = static_cast<float>(h);
                        return true;
                    }
                    float x, y;
                    screen(v, x, y);
                    r[0] = std::min(r[0], x), r[1] = std::min(r[1], y), r[2] = std::max(r[2], x), r[3] = std::max(r[3], y);
                }
                r[0] = std::max(r[0] - 2, 0.0f), r[1] = std::max(r[1] - 2, 0.0f);
                r[2] = std::min(r[2] + 2, static_cast<float>(w)), r[3] = std::min(r[3] + 2, static_cast<float>(h));
                return r[0] < r[2] && r[1] < r[3];
            };
            auto piece = [&](const Vec3& a, const Vec3& b, const Vec3& pr, const Vec3& nx, float ma, float mb, float kind,
                             float pad) {
                Vec3 lo{std::min(a.x, b.x) - pad, std::min(a.y, b.y) - 1.4f, std::min(a.z, b.z) - pad};
                Vec3 hi{std::max(a.x, b.x) + pad, std::max(a.y, b.y) + 0.9f, std::max(a.z, b.z) + pad};
                float r[4];
                if (g_decal.size() + 6 > g_r.decal.capacity || !rect(lo, hi, r)) return;
                DecalVertex v{0, 0, {a.x, a.y, a.z}, {b.x, b.y, b.z}, {pr.x, pr.y, pr.z}, {nx.x, nx.y, nx.z}, {ma, mb, kind}};
                DecalVertex c[4] = {v, v, v, v};
                c[0].x = r[0], c[0].y = r[1], c[1].x = r[2], c[1].y = r[1];
                c[2].x = r[0], c[2].y = r[3], c[3].x = r[2], c[3].y = r[3];
                g_decal.insert(g_decal.end(), {c[0], c[1], c[2], c[1], c[3], c[2]});
            };
            // The route in pieces of 1.5 m, each knowing its neighbours.
            std::vector<Vec3> pts;
            std::vector<float> ms;
            Vec3 p, dir;
            for (float at = feet;; at += 1.5f) {
                at = std::min(at, reach);
                if (!route.sample(at, p, dir)) break;
                pts.push_back(p);
                ms.push_back(at);
                if (at >= reach) break;
            }
            for (size_t i = 0; i + 1 < pts.size(); ++i)
                piece(pts[i], pts[i + 1], i ? pts[i - 1] : pts[i], i + 2 < pts.size() ? pts[i + 2] : pts[i + 1], ms[i],
                      ms[i + 1], 0, 1.0f);
            if (f.endsAtGoal) {
                const Vec3& g = trail.back();
                piece(g, g, g, g, end, end, 1, 1.6f);
            }
        }
    }

    // Round lights: the style's motes, or the stream's sparks.
    buildMotes(trail, f.trailBase, t, f.strength, static_cast<int>(f.style), g_motes);
    struct Placed {
        const Mote* m;
        Vec3 view;
    };
    std::vector<Placed> placed;
    placed.reserve(g_motes.size());
    for (const Mote& mo : g_motes) {
        Vec3 v = cam.toView(mo.pos);
        if (v.z >= kNear) placed.push_back({&mo, v});
    }
    std::sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b) { return a.view.z > b.view.z; });

    g_verts.clear();
    auto quad = [&](const MoteVertex& a, const MoteVertex& b, const MoteVertex& c, const MoteVertex& d) {  // a b / c d
        if (g_verts.size() + 6 > g_r.motes.capacity) return;
        g_verts.insert(g_verts.end(), {a, b, c, b, d, c});
    };
    const float margin = 40 * s;
    for (const Placed& p : placed) {
        const Mote& mo = *p.m;
        float sx, sy;
        screen(p.view, sx, sy);
        if (sx < -margin || sx > w + margin || sy < -margin || sy > h + margin) continue;
        float r = moteRadius(focal, p.view.z, mo.size, s), b = std::clamp(mo.bright, 0.0f, 1.0f), z = p.view.z;
        if (mo.hasTail) {
            Vec3 tv = cam.toView(mo.pos + mo.tail);
            float tx, ty;
            if (tv.z >= kNear) {
                screen(tv, tx, ty);
                float dx = sx - tx, dy = sy - ty, len = std::hypot(dx, dy);
                if (len > 1) {
                    float nx = -dy / len * r * 0.55f, ny = dx / len * r * 0.55f;
                    quad({tx - nx, ty - ny, 0, -1, b, z, 1}, {tx + nx, ty + ny, 0, 1, b, z, 1},
                         {sx - nx, sy - ny, 1, -1, b, z, 1}, {sx + nx, sy + ny, 1, 1, b, z, 1});
                }
            }
        }
        const float e = 4 * r;  // the halo reaches about four radii
        quad({sx - e, sy - e, -4, -4, b, z, 0}, {sx + e, sy - e, 4, -4, b, z, 0},
             {sx - e, sy + e, -4, 4, b, z, 0}, {sx + e, sy + e, 4, 4, b, z, 0});
    }

    if (now - g_infoMs > 250) {
        g_infoMs = now;
        // Self-check: the depth buffer at the path 5 m ahead, in metres, next to how far that spot
        // really is. They match when the projection and the depth maths are right.
        char check[160] = "";
        if (haveProj) {
            float fovDeg = 2 * std::atan(1 / cam.ys) * 57.2958f, A = lin[0], B = lin[1];
            float n0 = std::fabs(A) > 1e-6f ? -B / A : 0, n1 = std::fabs(1 - A) > 1e-6f ? B / (1 - A) : 0;
            Vec3 ahead = trail[0], dir;
            route.sample(std::min(feet + 5, end), ahead, dir);
            Vec3 at = cam.toView(ahead + Vec3{0, 0.05f, 0});
            float stored = -1;
            if (depthOn && at.z > kNear) {
                float px, py;
                screen(at, px, py);
                g_r.probeDepth(ctx, px * g_r.depthDesc.Width / w, py * g_r.depthDesc.Height / h, stored);
            }
            float said = stored >= 0 && std::fabs(stored - A) > 1e-7f ? B / (stored - A) : -1;
            snprintf(check, sizeof check,
                     "\nproj fov %.1f (%s), depth 0 = %.2f m, 1 = %.1f m\npath 5 m on: %.1f m away, depth says %.1f m (%.5f)",
                     fovDeg, projFrom, n0, n1, at.z, said, stored);
        }
        char line[sizeof g_block->hookInfo];
        snprintf(line, sizeof line, "%s %zu pieces, %zu motes; %s%s", stream ? "stream" : "motes",
                 g_decal.size() / 6, g_verts.size() / 6, info, check);
        setInfo(line);
    }
    if (g_verts.empty() && g_ribbon.empty() && g_decal.empty()) return;

    D3D11_MAPPED_SUBRESOURCE map;
    g_ownDraw = true;
    SavedState saved;
    saved.save(ctx);
    if (SUCCEEDED(ctx->Map(g_r.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        Constants c{{2.0f / w, 2.0f / h},
                    {g_r.depthTex ? static_cast<float>(g_r.depthDesc.Width) / w : 1,
                     g_r.depthTex ? static_cast<float>(g_r.depthDesc.Height) / h : 1},
                    {lin[0], lin[1], lin[2], lin[3]},
                    {cam.eye.x, cam.eye.y, cam.eye.z, t},
                    {cam.right.x, cam.right.y, cam.right.z, f.strength},
                    {cam.up.x, cam.up.y, cam.up.z, feet},
                    {cam.fwd.x, cam.fwd.y, cam.fwd.z, end},
                    {cam.xs, cam.ys, 0, 0},
                    {cx, cy, hw, hh}};
        std::memcpy(map.pData, &c, sizeof c);
        ctx->Unmap(g_r.cb, 0);

        ID3D11ShaderResourceView* noSrv = nullptr;
        ctx->PSSetShaderResources(0, 1, &noSrv);  // in case the depth copy is bound as input
        ctx->OMSetRenderTargets(1, &g_r.rtv, nullptr);
        D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(g_r.raster);
        const float zero[4] = {};
        ctx->OMSetBlendState(g_r.blend, zero, 0xFFFFFFFF);
        ctx->OMSetDepthStencilState(g_r.noDepth, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &g_r.cb);
        ctx->PSSetConstantBuffers(0, 1, &g_r.cb);
        ID3D11ShaderResourceView* srv = depthOn ? g_r.depthSrv : nullptr;
        ctx->PSSetShaderResources(0, 1, &srv);
        g_r.ribbon.draw(ctx, g_ribbon.data(), g_ribbon.size());
        g_r.decal.draw(ctx, g_decal.data(), g_decal.size());
        g_r.motes.draw(ctx, g_verts.data(), g_verts.size());
    }
    saved.restore(ctx);
    g_ownDraw = false;
}

HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST) && !g_inPresent) {
        g_inPresent = true;
        onPresent(sc);
        g_inPresent = false;
    }
    return oPresent(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (!(flags & DXGI_PRESENT_TEST) && !g_inPresent) {
        g_inPresent = true;
        onPresent(sc);
        g_inPresent = false;
    }
    return oPresent1(sc, sync, flags, p);
}

HRESULT STDMETHODCALLTYPE hkResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags) {
    // Our view of the backbuffer has to go, or resizing fails.
    if (sc == g_sc) g_r.dropTargets();
    return oResize(sc, n, w, h, f, flags);
}

// Finds the swap chain's vtable through a throwaway device and window, then hooks it.
DWORD WINAPI install(void*) {
    while (!GetModuleHandleW(L"d3d11.dll") || !GetModuleHandleW(L"dxgi.dll")) Sleep(200);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DSRPathfinderHookProbe";
    RegisterClassExW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC d{};
    d.BufferCount = 1;
    d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BufferDesc.Width = d.BufferDesc.Height = 8;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.OutputWindow = wnd;
    d.SampleDesc.Count = 1;
    d.Windowed = TRUE;
    d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, &sc, &dev,
                                           nullptr, nullptr);
        if (SUCCEEDED(hr)) break;
    }
    if (SUCCEEDED(hr)) {
        patch(sc, kResizeBuffers, hkResize, oResize);
        patch(sc, kPresent, hkPresent, oPresent);
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1)))) {
            patch(sc1, kPresent1, hkPresent1, oPresent1);
            sc1->Release();
        }
        sc->Release();
        dev->Release();
    }
    DestroyWindow(wnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Stay loaded for the life of the game: the hooks point into this DLL.
        HMODULE self;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&DllMain), &self);
        DisableThreadLibraryCalls(inst);
        if (HANDLE t = CreateThread(nullptr, 0, install, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
