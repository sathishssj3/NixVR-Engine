#include "rendering/dx11/dx11_lifecycle_manager.h"
#include "core/logger.h"
#include "core/fault_injector.h"
#include <d3d11_1.h>

namespace vrinject {

RenderFrameSnapshot Dx11LifecycleManager::ProcessPresent(IDXGISwapChain* swapChain, HRESULT presentResult) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Fault injection check
    if (FaultInjector::ShouldFail(Dx11FaultPoint::PRESENT_DEVICE_REMOVED)) presentResult = DXGI_ERROR_DEVICE_REMOVED;
    if (FaultInjector::ShouldFail(Dx11FaultPoint::PRESENT_DEVICE_RESET)) presentResult = DXGI_ERROR_DEVICE_RESET;
    if (FaultInjector::ShouldFail(Dx11FaultPoint::PRESENT_DEVICE_HUNG)) presentResult = DXGI_ERROR_DEVICE_HUNG;

    // 1. Handle explicit device loss before anything else
    if (presentResult == DXGI_ERROR_DEVICE_REMOVED || 
        presentResult == DXGI_ERROR_DEVICE_RESET || 
        presentResult == DXGI_ERROR_DEVICE_HUNG) {
        HandleDeviceLoss(presentResult);
    }

    RenderState currentState = m_state.load(std::memory_order_relaxed);

    if (currentState == RenderState::DEGRADED) {
        // Self-healing: If a new swapchain is presented after degradation (e.g. game destroyed an intro/splash
        // swapchain and created the real game swapchain), reset degradation state and attempt re-discovery.
        if (swapChain && swapChain != m_swapchainResources.swapChain.Get()) {
            LOG_INFO("Dx11LifecycleManager: New swapchain (%p) presented after degradation. Resetting state to attempt discovery.", swapChain);
            m_state.store(RenderState::UNINITIALIZED, std::memory_order_release);
            currentState = RenderState::UNINITIALIZED;
        } else {
            return CreateSnapshot();
        }
    } else if (currentState == RenderState::SHUTTING_DOWN || currentState == RenderState::STOPPED) {
        // Safe pass-through mode
        return CreateSnapshot();
    }

    if (!m_deviceResources.device ||
        currentState == RenderState::UNINITIALIZED || currentState == RenderState::RECOVERING || currentState == RenderState::REINITIALIZING) {
        Discover(swapChain);
    } else if (currentState == RenderState::RUNNING) {
        Validate(swapChain);
    }

    currentState = m_state.load(std::memory_order_relaxed);

    if (currentState == RenderState::RESIZE_PENDING || currentState == RenderState::INITIALIZING) {
        if (Rebuild()) {
            m_state.store(RenderState::RUNNING, std::memory_order_release);
        } else {
            // Failed to rebuild resources, might need another frame or device is lost
            m_state.store(RenderState::RESIZE_PENDING, std::memory_order_release);
        }
    } else if (currentState == RenderState::DEVICE_REMOVED) {
        if (Recover()) {
            m_state.store(RenderState::REINITIALIZING, std::memory_order_release);
        }
    }

    return CreateSnapshot();
}

void Dx11LifecycleManager::Discover(IDXGISwapChain* swapChain) {
    if (!swapChain) return;

    if (FaultInjector::ShouldFail(Dx11FaultPoint::DISCOVER_SWAPCHAIN)) return;

    m_swapchainResources.swapChain = swapChain;

    if (FaultInjector::ShouldFail(Dx11FaultPoint::DISCOVER_DEVICE) || 
        FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&m_deviceResources.device))) {
        // Fallback 1: Query IDXGIDevice then QueryInterface for ID3D11Device
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        if (SUCCEEDED(swapChain->GetDevice(__uuidof(IDXGIDevice), (void**)&dxgiDevice)) && dxgiDevice) {
            dxgiDevice.As(&m_deviceResources.device);
        }
        // Fallback 2: Query ID3D11Device1 then cast/As to ID3D11Device
        if (!m_deviceResources.device) {
            Microsoft::WRL::ComPtr<ID3D11Device1> device1;
            if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device1), (void**)&device1)) && device1) {
                device1.As(&m_deviceResources.device);
            }
        }
        if (!m_deviceResources.device) {
            Degrade("Failed to query ID3D11Device from SwapChain");
            return;
        }
    }

    m_deviceResources.device->GetImmediateContext(&m_deviceResources.immediateContext);
    if (!m_deviceResources.immediateContext) {
        Degrade("Failed to get ImmediateContext");
        return;
    }

    m_epoch.deviceGeneration++;
    m_epoch.swapchainGeneration++;
    m_epoch.resourceGeneration++;
    
    m_state.store(RenderState::INITIALIZING, std::memory_order_release);
    LOG_INFO("Dx11LifecycleManager: Discovered Device %p (Epoch %llu:%llu:%llu)", 
        m_deviceResources.device.Get(), m_epoch.deviceGeneration, m_epoch.swapchainGeneration, m_epoch.resourceGeneration);
}

