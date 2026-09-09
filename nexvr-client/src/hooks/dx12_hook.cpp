#include "hooks/dx12_hook.h"
#include <system_error>
#include "hooks/dxgi_factory_hook.h"
#include "core/logger.h"
#include "core/seh_shield.h"
#include "MinHook.h"
// removed openxr_manager.h
#include "rendering/backends/dx12_renderer.h"
#include "core/config_manager.h"
#include "core/diagnostic_context.h"
#include "core/subsystem_context.h"
#include "heuristics/camera_lock_manager.h"
#include "hooks/input_hook.h"
#include "core/overlay_manager.h"
#include "rendering/dx12/imgui_dx12_integration.h"
#include "rendering/comfort_guard.h"
#include "heuristics/depth_candidate_collector.h"
#include "rendering/dx12/dx12_descriptor_tracker.h"
#include <mutex>
#include <shared_mutex>
#include "rendering/dx12/dx12_lifecycle_manager.h"
#include "rendering/vulkan/vulkan_lifecycle_manager.h"
#include "core/diagnostic_context.h"
#include "core/frame_coordinator.h"

#include <wrl/client.h>
#include <unordered_map>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "core/engine_scanners/universal_scanner.h"
#include <chrono>

extern HMODULE g_hModule;

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace vrinject {
namespace DX12Hook {

typedef void(__stdcall* ExecuteCommandLists_t)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(__stdcall* Present1_t)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef void(__stdcall* DrawIndexedInstanced_t)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
typedef HRESULT(__stdcall* Map_t)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**);
typedef void(__stdcall* Unmap_t)(ID3D12Resource*, UINT, const D3D12_RANGE*);
typedef void(__stdcall* OMSetRenderTargets_t)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
typedef void(__stdcall* ClearDepthStencilView_t)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*);

Map_t OriginalMap = nullptr;
Unmap_t OriginalUnmap = nullptr;
DrawIndexedInstanced_t OriginalDrawIndexedInstanced = nullptr;
ExecuteCommandLists_t OriginalExecuteCommandLists = nullptr;
OMSetRenderTargets_t OriginalOMSetRenderTargets = nullptr;
ClearDepthStencilView_t OriginalClearDepthStencilView = nullptr;

Present_t OriginalPresentDX12 = nullptr;
Present1_t OriginalPresent1DX12 = nullptr;

