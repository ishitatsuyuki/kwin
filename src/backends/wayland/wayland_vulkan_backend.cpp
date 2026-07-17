/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "wayland_vulkan_backend.h"

#include "core/drmdevice.h"
#include "core/renderbackend.h"
#include "vulkan/vulkan_device.h"
#include "vulkan/vulkan_rendertarget.h"
#include "vulkan/vulkan_swapchain.h"
#include "wayland-client/linuxdmabuf.h"
#include "wayland_backend.h"
#include "wayland_display.h"
#include "wayland_output.h"

#include <KWayland/Client/subsurface.h>
#include <KWayland/Client/surface.h>
#include <drm_fourcc.h>

namespace KWin::Wayland
{

static constexpr VkImageUsageFlags s_outputUsage = VK_IMAGE_USAGE_STORAGE_BIT
    | VK_IMAGE_USAGE_SAMPLED_BIT
    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
    | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

static ModifierList outputModifiers(WaylandVulkanBackend *backend)
{
    VulkanDevice *device = backend->device();
    if (!device || !backend->backend()->display()->linuxDmabuf()) {
        return {};
    }
    return device->computeOutputFormats().value(DRM_FORMAT_ABGR8888).intersected(backend->backend()->display()->linuxDmabuf()->formats().value(DRM_FORMAT_ABGR8888));
}

WaylandVulkanLayer::WaylandVulkanLayer(WaylandOutput *output, WaylandVulkanBackend *backend, OutputLayerType type, int zpos)
    : WaylandLayer(output, type, zpos)
    , m_backend(backend)
{
}

WaylandVulkanLayer::~WaylandVulkanLayer() = default;

std::optional<OutputLayerBeginFrameInfo> WaylandVulkanLayer::doBeginFrame()
{
    if (m_color != m_previousColor) {
        m_damageJournal.clear();
    }
    const QSize size = targetRect().size();
    const ModifierList modifiers = outputModifiers(m_backend);
    if (size.isEmpty() || modifiers.isEmpty()) {
        return std::nullopt;
    }
    if (!m_swapchain || m_swapchain->size() != size || !modifiers.contains(m_swapchain->modifier())) {
        m_swapchain = VulkanSwapchain::create(m_backend->device(),
                                              m_backend->drmDevice()->allocator(),
                                              size,
                                              DRM_FORMAT_ABGR8888,
                                              modifiers,
                                              s_outputUsage);
        m_damageJournal.clear();
        if (!m_swapchain) {
            return std::nullopt;
        }
    }
    m_buffer = m_swapchain->acquire();
    if (!m_buffer) {
        return std::nullopt;
    }
    m_target = std::make_unique<VulkanRenderTarget>(m_buffer->texture(), m_buffer->releaseFd().duplicate(), bufferTransform());
    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(m_target.get(), m_color),
        .repaint = m_damageJournal.accumulate(m_buffer->age(), Region::infinite()),
    };
}

bool WaylandVulkanLayer::doEndFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame)
{
    if (!m_target || !m_buffer || !m_swapchain) {
        return false;
    }
    if (frame) {
        for (auto &query : m_target->takeRenderTimeQueries()) {
            frame->addRenderTimeQuery(std::move(query));
        }
    }
    FileDescriptor completionFence = m_target->takeCompletionFence();
    if (!completionFence.isValid()) {
        return false;
    }
    setBuffer(m_buffer->buffer(), damagedDeviceRegion, completionFence.duplicate());
    m_swapchain->releaseRendered(m_buffer.get(), std::move(completionFence));
    m_damageJournal.add(damagedDeviceRegion);
    m_target.reset();
    return true;
}

bool WaylandVulkanLayer::earlyScanoutChecks()
{
    return test();
}

bool WaylandVulkanLayer::importScanoutBuffer(GraphicsBuffer *buffer, const std::shared_ptr<OutputFrame> &frame)
{
    setBuffer(buffer, Region::infinite());
    m_damageJournal.clear();
    return true;
}

DrmDevice *WaylandVulkanLayer::scanoutDevice() const
{
    return m_backend->drmDevice();
}

FormatModifierMap WaylandVulkanLayer::supportedDrmFormats() const
{
    return m_backend->backend()->display()->linuxDmabuf()->formats();
}

void WaylandVulkanLayer::releaseBuffers()
{
    m_target.reset();
    m_buffer.reset();
    m_swapchain.reset();
    m_damageJournal.clear();
}

WaylandVulkanCursorLayer::WaylandVulkanCursorLayer(WaylandOutput *output, WaylandVulkanBackend *backend)
    : OutputLayer(output, OutputLayerType::CursorOnly, 255, 255, 255)
    , m_backend(backend)
{
}

