/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "virtual_vulkan_backend.h"

#include "core/drmdevice.h"
#include "core/renderbackend.h"
#include "virtual_backend.h"
#include "virtual_output.h"
#include "vulkan/vulkan_device.h"
#include "vulkan/vulkan_rendertarget.h"
#include "vulkan/vulkan_swapchain.h"
#include "vulkan/vulkan_texture.h"

#include <drm_fourcc.h>

namespace KWin
{

static constexpr VkImageUsageFlags s_outputUsage = VK_IMAGE_USAGE_STORAGE_BIT
    | VK_IMAGE_USAGE_SAMPLED_BIT
    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
    | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

VirtualVulkanLayer::VirtualVulkanLayer(BackendOutput *output, VulkanBackend *backend)
    : OutputLayer(output, OutputLayerType::Primary)
    , m_backend(backend)
{
}

VirtualVulkanLayer::~VirtualVulkanLayer() = default;

std::optional<OutputLayerBeginFrameInfo> VirtualVulkanLayer::doBeginFrame()
{
    VulkanDevice *device = m_backend->device();
    if (!device) {
        return std::nullopt;
    }
    const QSize nativeSize = m_output->modeSize();
    const ModifierList modifiers = device->computeOutputFormats().value(DRM_FORMAT_ABGR8888);
    if (modifiers.isEmpty()) {
        return std::nullopt;
    }
    if (!m_swapchain || m_swapchain->size() != nativeSize) {
        m_swapchain = VulkanSwapchain::create(device,
                                              m_backend->drmDevice()->allocator(),
                                              nativeSize,
                                              DRM_FORMAT_ABGR8888,
                                              modifiers,
                                              s_outputUsage);
        m_damageJournal.clear();
        if (!m_swapchain) {
            return std::nullopt;
        }
    }

    m_current = m_swapchain->acquire();
    if (!m_current) {
        return std::nullopt;
    }
    m_target = std::make_unique<VulkanRenderTarget>(m_current->texture(), m_current->releaseFd().duplicate(), bufferTransform());
    const Region fallback(Rect(QPoint(), nativeSize));
    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(m_target.get()),
        .repaint = m_damageJournal.accumulate(m_current->age(), fallback),
    };
}

bool VirtualVulkanLayer::doEndFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame)
{
    if (!m_target || !m_current || !m_swapchain) {
        return false;
    }
    for (auto &query : m_target->takeRenderTimeQueries()) {
        frame->addRenderTimeQuery(std::move(query));
    }
    FileDescriptor completionFence = m_target->takeCompletionFence();
    if (!completionFence.isValid()) {
        m_damageJournal.clear();
        return false;
    }
    m_swapchain->release(m_current.get(), std::move(completionFence));
    m_damageJournal.add(damagedDeviceRegion);
    m_target.reset();
    return true;
}

DrmDevice *VirtualVulkanLayer::scanoutDevice() const
{
    return m_backend->drmDevice();
}

FormatModifierMap VirtualVulkanLayer::supportedDrmFormats() const
{
    return m_backend->device() ? m_backend->device()->computeOutputFormats() : FormatModifierMap{};
}

void VirtualVulkanLayer::releaseBuffers()
{
    m_target.reset();
    m_current.reset();
    m_swapchain.reset();
    m_damageJournal.clear();
}

VulkanTexture *VirtualVulkanLayer::texture() const
{
    return m_current ? m_current->texture() : nullptr;
}

VirtualVulkanBackend::VirtualVulkanBackend(VirtualBackend *backend)
    : VulkanBackend(backend->renderDevice())
    , m_backend(backend)
{
    connect(backend, &VirtualBackend::outputAdded, this, &VirtualVulkanBackend::addOutput);
    for (BackendOutput *output : backend->outputs()) {
        addOutput(output);
    }
}

VirtualVulkanBackend::~VirtualVulkanBackend()
{
    for (BackendOutput *output : m_backend->outputs()) {
        static_cast<VirtualOutput *>(output)->setOutputLayer(nullptr);
    }
}

void VirtualVulkanBackend::addOutput(BackendOutput *output)
{
    static_cast<VirtualOutput *>(output)->setOutputLayer(std::make_unique<VirtualVulkanLayer>(output, this));
}

QList<OutputLayer *> VirtualVulkanBackend::compatibleOutputLayers(BackendOutput *output)
{
    return {static_cast<VirtualOutput *>(output)->outputLayer()};
}

} // namespace KWin

#include "moc_virtual_vulkan_backend.cpp"