typedef HRESULT(__stdcall* ResizeBuffers_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
ResizeBuffers_t OriginalResizeBuffers = nullptr;

typedef HRESULT(__stdcall* ResizeBuffers1_t)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
ResizeBuffers1_t OriginalResizeBuffers1 = nullptr;

typedef void(__stdcall* CreateDepthStencilView_t)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
CreateDepthStencilView_t OriginalCreateDepthStencilView = nullptr;
static void* g_targetCreateDepthStencilView = nullptr;

typedef void(__stdcall* CopyDescriptors_t)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
CopyDescriptors_t OriginalCopyDescriptors = nullptr;
static void* g_targetCopyDescriptors = nullptr;

typedef void(__stdcall* CopyDescriptorsSimple_t)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
CopyDescriptorsSimple_t OriginalCopyDescriptorsSimple = nullptr;
static void* g_targetCopyDescriptorsSimple = nullptr;


static void* g_targetExecuteCommandLists = nullptr;
static void* g_targetDrawIndexedInstanced = nullptr;
static void* g_targetOMSetRenderTargets = nullptr;
static void* g_targetClearDepthStencilView = nullptr;
static void* g_targetPresentDX12 = nullptr;
static void* g_targetPresent1DX12 = nullptr;
static void* g_targetResizeBuffers = nullptr;
static void* g_targetResizeBuffers1 = nullptr;
static void* g_targetMapDX12 = nullptr;
static void* g_targetUnmapDX12 = nullptr;

std::unordered_map<ID3D12Resource*, void*> g_dx12MappedResources;
std::recursive_mutex g_dx12MapMutex;

// extern OpenXRManager g_openxrManager;
extern bool g_openxrInitialized;
void OnPresent(IDXGISwapChain* pSwapChain);

HRESULT __stdcall hkMap(ID3D12Resource* pResource, UINT Subresource, const D3D12_RANGE* pReadRange, void** ppData) {
    HRESULT hr = OriginalMap ? OriginalMap(pResource, Subresource, pReadRange, ppData) : E_FAIL;
    if (SUCCEEDED(hr) && ppData && *ppData && vrinject::seh::IsValidMemoryPointer(pResource)) {
        std::lock_guard<std::recursive_mutex> lock(g_dx12MapMutex);
        g_dx12MappedResources[pResource] = *ppData;
    }
    return hr;
}

static void* PopMappedResourcePtr(ID3D12Resource* pResource) {
    std::lock_guard<std::recursive_mutex> lock(g_dx12MapMutex);
    auto it = g_dx12MappedResources.find(pResource);
    if (it != g_dx12MappedResources.end()) {
        void* ptr = it->second;
        g_dx12MappedResources.erase(it);
        return ptr;
    }
    return nullptr;
}

void __stdcall hkUnmap(ID3D12Resource* pResource, UINT Subresource, const D3D12_RANGE* pWrittenRange) {
    if (vrinject::seh::IsValidMemoryPointer(pResource)) {
        void* pData = PopMappedResourcePtr(pResource);
        if (pData) {
            __try {
                D3D12_RESOURCE_DESC desc = pResource->GetDesc();
                if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
                    SubsystemContext::Get().GetUniversalScanner()->ProcessConstantBuffer(pData, static_cast<size_t>(desc.Width));
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    if (OriginalUnmap) {
        OriginalUnmap(pResource, Subresource, pWrittenRange);
    }
}
extern bool g_openxrInitialized;
IDXGISwapChain* g_mainSwapChain = nullptr;

void VerifyHookIntegrityDX12() {
    auto verify = [](void* target, const char* name) {
        if (!target) return;
        
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(target, &mbi, sizeof(mbi)) == 0 || 
            !(mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_READONLY | PAGE_READWRITE))) {
            return; // Not readable memory
        }

        uint8_t* code = reinterpret_cast<uint8_t*>(target);
        // MinHook detours start with relative JMP (0xE9), short JMP (0xEB), 
        // absolute JMP (0xFF 0x25 or 0xFF 0x15), or 64-bit absolute JMP (0x48 0xB8 ... 0xFF 0xE0).
        bool intact = (code[0] == 0xE9 || code[0] == 0xEB || 
                       (code[0] == 0xFF && (code[1] == 0x25 || code[1] == 0x15)) ||
                       (code[0] == 0x48 && code[1] == 0xB8 && code[10] == 0xFF && code[11] == 0xE0));
        if (!intact) {
            LOG_WARN("DX12Hook: Hook integrity check FAILED for %s! Attempting to re-enable hook.", name);
            MH_QueueEnableHook(target);
            MH_ApplyQueued();
        }
    };
    
    verify(g_targetExecuteCommandLists, "ExecuteCommandLists");
    verify(g_targetDrawIndexedInstanced, "DrawIndexedInstanced");
    verify(g_targetOMSetRenderTargets, "OMSetRenderTargets");
    verify(g_targetClearDepthStencilView, "ClearDepthStencilView");
    verify(g_targetPresentDX12, "PresentDX12");
    verify(g_targetPresent1DX12, "Present1DX12");
    verify(g_targetResizeBuffers, "ResizeBuffers");
    verify(g_targetResizeBuffers1, "ResizeBuffers1");
    verify(g_targetMapDX12, "Map");
    verify(g_targetUnmapDX12, "Unmap");
    verify(g_targetCreateDepthStencilView, "CreateDepthStencilView");
    verify(g_targetCopyDescriptors, "CopyDescriptors");
    verify(g_targetCopyDescriptorsSimple, "CopyDescriptorsSimple");
}

template<typename SwapChainType, typename OriginalFunc, typename... Args>
HRESULT ProcessPresentDX12(SwapChainType* pSwapChain, OriginalFunc originalFunc, Args... args) {
    if (!vrinject::seh::IsValidMemoryPointer(pSwapChain) || !originalFunc) {
        return originalFunc ? originalFunc(pSwapChain, args...) : DXGI_ERROR_INVALID_CALL;
    }

    if (vrinject::vulkan::VulkanLifecycleManager::Get().GetState() != vrinject::RenderState::UNINITIALIZED) {
        // Vulkan is active, this is likely an internal driver swapchain presentation. Pass through.
        return originalFunc(pSwapChain, args...);
    }

    HRESULT hr = S_OK;
    try {
        // In DX12, we need the command queue to provide to the lifecycle manager.
        // Get it from the swapchain.
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> localQueue;
        ID3D12CommandQueue* pQueue = nullptr;
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D12CommandQueue), (void**)&localQueue))) {
            pQueue = localQueue.Get();
        } else {
            // Fallback for some wrappers that return ID3D12Device directly (technically invalid for swapchains, but happens)
            Microsoft::WRL::ComPtr<ID3D12Device> localDevice;
            if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void**)&localDevice))) {
                LOG_WARN("DX12Hook: SwapChain->GetDevice returned Device instead of CommandQueue! We need a queue.");
            }
        }
        
        if (g_mainSwapChain == nullptr) {
            g_mainSwapChain = pSwapChain;
        }

        // Execute original present first
        hr = originalFunc(pSwapChain, args...);

        // Advance lifecycle state machine and get immutable snapshot
        RenderFrameSnapshot snapshot = Dx12LifecycleManager::Get().ProcessPresent(pSwapChain, pQueue, hr);

        // Exception-safe RAII frame lifecycle
        ScopedFrame frame(*SubsystemContext::Get().GetFrameCoordinator(), snapshot);

        static int s_frameCount = 0;
        if (++s_frameCount % 600 == 0) {
            VerifyHookIntegrityDX12();
        }

    } catch (const std::system_error& e) {
        LOG_ERROR("DX12Hook: ProcessPresent system exception: %s (code: %d)", e.what(), e.code().value());
    } catch (const std::exception& e) {
        LOG_ERROR("DX12Hook: ProcessPresent exception caught: %s", e.what());
    } catch (...) {
        LOG_ERROR("DX12Hook: ProcessPresent unknown exception caught. Check VEH observer logs for SEH code.");
    }

    return hr;
}

