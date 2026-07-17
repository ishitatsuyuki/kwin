/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "drm_vulkan_layer.h"

#include "core/drmdevice.h"
#include "core/graphicsbuffer.h"
#include "core/renderbackend.h"
#include "core/renderdevice.h"
#include "drm_backend.h"
#include "drm_buffer.h"
#include "drm_crtc.h"
#include "drm_gpu.h"
#include "drm_output.h"
#include "drm_pipeline.h"
#include "drm_virtual_output.h"
#include "drm_vulkan_backend.h"
#include "multigpuswapchain.h"
#include "utils/envvar.h"
#include "vulkan/vulkan_device.h"
#include "vulkan/vulkan_rendertarget.h"
#include "vulkan/vulkan_swapchain.h"

#include <drm_fourcc.h>

namespace KWin
{

static constexpr VkImageUsageFlags s_outputUsage = VK_IMAGE_USAGE_STORAGE_BIT
    | VK_IMAGE_USAGE_SAMPLED_BIT
    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
    | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

static FormatModifierMap intersectFormats(const FormatModifierMap &first, const FormatModifierMap &second)
{
    FormatModifierMap ret;
    for (auto it = first.constBegin(); it != first.constEnd(); ++it) {
        const ModifierList modifiers = it.value().intersected(second.value(it.key()));
        if (!modifiers.isEmpty()) {
            ret.insert(it.key(), modifiers);
        }
    }
    return ret;
}

bool DrmVulkanLayer::isMultiGpu() const
{
    return m_gpu != m_backend->backend()->primaryGpu();
}

DrmVulkanLayer::DrmVulkanLayer(DrmVulkanBackend *backend, DrmPlane *plane)
    : DrmPipelineLayer(plane)
    , m_backend(backend)
    , m_gpu(plane->gpu())
{
}

DrmVulkanLayer::DrmVulkanLayer(DrmVulkanBackend *backend, DrmGpu *gpu, DrmPlane::TypeIndex type)
    : DrmPipelineLayer(type)
    , m_backend(backend)
    , m_gpu(gpu)
{
}

DrmVulkanLayer::~DrmVulkanLayer() = default;

bool DrmVulkanLayer::ensureSwapchain()
{
    const QSize size = targetRect().size();
    FormatModifierMap formats;
    if (isMultiGpu()) {
        if (!m_gpu->renderDevice()) {
            return false;
        }
        formats = intersectFormats(m_backend->device()->computeOutputFormats(),
                                   m_gpu->renderDevice()->allImportableFormats());
        formats = intersectFormats(formats, supportedDrmFormats());
    } else {
        formats = intersectFormats(m_backend->device()->computeOutputFormats(), supportedDrmFormats());
    }
    const QList<FormatInfo> candidates = filterAndSortFormats(formats, m_requiredAlphaBits, drmOutput()->colorPowerTradeoff());
    if (size.isEmpty() || candidates.isEmpty()) {
        return false;
    }
    if (m_swapchain && m_swapchain->size() == size
        && formats.value(m_swapchain->format()).contains(m_swapchain->modifier())
        && (!isMultiGpu() || (m_importSwapchain && !m_importSwapchain->needsRecreation()))) {
        return true;
    }
    m_target.reset();
    m_current.reset();
    m_currentFramebuffer.reset();
    m_importSwapchain.reset();
    for (const FormatInfo &candidate : candidates) {
        auto swapchain = VulkanSwapchain::create(m_backend->device(),
                                                 m_backend->drmDevice()->allocator(),
                                                 size,
                                                 candidate.drmFormat,
                                                 formats.value(candidate.drmFormat),
                                                 s_outputUsage);
        if (!swapchain) {
            continue;
        }
        std::unique_ptr<MultiGpuSwapchain> importSwapchain;
        if (isMultiGpu()) {
            importSwapchain = MultiGpuSwapchain::create(m_gpu->renderDevice(),
                                                        m_gpu->drmDevice(),
                                                        swapchain->format(),
                                                        swapchain->modifier(),
                                                        size,
                                                        supportedDrmFormats());
            if (!importSwapchain) {
                continue;
            }
        }
        m_swapchain = std::move(swapchain);
        m_importSwapchain = std::move(importSwapchain);
        break;
    }
    m_damageJournal.clear();
    return bool(m_swapchain);
}

std::optional<OutputLayerBeginFrameInfo> DrmVulkanLayer::doBeginFrame()
{
    m_scanoutBuffer.reset();
    m_currentFramebuffer.reset();
    if (!ensureSwapchain()) {
        return std::nullopt;
    }
    m_current = m_swapchain->acquire();
    if (!m_current) {
        return std::nullopt;
    }
    std::optional<ColorPipeline> outputColorPipeline;
    auto renderColor = colorDescription();
    if (drmOutput()->needsShadowBuffer()) {
        renderColor = drmOutput()->blendingColor();
        if (const auto &profile = pipeline()->iccProfile()) {
            outputColorPipeline = ColorPipeline::createIcc(profile,
                                                           renderColor,
                                                           drmOutput()->wireColor(drmOutput()->nextState()),
                                                           drmOutput()->wireTransfer(drmOutput()->nextState()),
                                                           RenderingIntent::AbsoluteColorimetricNoAdaptation);
        } else {
            outputColorPipeline = ColorPipeline::create(renderColor,
                                                        colorDescription(),
                                                        RenderingIntent::AbsoluteColorimetricNoAdaptation);
        }
    }
    if (m_outputColorPipeline != outputColorPipeline) {
        m_damageJournal.clear();
        m_outputColorPipeline = outputColorPipeline;
    }
    m_target = std::make_unique<VulkanRenderTarget>(m_current->texture(),
                                                    m_current->releaseFd().duplicate(),
                                                    bufferTransform(),
                                                    m_outputColorPipeline ? &*m_outputColorPipeline : nullptr);
    const Region fallback(Rect(QPoint(), targetRect().size()));
    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(m_target.get(), renderColor),
        .repaint = m_damageJournal.accumulate(m_current->age(), fallback),
    };
}