void Dx11LifecycleManager::Validate(IDXGISwapChain* swapChain) {
    if (!swapChain || !m_swapchainResources.swapChain) return;

    // Check if swapchain pointer changed (e.g., game completely recreated it)
    if (swapChain != m_swapchainResources.swapChain.Get()) {
        LOG_WARN("Dx11LifecycleManager: Swapchain pointer changed. Triggering rediscovery.");
        m_state.store(RenderState::REINITIALIZING, std::memory_order_release);
        return;
    }

    if (FaultInjector::ShouldFail(Dx11FaultPoint::RESIZE_BUFFERS)) {
        m_state.store(RenderState::RESIZE_PENDING, std::memory_order_release);
        return;
    }

    // Check for resize/fullscreen changes
    DXGI_SWAP_CHAIN_DESC desc;
    if (SUCCEEDED(swapChain->GetDesc(&desc))) {
        bool needsRebuild = false;
        
        if (desc.BufferDesc.Width != m_swapchainResources.width || desc.BufferDesc.Height != m_swapchainResources.height) {
            LOG_INFO("Dx11LifecycleManager: Resize detected (%u x %u -> %u x %u)", 
                     m_swapchainResources.width, m_swapchainResources.height, desc.BufferDesc.Width, desc.BufferDesc.Height);
            needsRebuild = true;
        }
        
        BOOL fullscreen = FALSE;
        swapChain->GetFullscreenState(&fullscreen, nullptr);
        if ((fullscreen == TRUE) != m_swapchainResources.fullscreen) {
            LOG_INFO("Dx11LifecycleManager: Fullscreen state changed (%d -> %d)", m_swapchainResources.fullscreen, fullscreen);
            needsRebuild = true;
        }

        if (needsRebuild) {
            m_state.store(RenderState::RESIZE_PENDING, std::memory_order_release);
        }
    }
}

bool Dx11LifecycleManager::Rebuild() {
    if (!m_swapchainResources.swapChain || !m_deviceResources.device) return false;

    // Release old resources
    m_swapchainResources.backBuffer.Reset();
    m_swapchainResources.rtv.Reset();

    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(m_swapchainResources.swapChain->GetDesc(&desc))) {
        return false;
    }

    m_swapchainResources.width = desc.BufferDesc.Width;
    m_swapchainResources.height = desc.BufferDesc.Height;
    m_swapchainResources.format = desc.BufferDesc.Format;
    
    BOOL fullscreen = FALSE;
    m_swapchainResources.swapChain->GetFullscreenState(&fullscreen, nullptr);
    m_swapchainResources.fullscreen = (fullscreen == TRUE);

    if (FaultInjector::ShouldFail(Dx11FaultPoint::GET_BACKBUFFER) || 
        FAILED(m_swapchainResources.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&m_swapchainResources.backBuffer))) {
        LOG_ERROR("Dx11LifecycleManager: Failed to get backbuffer during rebuild");
        return false;
    }

    if (FaultInjector::ShouldFail(Dx11FaultPoint::CREATE_RTV) || 
        FAILED(m_deviceResources.device->CreateRenderTargetView(m_swapchainResources.backBuffer.Get(), nullptr, &m_swapchainResources.rtv))) {
        LOG_ERROR("Dx11LifecycleManager: Failed to create RTV during rebuild");
        return false;
    }

    if (m_state.load() == RenderState::RESIZE_PENDING) {
        m_epoch.swapchainGeneration++; // swapchain internal configuration changed
    }
    m_epoch.resourceGeneration++;
    
    LOG_INFO("Dx11LifecycleManager: Resources rebuilt successfully (Epoch %llu:%llu:%llu)", 
        m_epoch.deviceGeneration, m_epoch.swapchainGeneration, m_epoch.resourceGeneration);
    m_recoveryAttempts = 0; // Reset budget on successful build
    return true;
}