HRESULT __stdcall hkPresentDX12(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    return ProcessPresentDX12(pSwapChain, OriginalPresentDX12, SyncInterval, Flags);
}

HRESULT __stdcall hkPresent1DX12(IDXGISwapChain1* pSwapChain, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    return ProcessPresentDX12(pSwapChain, OriginalPresent1DX12, SyncInterval, PresentFlags, pPresentParameters);
}

HRESULT __stdcall hkResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    LOG_INFO("DX12Hook: hkResizeBuffers requested format %d (%ux%u)", NewFormat, Width, Height);
    Dx12LifecycleManager::Get().ReleaseSwapchainReferences();
    HRESULT hr = OriginalResizeBuffers ? OriginalResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags) : DXGI_ERROR_INVALID_CALL;
    if (SUCCEEDED(hr)) {
        Dx12LifecycleManager::Get().NotifyResizeComplete();
    }
    return hr;
}

HRESULT __stdcall hkResizeBuffers1(IDXGISwapChain3* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags, const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue) {
    LOG_INFO("DX12Hook: hkResizeBuffers1 requested format %d (%ux%u)", NewFormat, Width, Height);
    Dx12LifecycleManager::Get().ReleaseSwapchainReferences();
    HRESULT hr = OriginalResizeBuffers1 ? OriginalResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask, ppPresentQueue) : DXGI_ERROR_INVALID_CALL;
    if (SUCCEEDED(hr)) {
        Dx12LifecycleManager::Get().NotifyResizeComplete();
    }
    return hr;
}