bool DrmVulkanLayer::doEndFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame)
{
    if (!m_target || !m_current || !m_swapchain) {
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
    if (m_importSwapchain) {
        auto imported = m_importSwapchain->copyRgbBuffer(m_current->buffer(),
                                                         damagedDeviceRegion,
                                                         completionFence.duplicate(),
                                                         frame,
                                                         m_current->releasePoint());
        m_swapchain->release(m_current.get(), std::move(completionFence));
        if (imported) {
            m_currentFramebuffer = m_gpu->importBuffer(imported->buffer, std::move(imported->sync));
        }
    } else {
        m_currentFramebuffer = m_gpu->importBuffer(m_current->buffer(), completionFence.duplicate());
        m_swapchain->release(m_current.get(), std::move(completionFence));
    }
    if (!m_currentFramebuffer) {
        return false;
    }
    m_damageJournal.add(damagedDeviceRegion);
    m_target.reset();
    return true;
}

bool DrmVulkanLayer::preparePresentationTest()
{
    if (m_type != OutputLayerType::Primary && drmOutput()->shouldDisableNonPrimaryPlanes()) {
        return false;
    }
    m_scanoutBuffer.reset();
    if (!ensureSwapchain()) {
        return false;
    }
    m_current = m_swapchain->acquire();
    if (!m_current) {
        return false;
    }
    if (m_importSwapchain) {
        auto imported = m_importSwapchain->copyRgbBuffer(m_current->buffer(), Region::infinite(), FileDescriptor{}, nullptr, m_current->releasePoint());
        if (imported) {
            m_currentFramebuffer = m_gpu->importBuffer(imported->buffer, std::move(imported->sync));
        }
    } else {
        m_currentFramebuffer = m_gpu->importBuffer(m_current->buffer(), FileDescriptor{});
    }
    m_swapchain->release(m_current.get(), FileDescriptor{});
    return bool(m_currentFramebuffer);
}

std::shared_ptr<DrmFramebuffer> DrmVulkanLayer::currentBuffer() const
{
    return m_scanoutBuffer ? m_scanoutBuffer : m_currentFramebuffer;
}

static const auto s_allowHardwareRotation = environmentVariableBoolValue("KWIN_ENABLE_HW_ROTATION");
static const bool s_directScanoutDisabled = environmentVariableBoolValue("KWIN_DRM_NO_DIRECT_SCANOUT").value_or(false);

bool DrmVulkanLayer::earlyScanoutChecks()
{
    if (s_directScanoutDisabled) {
        return false;
    }
    if (m_type != OutputLayerType::Primary && drmOutput()->shouldDisableNonPrimaryPlanes()) {
        return false;
    }
    if (gpu()->needsModeset() || drmOutput()->needsShadowBuffer()) {
        return false;
    }
    if (!m_colorPipeline.isIdentity()) {
        if (!m_plane || drmOutput()->colorPowerTradeoff() == BackendOutput::ColorPowerTradeoff::PreferAccuracy) {
            return false;
        }
        const auto pipelines = m_plane->colorPipelines();
        const bool match = std::ranges::any_of(pipelines, [this](DrmColorOp *colorop) {
            return colorop->colorOp()->matchPipeline(gpu(), m_colorPipeline);
        });
        if (!match) {
            return false;
        }
    }
    if (sourceRect() != sourceRect().toRect()) {
        return false;
    }
    if (offloadTransform() != OutputTransform::Kind::Normal) {
        if (!s_allowHardwareRotation.value_or(!gpu()->drmDevice()->isAmdgpu() || gpu()->addFB2ModifiersSupported())) {
            return false;
        }
        if (!m_plane || !m_plane->supportsTransformation(offloadTransform())) {
            return false;
        }
    }
    return true;
}

bool DrmVulkanLayer::importScanoutBuffer(GraphicsBuffer *buffer, const std::shared_ptr<OutputFrame> &frame)
{
    const DmaBufAttributes *attributes = buffer->dmabufAttributes();
    if (!attributes) {
        return false;
    }
    if (attributes->device != gpu()->drmDevice()->deviceId()
        && (!gpu()->renderDevice() || attributes->device != gpu()->renderDevice()->drmDevice()->deviceId())) {
        return false;
    }
    m_scanoutBuffer = gpu()->importBuffer(buffer, FileDescriptor{});
    if (m_scanoutBuffer) {
        m_damageJournal.clear();
        if (m_importSwapchain) {
            m_importSwapchain->resetDamageTracking();
        }
    }
    return bool(m_scanoutBuffer);
}

void DrmVulkanLayer::releaseBuffers()
{
    m_target.reset();
    m_current.reset();
    m_currentFramebuffer.reset();
    m_scanoutBuffer.reset();
    m_importSwapchain.reset();
    m_outputColorPipeline.reset();
    m_swapchain.reset();
    m_damageJournal.clear();
}

DrmVirtualVulkanLayer::DrmVirtualVulkanLayer(DrmVulkanBackend *backend, DrmVirtualOutput *output)
    : DrmOutputLayer(output, OutputLayerType::Primary)
    , m_backend(backend)
{
}

DrmVirtualVulkanLayer::~DrmVirtualVulkanLayer() = default;

std::optional<OutputLayerBeginFrameInfo> DrmVirtualVulkanLayer::doBeginFrame()
{
    VulkanDevice *device = m_backend->device();
    const QSize size = m_output->modeSize();
    const ModifierList modifiers = device ? device->computeOutputFormats().value(DRM_FORMAT_ABGR8888) : ModifierList{};
    if (modifiers.isEmpty()) {
        return std::nullopt;
    }
    if (!m_swapchain || m_swapchain->size() != size || !modifiers.contains(m_swapchain->modifier())) {
        m_swapchain = VulkanSwapchain::create(device,
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
    m_current = m_swapchain->acquire();
    if (!m_current) {
        return std::nullopt;
    }
    m_target = std::make_unique<VulkanRenderTarget>(m_current->texture(), m_current->releaseFd().duplicate(), bufferTransform());
    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(m_target.get(), colorDescription()),
        .repaint = m_damageJournal.accumulate(m_current->age(), Region(Rect(QPoint(), size))),
    };
}

bool DrmVirtualVulkanLayer::doEndFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame)
{
    if (!m_target || !m_current || !m_swapchain) {
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
    m_swapchain->release(m_current.get(), std::move(completionFence));
    m_damageJournal.add(damagedDeviceRegion);
    m_target.reset();
    return true;
}

DrmDevice *DrmVirtualVulkanLayer::scanoutDevice() const
{
    return m_backend->drmDevice();
}

FormatModifierMap DrmVirtualVulkanLayer::supportedDrmFormats() const
{
    return m_backend->device() ? m_backend->device()->computeOutputFormats() : FormatModifierMap{};
}

void DrmVirtualVulkanLayer::releaseBuffers()
{
    m_target.reset();
    m_current.reset();
    m_swapchain.reset();
    m_damageJournal.clear();
}

} // namespace KWin
