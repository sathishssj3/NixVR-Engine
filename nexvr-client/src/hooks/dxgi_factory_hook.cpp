#include "hooks/dxgi_factory_hook.h"
#include "MinHook.h"
#include "core/logger.h"
#include "rendering/vulkan/vulkan_lifecycle_manager.h"
#include <mutex>

namespace vrinject {
namespace DXGIFactoryHook {

typedef HRESULT(__stdcall* CreateSwapChain_t)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(__stdcall* CreateSwapChainForHwnd_t)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);

CreateSwapChain_t OriginalCreateSwapChain = nullptr;
CreateSwapChainForHwnd_t OriginalCreateSwapChainForHwnd = nullptr;

Microsoft::WRL::ComPtr<ID3D12CommandQueue> g_capturedCommandQueue;
std::mutex g_mutex;

HRESULT __stdcall hkCreateSwapChain(IDXGIFactory* pFactory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain) {
    LOG_INFO("DXGIFactoryHook: CreateSwapChain called");

    if (vrinject::vulkan::VulkanLifecycleManager::Get().GetState() == vrinject::RenderState::RUNNING) {
        LOG_INFO("DXGIFactoryHook: Vulkan is actively presenting. Ignoring DXGI swapchain creation to avoid driver conflict.");
        return OriginalCreateSwapChain(pFactory, pDevice, pDesc, ppSwapChain);
    }

    DXGI_SWAP_CHAIN_DESC modifiedDesc = {};
    if (pDesc) {
        modifiedDesc = *pDesc;
        LOG_INFO("DXGIFactoryHook: CreateSwapChain requested format %d", modifiedDesc.BufferDesc.Format);
        // Only force R8G8B8A8_UNORM if the requested format is not already a supported format.
        // This preserves HDR/10-bit formats requested by the game.
        modifiedDesc.BufferUsage |= DXGI_USAGE_SHADER_INPUT;
    }

    HRESULT hr = OriginalCreateSwapChain(pFactory, pDevice, pDesc ? &modifiedDesc : nullptr, ppSwapChain);
    if (FAILED(hr) && pDesc) {
        LOG_WARN("DXGIFactoryHook: CreateSwapChain with modifiedDesc failed (0x%X), retrying with original desc", hr);
        hr = OriginalCreateSwapChain(pFactory, pDevice, pDesc, ppSwapChain);
    }
    if (SUCCEEDED(hr) && pDevice) {
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&queue))) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_capturedCommandQueue = queue;
            LOG_INFO("DXGIFactoryHook: Captured ID3D12CommandQueue via CreateSwapChain");
        }

    }
    LOG_INFO("DXGIFactoryHook: CreateSwapChain returned hr=0x%X", hr);
    return hr;
}

HRESULT __stdcall hkCreateSwapChainForHwnd(IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) {
    LOG_INFO("DXGIFactoryHook: CreateSwapChainForHwnd called");

    if (vrinject::vulkan::VulkanLifecycleManager::Get().GetState() == vrinject::RenderState::RUNNING) {
        LOG_INFO("DXGIFactoryHook: Vulkan is actively presenting. Ignoring DXGI swapchain creation to avoid driver conflict.");
        return OriginalCreateSwapChainForHwnd(pFactory, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    }

    DXGI_SWAP_CHAIN_DESC1 modifiedDesc = {};
    if (pDesc) {
        modifiedDesc = *pDesc;
        LOG_INFO("DXGIFactoryHook: CreateSwapChainForHwnd requested format %d", modifiedDesc.Format);
        // Only force R8G8B8A8_UNORM if the requested format is not already a supported format.
        // This preserves HDR/10-bit formats requested by the game.
        modifiedDesc.BufferUsage |= DXGI_USAGE_SHADER_INPUT;
    }

    HRESULT hr = OriginalCreateSwapChainForHwnd(pFactory, pDevice, hWnd, pDesc ? &modifiedDesc : nullptr, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    if (FAILED(hr) && pDesc) {
        LOG_WARN("DXGIFactoryHook: CreateSwapChainForHwnd with modifiedDesc failed (0x%X), retrying with original desc", hr);
        hr = OriginalCreateSwapChainForHwnd(pFactory, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    }
    if (SUCCEEDED(hr) && pDevice) {
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&queue))) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_capturedCommandQueue = queue;
            LOG_INFO("DXGIFactoryHook: Captured ID3D12CommandQueue via CreateSwapChainForHwnd");
        }

    }
    LOG_INFO("DXGIFactoryHook: CreateSwapChainForHwnd returned hr=0x%X", hr);
    return hr;
}