void __stdcall hkCreateDepthStencilView(ID3D12Device* pDevice, ID3D12Resource* pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    Dx12DescriptorTracker::Get().RegisterDescriptor(DestDescriptor, pResource, pDesc);
    if (OriginalCreateDepthStencilView) {
        OriginalCreateDepthStencilView(pDevice, pResource, pDesc, DestDescriptor);
    }
}

void __stdcall hkCopyDescriptors(ID3D12Device* pDevice, UINT NumDestDescriptorRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts, const UINT* pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts, const UINT* pSrcDescriptorRangeSizes, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) {
    if (OriginalCopyDescriptors) {
        OriginalCopyDescriptors(pDevice, NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes, NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes, DescriptorHeapsType);
    }
}

void __stdcall hkCopyDescriptorsSimple(ID3D12Device* pDevice, UINT NumDescriptors, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart, D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) {
    SIZE_T increment = pDevice->GetDescriptorHandleIncrementSize(DescriptorHeapsType);
    for (UINT i = 0; i < NumDescriptors; ++i) {
        D3D12_CPU_DESCRIPTOR_HANDLE dst = DestDescriptorRangeStart;
        dst.ptr += i * increment;
        D3D12_CPU_DESCRIPTOR_HANDLE src = SrcDescriptorRangeStart;
        src.ptr += i * increment;
        Dx12DescriptorTracker::Get().CopyDescriptor(dst, src);
    }
    if (OriginalCopyDescriptorsSimple) {
        OriginalCopyDescriptorsSimple(pDevice, NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart, DescriptorHeapsType);
    }
}

void __stdcall hkExecuteCommandLists(ID3D12CommandQueue* pCommandQueue, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists) {
    if (pCommandQueue && vrinject::seh::IsValidMemoryPointer(pCommandQueue)) {
        D3D12_COMMAND_QUEUE_DESC desc = pCommandQueue->GetDesc();
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            DXGIFactoryHook::SetCapturedCommandQueue(pCommandQueue);
        }
    }
    if (OriginalExecuteCommandLists) {
        OriginalExecuteCommandLists(pCommandQueue, NumCommandLists, ppCommandLists);
    }
}