WaylandVulkanCursorLayer::~WaylandVulkanCursorLayer() = default;

std::optional<OutputLayerBeginFrameInfo> WaylandVulkanCursorLayer::doBeginFrame()
{
    const QSize size = targetRect().size();
    const ModifierList modifiers = outputModifiers(m_backend);
    if (size.isEmpty() || modifiers.isEmpty()) {
        return std::nullopt;
    }
    if (!m_swapchain || m_swapchain->size() != size || !modifiers.contains(m_swapchain->modifier())) {
        m_swapchain = VulkanSwapchain::create(m_backend->device(),
                                              m_backend->drmDevice()->allocator(),
                                              size,
                                              DRM_FORMAT_ABGR8888,
                                              modifiers,
                                              s_outputUsage);
        if (!m_swapchain) {
            return std::nullopt;
        }
    }
    m_buffer = m_swapchain->acquire();
    if (!m_buffer) {
        return std::nullopt;
    }
    m_target = std::make_unique<VulkanRenderTarget>(m_buffer->texture(), m_buffer->releaseFd().duplicate(), bufferTransform());
    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(m_target.get()),
        .repaint = Region::infinite(),
    };
}

bool WaylandVulkanCursorLayer::doEndFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame)
{
    if (!m_target || !m_buffer || !m_swapchain) {
        return false;
    }
    if (frame) {
        for (auto &query : m_target->takeRenderTimeQueries()) {
            frame->addRenderTimeQuery(std::move(query));
        }
    }
    FileDescriptor completionFence = m_target->takeCompletionFence();
    if (!completionFence.isValid()) {
        return false;
    }
    wl_buffer *buffer = m_backend->backend()->importBuffer(m_buffer->buffer());
    if (!buffer) {
        return false;
    }
    auto output = static_cast<WaylandOutput *>(m_output.get());
    output->cursor()->update(buffer,
                             m_buffer->buffer()->size() / m_output->scale(),
                             (hotspot() / m_output->scale()).toPoint(),
                             m_buffer->buffer(),
                             completionFence.duplicate());
    m_swapchain->releaseRendered(m_buffer.get(), std::move(completionFence));
    m_target.reset();
    return true;
}

DrmDevice *WaylandVulkanCursorLayer::scanoutDevice() const
{
    return m_backend->drmDevice();
}

FormatModifierMap WaylandVulkanCursorLayer::supportedDrmFormats() const
{
    return m_backend->backend()->display()->linuxDmabuf()->formats();
}

void WaylandVulkanCursorLayer::releaseBuffers()
{
    m_target.reset();
    m_buffer.reset();
    m_swapchain.reset();
}

WaylandVulkanBackend::WaylandVulkanBackend(WaylandBackend *backend)
    : VulkanBackend(backend->renderDevice())
    , m_backend(backend)
{
    connect(m_backend, &WaylandBackend::outputAdded, this, &WaylandVulkanBackend::createOutputLayers);
    for (BackendOutput *output : m_backend->outputs()) {
        createOutputLayers(output);
    }
}

WaylandVulkanBackend::~WaylandVulkanBackend()
{
    for (BackendOutput *output : m_backend->outputs()) {
        static_cast<WaylandOutput *>(output)->setOutputLayers({});
    }
}

WaylandBackend *WaylandVulkanBackend::backend() const
{
    return m_backend;
}

void WaylandVulkanBackend::createOutputLayers(BackendOutput *output)
{
    auto waylandOutput = static_cast<WaylandOutput *>(output);
    std::vector<std::unique_ptr<OutputLayer>> layers;
    auto primary = std::make_unique<WaylandVulkanLayer>(waylandOutput, this, OutputLayerType::Primary, 0);
    primary->subSurface()->placeAbove(waylandOutput->surface());
    layers.push_back(std::move(primary));
    for (int z = 1; z < 5; ++z) {
        auto layer = std::make_unique<WaylandVulkanLayer>(waylandOutput, this, OutputLayerType::GenericLayer, z);
        layer->subSurface()->placeAbove(static_cast<WaylandVulkanLayer *>(layers.back().get())->surface());
        layers.push_back(std::move(layer));
    }
    layers.push_back(std::make_unique<WaylandVulkanCursorLayer>(waylandOutput, this));
    waylandOutput->setOutputLayers(std::move(layers));
}

QList<OutputLayer *> WaylandVulkanBackend::compatibleOutputLayers(BackendOutput *output)
{
    return static_cast<WaylandOutput *>(output)->outputLayers();
}

} // namespace KWin::Wayland

#include "moc_wayland_vulkan_backend.cpp"