static void* g_targetCreateSwapChain = nullptr;
static void* g_targetCreateSwapChainForHwnd = nullptr;

bool Initialize() {
    IDXGIFactory2* pFactory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void**)&pFactory))) {
        LOG_ERROR("DXGIFactoryHook: Failed to create IDXGIFactory2");
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(pFactory);
    void* createSwapChainAddress = vtable[10]; // IDXGIFactory::CreateSwapChain
    void* createSwapChainForHwndAddress = vtable[15]; // IDXGIFactory2::CreateSwapChainForHwnd

    pFactory->Release();

    bool hookedAny = false;

    if (createSwapChainAddress && MH_CreateHook(createSwapChainAddress, (void*)hkCreateSwapChain, (void**)&OriginalCreateSwapChain) == MH_OK) {
        g_targetCreateSwapChain = createSwapChainAddress;
        if (MH_EnableHook(createSwapChainAddress) == MH_OK) {
            hookedAny = true;
            LOG_INFO("DXGIFactoryHook: CreateSwapChain hooked successfully");
        } else {
            LOG_WARN("DXGIFactoryHook: Failed to enable CreateSwapChain hook");
        }
    } else {
        LOG_WARN("DXGIFactoryHook: Failed to hook CreateSwapChain (continuing to CreateSwapChainForHwnd)");
    }

    if (createSwapChainForHwndAddress && MH_CreateHook(createSwapChainForHwndAddress, (void*)hkCreateSwapChainForHwnd, (void**)&OriginalCreateSwapChainForHwnd) == MH_OK) {
        g_targetCreateSwapChainForHwnd = createSwapChainForHwndAddress;
        if (MH_EnableHook(createSwapChainForHwndAddress) == MH_OK) {
            hookedAny = true;
            LOG_INFO("DXGIFactoryHook: CreateSwapChainForHwnd hooked successfully");
        } else {
            LOG_WARN("DXGIFactoryHook: Failed to enable CreateSwapChainForHwnd hook");
        }
    } else {
        LOG_WARN("DXGIFactoryHook: Failed to hook CreateSwapChainForHwnd");
    }

    if (!hookedAny) {
        LOG_ERROR("DXGIFactoryHook: Failed to hook both CreateSwapChain and CreateSwapChainForHwnd");
        return false;
    }

    LOG_INFO("DXGIFactoryHook: Initialized successfully");
    return true;
}

void Shutdown() {
    if (g_targetCreateSwapChain) {
        MH_DisableHook(g_targetCreateSwapChain);
        g_targetCreateSwapChain = nullptr;
    }
    if (g_targetCreateSwapChainForHwnd) {
        MH_DisableHook(g_targetCreateSwapChainForHwnd);
        g_targetCreateSwapChainForHwnd = nullptr;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_capturedCommandQueue.Reset();
}

ID3D12CommandQueue* GetCapturedCommandQueue() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_capturedCommandQueue.Get();
}

void SetCapturedCommandQueue(ID3D12CommandQueue* queue) {
    if (!queue) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_capturedCommandQueue.Get() != queue) {
        g_capturedCommandQueue = queue;
        LOG_INFO("DXGIFactoryHook: Captured ID3D12CommandQueue updated to %p", queue);
    }
}

}
}