void __stdcall hkDrawIndexedInstanced(ID3D12GraphicsCommandList* pCommandList, UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) {
    if (OriginalDrawIndexedInstanced) {
        OriginalDrawIndexedInstanced(pCommandList, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
    }
}

void __stdcall hkOMSetRenderTargets(ID3D12GraphicsCommandList* pCommandList, UINT NumRenderTargetDescriptors, const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors, BOOL RTsSingleHandleToDescriptorRange, const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor) {
    if (pDepthStencilDescriptor && pDepthStencilDescriptor->ptr != 0) {
        GraphicsResourceIdentity identity;
        if (Dx12DescriptorTracker::Get().ResolveDescriptor(*pDepthStencilDescriptor, identity)) {
            // Forward identity to collector.
            SubsystemContext::Get().GetDepthCandidateCollector()->OnOMSetRenderTargets(identity);
        }
    }
    if (OriginalOMSetRenderTargets) {
        OriginalOMSetRenderTargets(pCommandList, NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
    }
}

void __stdcall hkClearDepthStencilView(ID3D12GraphicsCommandList* pCommandList, D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects) {
    if (DepthStencilView.ptr != 0) {
        GraphicsResourceIdentity identity;
        if (Dx12DescriptorTracker::Get().ResolveDescriptor(DepthStencilView, identity)) {
            SubsystemContext::Get().GetDepthCandidateCollector()->OnClearDepthStencilView(identity, Depth);
        }
    }
    if (OriginalClearDepthStencilView) {
        OriginalClearDepthStencilView(pCommandList, DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects);
    }
}


bool Initialize() {
    LOG_INFO("DX12Hook: Initialize started");
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        LOG_ERROR("DX12Hook: MinHook failed");
        return false;
    }

    // Create dummy window and swapchain to get vtables
    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA), CS_HREDRAW | CS_VREDRAW, DefWindowProcA, 0, 0, GetModuleHandle(nullptr), NULL, NULL, NULL, NULL, "VRInjectDummyDX12", NULL };
    RegisterClassExA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "Dummy", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);

    ID3D12Device* pDevice = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&pDevice))) {
        LOG_ERROR("DX12Hook: D3D12CreateDevice failed");
        DestroyWindow(hwnd);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ID3D12CommandQueue* pCommandQueue = nullptr;
    if (FAILED(pDevice->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue), (void**)&pCommandQueue)) || !pCommandQueue) {
        LOG_ERROR("DX12Hook: CreateCommandQueue failed");
        pDevice->Release();
        DestroyWindow(hwnd);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return false;
    }

    IDXGIFactory4* dxgiFactory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory))) || !dxgiFactory) {
        LOG_ERROR("DX12Hook: CreateDXGIFactory1 failed");
        pCommandQueue->Release();
        pDevice->Release();
        DestroyWindow(hwnd);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd0 = {};
    sd0.BufferCount = 2;
    sd0.BufferDesc.Width = 100;
    sd0.BufferDesc.Height = 100;
    sd0.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd0.BufferDesc.RefreshRate.Numerator = 60;
    sd0.BufferDesc.RefreshRate.Denominator = 1;
    sd0.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd0.OutputWindow = hwnd;
    sd0.SampleDesc.Count = 1;
    sd0.Windowed = TRUE;
    sd0.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain* pSwapChain = nullptr;
    if (FAILED(dxgiFactory->CreateSwapChain(pCommandQueue, &sd0, &pSwapChain))) {
        LOG_ERROR("DX12Hook: CreateSwapChain failed");
        dxgiFactory->Release();
        pCommandQueue->Release();
        pDevice->Release();
        DestroyWindow(hwnd);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return false;
    }
    if (!pSwapChain) {
        return false;
    }
    IDXGISwapChain* pSwapChain0 = pSwapChain;

    HWND hwnd2 = CreateWindowExA(0, wc.lpszClassName, "Dummy2", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.BufferCount = 2;
    sd.Width = 100;
    sd.Height = 100;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SampleDesc.Count = 1;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain1* pSwapChain1 = nullptr;
    if (FAILED(dxgiFactory->CreateSwapChainForHwnd(pCommandQueue, hwnd2, &sd, nullptr, nullptr, &pSwapChain1)) || !pSwapChain1) {
        LOG_ERROR("DX12Hook: CreateSwapChainForHwnd failed");
        pSwapChain0->Release();
        dxgiFactory->Release();
        pCommandQueue->Release();
        pDevice->Release();
        // FIX #16: Destroy hwnd2 on this error path too.
        DestroyWindow(hwnd2);
        DestroyWindow(hwnd);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return false;
    }


    void** pSwapChain0Vtable = *reinterpret_cast<void***>(pSwapChain0);
    void** pSwapChain1Vtable = *reinterpret_cast<void***>(pSwapChain1);
    void** pCommandQueueVtable = *reinterpret_cast<void***>(pCommandQueue);

    // Create a committed resource to get the ID3D12Resource vtable
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC resDesc = {};
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resDesc.Width = 256;
    resDesc.Height = 1;
    resDesc.DepthOrArraySize = 1;
    resDesc.MipLevels = 1;
    resDesc.Format = DXGI_FORMAT_UNKNOWN;
    resDesc.SampleDesc.Count = 1;
    resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* pDummyResource = nullptr;
    if (FAILED(pDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&pDummyResource))) {
        pDummyResource = nullptr;
    }

    void* present0Address = pSwapChain0Vtable[8];
    void* resizeBuffersAddress = pSwapChain0Vtable[13];
    void* present1Address = pSwapChain1Vtable[8];
    void* present1ExAddress = pSwapChain1Vtable[22];
    void* resizeBuffers1Address = nullptr;
    
    // Check if IDXGISwapChain3 is supported
    IDXGISwapChain3* pSwapChain3 = nullptr;
    if (pSwapChain1 && SUCCEEDED(pSwapChain1->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&pSwapChain3))) {
        void** pSwapChain3Vtable = *reinterpret_cast<void***>(pSwapChain3);
        resizeBuffers1Address = pSwapChain3Vtable[39];
        pSwapChain3->Release();
    }
    void* executeCommandListsAddress = pCommandQueueVtable[10];
    
    pSwapChain0->Release();
    pSwapChain1->Release();
    dxgiFactory->Release();
    void* mapAddress = nullptr;
    void* unmapAddress = nullptr;
    if (pDummyResource) {
        void** pResourceVtable = *reinterpret_cast<void***>(pDummyResource);
        mapAddress = pResourceVtable[8];
        unmapAddress = pResourceVtable[9];
        pDummyResource->Release();
    }

    // Extract Command List VTable for DrawIndexedInstanced
    void* drawIndexedInstancedAddress = nullptr;
    void* omSetRenderTargetsAddress = nullptr;
    void* clearDepthStencilViewAddress = nullptr;
    ID3D12CommandAllocator* pAllocator = nullptr;
    if (SUCCEEDED(pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&pAllocator))) {
        ID3D12GraphicsCommandList* pCommandList = nullptr;
        if (SUCCEEDED(pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, pAllocator, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&pCommandList))) {
            void** pCommandListVtable = *reinterpret_cast<void***>(pCommandList);
            drawIndexedInstancedAddress = pCommandListVtable[13]; // DrawIndexedInstanced
            omSetRenderTargetsAddress = pCommandListVtable[46];   // OMSetRenderTargets
            clearDepthStencilViewAddress = pCommandListVtable[47];// ClearDepthStencilView
            pCommandList->Release();
        }
        pAllocator->Release();
    }

    void** pDeviceVtable = *reinterpret_cast<void***>(pDevice);
    void* createDepthStencilViewAddress = pDeviceVtable[21];
    void* copyDescriptorsAddress = pDeviceVtable[23];
    void* copyDescriptorsSimpleAddress = pDeviceVtable[24];

    pCommandQueue->Release();
    pDevice->Release();
    DestroyWindow(hwnd);
    DestroyWindow(hwnd2);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);

    // FIX #5: Validate hook target addresses are within a known D3D/DXGI module before hooking.
    // This prevents hooking a VTable proxy installed by another overlay or anti-cheat.
    auto IsValidHookTarget = [](void* addr) -> bool {
        if (!addr) return false;
        HMODULE hMod = nullptr;
        // GetModuleHandleExA with GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS gives us the
        // owning module of any address, without incrementing the reference count.
        if (!GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(addr), &hMod) || !hMod) {
            LOG_WARN("DX12Hook: Hook target %p has no owning module — likely a proxy vtable, skipping.", addr);
            return false;
        }
        char modName[MAX_PATH] = {};
        GetModuleFileNameA(hMod, modName, MAX_PATH);
        // Only allow hooks into known Microsoft D3D/DXGI system DLLs.
        std::string path(modName);
        // Extract filename from path
        size_t lastSlash = path.find_last_of("\\/");
        std::string filename = (lastSlash == std::string::npos) ? path : path.substr(lastSlash + 1);
        auto toLower = [](std::string s) { for (auto& c : s) c = (char)tolower(c); return s; };
        std::string filenameLower = toLower(filename);
        std::string pathLower = toLower(path);
        
        // Exact matches for known system DLLs (case-insensitive)
        if (filenameLower != "d3d12.dll" &&
            filenameLower != "d3d12core.dll" &&
            filenameLower != "dxgi.dll" &&
            filenameLower != "d3d11.dll") {
            LOG_WARN("DX12Hook: Hook target %p is in '%s', not a known D3D/DXGI system DLL — skipping.", addr, modName);
            return false;
        }

        // Hardening: Verify the DLL resides strictly in the Windows system directory to block local proxy DLL hijacks
        char sysDir[MAX_PATH] = {};
        if (GetSystemDirectoryA(sysDir, MAX_PATH)) {
            std::string sysPathLower = toLower(sysDir);
            if (pathLower.rfind(sysPathLower, 0) != 0) {
                LOG_WARN("DX12Hook: Hook target %p resides in '%s', which is outside Windows system directory '%s' — skipping.", addr, modName, sysDir);
                return false;
            }
        }
        return true;
    };

    if (IsValidHookTarget(executeCommandListsAddress) &&
        MH_CreateHook(executeCommandListsAddress, (void*)hkExecuteCommandLists, (void**)&OriginalExecuteCommandLists) == MH_OK) {
        g_targetExecuteCommandLists = executeCommandListsAddress;
        MH_EnableHook(executeCommandListsAddress);
    } else {
        LOG_ERROR("MH_CreateHook failed for DX12 ExecuteCommandLists");
    }

    if (IsValidHookTarget(createDepthStencilViewAddress) &&
        MH_CreateHook(createDepthStencilViewAddress, (void*)hkCreateDepthStencilView, (void**)&OriginalCreateDepthStencilView) == MH_OK) {
        g_targetCreateDepthStencilView = createDepthStencilViewAddress;
        MH_EnableHook(createDepthStencilViewAddress);
    }
    if (IsValidHookTarget(copyDescriptorsAddress) &&
        MH_CreateHook(copyDescriptorsAddress, (void*)hkCopyDescriptors, (void**)&OriginalCopyDescriptors) == MH_OK) {
        g_targetCopyDescriptors = copyDescriptorsAddress;
        MH_EnableHook(copyDescriptorsAddress);
    }
    if (IsValidHookTarget(copyDescriptorsSimpleAddress) &&
        MH_CreateHook(copyDescriptorsSimpleAddress, (void*)hkCopyDescriptorsSimple, (void**)&OriginalCopyDescriptorsSimple) == MH_OK) {
        g_targetCopyDescriptorsSimple = copyDescriptorsSimpleAddress;
        MH_EnableHook(copyDescriptorsSimpleAddress);
    }

    if (IsValidHookTarget(drawIndexedInstancedAddress) &&
        MH_CreateHook(drawIndexedInstancedAddress, (void*)hkDrawIndexedInstanced, (void**)&OriginalDrawIndexedInstanced) == MH_OK) {
        g_targetDrawIndexedInstanced = drawIndexedInstancedAddress;
        MH_EnableHook(drawIndexedInstancedAddress);
    } else {
        LOG_ERROR("DX12Hook: MH_CreateHook failed for DrawIndexedInstanced");
    }

    if (IsValidHookTarget(omSetRenderTargetsAddress) &&
        MH_CreateHook(omSetRenderTargetsAddress, (void*)hkOMSetRenderTargets, (void**)&OriginalOMSetRenderTargets) == MH_OK) {
        g_targetOMSetRenderTargets = omSetRenderTargetsAddress;
        MH_EnableHook(omSetRenderTargetsAddress);
    } else {
        LOG_ERROR("DX12Hook: MH_CreateHook failed for OMSetRenderTargets");
    }

    if (IsValidHookTarget(clearDepthStencilViewAddress) &&
        MH_CreateHook(clearDepthStencilViewAddress, (void*)hkClearDepthStencilView, (void**)&OriginalClearDepthStencilView) == MH_OK) {
        g_targetClearDepthStencilView = clearDepthStencilViewAddress;
        MH_EnableHook(clearDepthStencilViewAddress);
    } else {
        LOG_ERROR("DX12Hook: MH_CreateHook failed for ClearDepthStencilView");
    }

    if (IsValidHookTarget(present0Address) &&
        MH_CreateHook(present0Address, (void*)hkPresentDX12, (void**)&OriginalPresentDX12) == MH_OK) {
        g_targetPresentDX12 = present0Address;
        MH_EnableHook(present0Address);
    } else {
        // DX11 hook already owns Present — it will forward DX12 swapchains to OnPresent()
        LOG_INFO("DX12Hook: Present already hooked (DX11 hook owns it), DX12 will rely on DX11 forwarding.");
    }

    if (IsValidHookTarget(present1ExAddress) &&
        MH_CreateHook(present1ExAddress, (void*)hkPresent1DX12, (void**)&OriginalPresent1DX12) == MH_OK) {
        g_targetPresent1DX12 = present1ExAddress;
        MH_EnableHook(present1ExAddress);
    } else {
        LOG_INFO("DX12Hook: Present1 already hooked (DX11 hook owns it).");
    }

    if (IsValidHookTarget(resizeBuffersAddress) &&
        MH_CreateHook(resizeBuffersAddress, (void*)hkResizeBuffers, (void**)&OriginalResizeBuffers) == MH_OK) {
        g_targetResizeBuffers = resizeBuffersAddress;
        MH_EnableHook(resizeBuffersAddress);
    } else {
        LOG_INFO("DX12Hook: ResizeBuffers already hooked (DX11 hook owns it).");
    }

    if (IsValidHookTarget(resizeBuffers1Address) &&
        MH_CreateHook(resizeBuffers1Address, (void*)hkResizeBuffers1, (void**)&OriginalResizeBuffers1) == MH_OK) {
        g_targetResizeBuffers1 = resizeBuffers1Address;
        MH_EnableHook(resizeBuffers1Address);
    }

    // Map and Unmap are intentionally NOT hooked in DX12 to avoid race conditions
    // and driver invalid-call crashes on multi-threaded worker ring buffers.
    LOG_INFO("DX12Hook: Dummy Initialize success, waiting for DynamicHook");
    return true;
}