void Dx11LifecycleManager::HandleDeviceLoss(HRESULT presentResult) {
    if (m_state.load(std::memory_order_relaxed) == RenderState::DEGRADED) return;

    const char* reason = "Unknown";
    if (presentResult == DXGI_ERROR_DEVICE_REMOVED) reason = "DXGI_ERROR_DEVICE_REMOVED";
    else if (presentResult == DXGI_ERROR_DEVICE_RESET) reason = "DXGI_ERROR_DEVICE_RESET";
    else if (presentResult == DXGI_ERROR_DEVICE_HUNG) reason = "DXGI_ERROR_DEVICE_HUNG";

    LOG_ERROR("Dx11LifecycleManager: Device Lost Detected: %s", reason);

    if (m_deviceResources.device) {
        HRESULT removeReason = m_deviceResources.device->GetDeviceRemovedReason();
        LOG_ERROR("Dx11LifecycleManager: GetDeviceRemovedReason(): 0x%X", removeReason);
        SubsystemContext::Get().GetDiagnosticContext()->PostEvent(DiagnosticLevel::Critical, "DX11Lifecycle", "Graphics device lost");
    }

    m_state.store(RenderState::DEVICE_REMOVED, std::memory_order_release);
}

bool Dx11LifecycleManager::Recover() {
    m_recoveryAttempts++;
    LOG_WARN("Dx11LifecycleManager: Attempting recovery %d/%d", m_recoveryAttempts, MAX_RECOVERY_ATTEMPTS);

    if (m_recoveryAttempts > MAX_RECOVERY_ATTEMPTS) {
        Degrade("Exceeded maximum recovery attempts");
        return false;
    }

    // Wipe current state to force a full re-discovery on the next frame
    m_swapchainResources.backBuffer.Reset();
    m_swapchainResources.rtv.Reset();
    m_deviceResources.immediateContext.Reset();
    m_deviceResources.device.Reset();
    m_swapchainResources.swapChain.Reset();

    // Typically, the game will create a new device and swapchain, which we will hook on the next Present.
    return true;
}

void Dx11LifecycleManager::Degrade(const char* reason) {
    LOG_ERROR("Dx11LifecycleManager: Degrading NexVR Features. Reason: %s", reason);
    SubsystemContext::Get().GetDiagnosticContext()->PostEvent(DiagnosticLevel::Warning, "DX11Lifecycle", reason);
    m_state.store(RenderState::DEGRADED, std::memory_order_release);
}

RenderFrameSnapshot Dx11LifecycleManager::CreateSnapshot() {
    RenderFrameSnapshot snap;
    snap.backend = GraphicsBackend::DX11;
    snap.epoch = m_epoch;
    snap.state = m_state.load(std::memory_order_relaxed);
    snap.nativeDevice = m_deviceResources.device.Get();
    snap.nativeContext = m_deviceResources.immediateContext.Get();
    snap.nativeSwapchain = m_swapchainResources.swapChain.Get();
    snap.backBuffer = m_swapchainResources.backBuffer.Get();
    // note: native DX11 rtv is not exposed generically right now.
    // We could store it if needed, or recreate it when needed.
    // For now we don't expose rtv generically.
    snap.format = m_swapchainResources.format;
    snap.width = m_swapchainResources.width;
    snap.height = m_swapchainResources.height;
    snap.fullscreen = m_swapchainResources.fullscreen;
    return snap;
}

void Dx11LifecycleManager::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state.store(RenderState::SHUTTING_DOWN, std::memory_order_release);
    m_swapchainResources.backBuffer.Reset();
    m_swapchainResources.rtv.Reset();
    m_deviceResources.immediateContext.Reset();
    m_deviceResources.device.Reset();
    m_swapchainResources.swapChain.Reset();
    m_state.store(RenderState::STOPPED, std::memory_order_release);
    LOG_INFO("Dx11LifecycleManager: Shutdown complete");
}

void Dx11LifecycleManager::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state.store(RenderState::UNINITIALIZED, std::memory_order_release);
    m_epoch = GraphicsResourceEpoch();
    m_recoveryAttempts = 0;
    m_swapchainResources.backBuffer.Reset();
    m_swapchainResources.rtv.Reset();
    m_deviceResources.immediateContext.Reset();
    m_deviceResources.device.Reset();
    m_swapchainResources.swapChain.Reset();
}

void Dx11LifecycleManager::ReleaseSwapchainReferences() {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Release ALL references to swapchain buffers.
    // DXGI requires zero outstanding references before ResizeBuffers.
    m_swapchainResources.backBuffer.Reset();
    m_swapchainResources.rtv.Reset();

    if (m_deviceResources.immediateContext) {
        m_deviceResources.immediateContext->ClearState();
        m_deviceResources.immediateContext->Flush();
    }

    LOG_INFO("Dx11LifecycleManager: Released swapchain references for ResizeBuffers");
}

void Dx11LifecycleManager::NotifyResizeComplete() {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Mark for rebuild — the next ProcessPresent will re-acquire the backbuffer
    m_state.store(RenderState::RESIZE_PENDING, std::memory_order_release);
    LOG_INFO("Dx11LifecycleManager: ResizeBuffers completed, marked for rebuild");
}

} // namespace vrinject

