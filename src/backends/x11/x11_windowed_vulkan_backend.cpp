/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "x11_windowed_vulkan_backend.h"

#include "core/drmdevice.h"
#include "core/renderbackend.h"
#include "vulkan/vulkan_device.h"
#include "vulkan/vulkan_rendertarget.h"
#include "vulkan/vulkan_swapchain.h"
#include "vulkan/vulkan_texture.h"
#include "x11_windowed_backend.h"
#include "x11_windowed_output.h"

namespace KWin
{

static constexpr VkImageUsageFlags s_outputUsage = VK_IMAGE_USAGE_STORAGE_BIT
    | VK_IMAGE_USAGE_SAMPLED_BIT
    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
    | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

static ModifierList outputModifiers(X11WindowedVulkanBackend *backend, X11WindowedOutput *output)
{
    const uint32_t format = backend->backend()->driFormatForDepth(output->depth());
    return backend->device()->computeOutputFormats().value(format).intersected(backend->backend()->driFormats().value(format));
}

X11WindowedVulkanPrimaryLayer::X11WindowedVulkanPrimaryLayer(X11WindowedOutput *output, X11WindowedVulkanBackend *backend)
    : OutputLayer(output, OutputLayerType::Primary)
    , m_output(output)
    , m_backend(backend)
{
}

X11WindowedVulkanPrimaryLayer::~X11WindowedVulkanPrimaryLayer() = default;

std::optional<OutputLayerBeginFrameInfo> X11WindowedVulkanPrimaryLayer::doBeginFrame()
{
    const QSize size = m_output->modeSize();
    const uint32_t format = m_backend->backend()->driFormatForDepth(m_output->depth());
    const ModifierList modifiers = outputModifiers(m_backend, m_output);
    if (size.isEmpty() || modifiers.isEmpty()) {
        return std::nullopt;
    }
    if (!m_swapchain || m_swapchain->size() != size || m_swapchain->format() != format || !modifiers.contains(m_swapchain->modifier())) {
        m_swapchain = VulkanSwapchain::create(m_backend->device(),
                                              m_backend->drmDevice()->allocator(),
                                              size,
                                              format,
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
    const Region fullRepaint(Rect(QPoint(), size));
    const Region repaint = m_damageJournal.accumulate(m_buffer->age(), fullRepaint) | m_output->exposedArea();
    m_output->clearExposedArea();
    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(m_target.get()),
        .repaint = repaint,
    };
}

bool X11WindowedVulkanPrimaryLayer::doEndFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame)
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
        m_damageJournal.clear();
        return false;
    }
    m_output->setPrimaryBuffer(m_buffer->buffer(), completionFence.duplicate());
    m_swapchain->release(m_buffer.get(), std::move(completionFence));
    m_damageJournal.add(damagedDeviceRegion);
    m_target.reset();
    return true;
}

DrmDevice *X11WindowedVulkanPrimaryLayer::scanoutDevice() const
{
    return m_backend->drmDevice();
}

FormatModifierMap X11WindowedVulkanPrimaryLayer::supportedDrmFormats() const
{
    return m_backend->backend()->driFormats();
}

void X11WindowedVulkanPrimaryLayer::releaseBuffers()
{
    m_target.reset();
    m_buffer.reset();
    m_swapchain.reset();
    m_damageJournal.clear();
}

X11WindowedVulkanCursorLayer::X11WindowedVulkanCursorLayer(X11WindowedOutput *output, X11WindowedVulkanBackend *backend)
    : OutputLayer(output, OutputLayerType::CursorOnly)
    , m_output(output)
    , m_backend(backend)
{
}

X11WindowedVulkanCursorLayer::~X11WindowedVulkanCursorLayer() = default;

std::optional<OutputLayerBeginFrameInfo> X11WindowedVulkanCursorLayer::doBeginFrame()
{
    const QSize size = targetRect().size();
    if (size.isEmpty()) {
        return std::nullopt;
    }
    if (!m_texture || m_texture->size() != size) {
        m_texture = VulkanTexture::allocate(m_backend->device(),
                                            vk::Format::eR8G8B8A8Unorm,
                                            size,
                                            vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc,
                                            VulkanQueueRole::Compute);
        if (!m_texture) {
            return std::nullopt;
        }
    }
    m_target = std::make_unique<VulkanRenderTarget>(m_texture.get(), FileDescriptor{}, bufferTransform());
    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(m_target.get()),
        .repaint = Region::infinite(),
    };
}

bool X11WindowedVulkanCursorLayer::doEndFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame)
{
    if (!m_target || !m_texture) {
        return false;
    }
    if (frame) {
        for (auto &query : m_target->takeRenderTimeQueries()) {
            frame->addRenderTimeQuery(std::move(query));
        }
    }
    if (!m_target->takeCompletionFence().isValid()) {
        return false;
    }
    const QImage image = m_texture->download();
    if (image.isNull()) {
        return false;
    }
    m_output->cursor()->update(image, hotspot());
    m_target.reset();
    return true;
}

DrmDevice *X11WindowedVulkanCursorLayer::scanoutDevice() const
{
    return m_backend->drmDevice();
}

FormatModifierMap X11WindowedVulkanCursorLayer::supportedDrmFormats() const
{
    return m_backend->device()->computeOutputFormats();
}

void X11WindowedVulkanCursorLayer::releaseBuffers()
{
    m_target.reset();
    m_texture.reset();
}

X11WindowedVulkanBackend::X11WindowedVulkanBackend(X11WindowedBackend *backend)
    : VulkanBackend(backend->renderDevice())
    , m_backend(backend)
{
    connect(m_backend, &X11WindowedBackend::outputAdded, this, &X11WindowedVulkanBackend::createOutputLayers);
    for (BackendOutput *output : m_backend->outputs()) {
        createOutputLayers(output);
    }
}

X11WindowedVulkanBackend::~X11WindowedVulkanBackend()
{
    for (BackendOutput *output : m_backend->outputs()) {
        static_cast<X11WindowedOutput *>(output)->setOutputLayers({});
    }
}

X11WindowedBackend *X11WindowedVulkanBackend::backend() const
{
    return m_backend;
}

void X11WindowedVulkanBackend::createOutputLayers(BackendOutput *output)
{
    auto x11Output = static_cast<X11WindowedOutput *>(output);
    std::vector<std::unique_ptr<OutputLayer>> layers;
    layers.push_back(std::make_unique<X11WindowedVulkanPrimaryLayer>(x11Output, this));
    layers.push_back(std::make_unique<X11WindowedVulkanCursorLayer>(x11Output, this));
    x11Output->setOutputLayers(std::move(layers));
}

QList<OutputLayer *> X11WindowedVulkanBackend::compatibleOutputLayers(BackendOutput *output)
{
    return static_cast<X11WindowedOutput *>(output)->outputLayers();
}

} // namespace KWin

#include "moc_x11_windowed_vulkan_backend.cpp"