void Shutdown() {
    if (g_targetExecuteCommandLists) { MH_DisableHook(g_targetExecuteCommandLists); g_targetExecuteCommandLists = nullptr; }
    if (g_targetCreateDepthStencilView) { MH_DisableHook(g_targetCreateDepthStencilView); g_targetCreateDepthStencilView = nullptr; }
    if (g_targetCopyDescriptors) { MH_DisableHook(g_targetCopyDescriptors); g_targetCopyDescriptors = nullptr; }
    if (g_targetCopyDescriptorsSimple) { MH_DisableHook(g_targetCopyDescriptorsSimple); g_targetCopyDescriptorsSimple = nullptr; }
    if (g_targetDrawIndexedInstanced) { MH_DisableHook(g_targetDrawIndexedInstanced); g_targetDrawIndexedInstanced = nullptr; }
    if (g_targetOMSetRenderTargets) { MH_DisableHook(g_targetOMSetRenderTargets); g_targetOMSetRenderTargets = nullptr; }
    if (g_targetClearDepthStencilView) { MH_DisableHook(g_targetClearDepthStencilView); g_targetClearDepthStencilView = nullptr; }
    if (g_targetPresentDX12) { MH_DisableHook(g_targetPresentDX12); g_targetPresentDX12 = nullptr; }
    if (g_targetPresent1DX12) { MH_DisableHook(g_targetPresent1DX12); g_targetPresent1DX12 = nullptr; }
    if (g_targetResizeBuffers) { MH_DisableHook(g_targetResizeBuffers); g_targetResizeBuffers = nullptr; }
    if (g_targetResizeBuffers1) { MH_DisableHook(g_targetResizeBuffers1); g_targetResizeBuffers1 = nullptr; }

    ImGuiDX12Integration::GetInstance().Shutdown();
    Dx12LifecycleManager::Get().Shutdown();
}

FrameResourcesDX12 GetCurrentFrame() {
    FrameResourcesDX12 fr;
    RenderState state = Dx12LifecycleManager::Get().GetState();
    if (state == RenderState::RUNNING) {
        fr.valid = true;
    }
    return fr;
}

void SetOnFrameCallback(OnFrameCallbackDX12 callback) {
    // Deprecated: FrameCoordinator manages callbacks now
}

} // namespace DX12Hook
} // namespace vrinject

