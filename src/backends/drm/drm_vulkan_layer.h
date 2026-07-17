/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "drm_layer.h"
#include "utils/damagejournal.h"

namespace KWin
{

class DrmVulkanBackend;
class DrmVirtualOutput;
class MultiGpuSwapchain;
class VulkanRenderTarget;
class VulkanSwapchain;
class VulkanSwapchainSlot;

class DrmVulkanLayer : public DrmPipelineLayer
{
public:
    DrmVulkanLayer(DrmVulkanBackend *backend, DrmPlane *plane);
    DrmVulkanLayer(DrmVulkanBackend *backend, DrmGpu *gpu, DrmPlane::TypeIndex type);
    ~DrmVulkanLayer() override;

    std::optional<OutputLayerBeginFrameInfo> doBeginFrame() override;
    bool doEndFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame) override;
    bool preparePresentationTest() override;
    std::shared_ptr<DrmFramebuffer> currentBuffer() const override;
    void releaseBuffers() override;

private:
    bool earlyScanoutChecks() override;
    bool importScanoutBuffer(GraphicsBuffer *buffer, const std::shared_ptr<OutputFrame> &frame) override;
    bool ensureSwapchain();
    bool isMultiGpu() const;

    DrmVulkanBackend *const m_backend;
    DrmGpu *const m_gpu;
    std::unique_ptr<VulkanSwapchain> m_swapchain;
    std::shared_ptr<VulkanSwapchainSlot> m_current;
    std::shared_ptr<DrmFramebuffer> m_currentFramebuffer;
    std::shared_ptr<DrmFramebuffer> m_scanoutBuffer;
    std::unique_ptr<VulkanRenderTarget> m_target;
    std::unique_ptr<MultiGpuSwapchain> m_importSwapchain;
    std::optional<ColorPipeline> m_outputColorPipeline;
    DamageJournal m_damageJournal;
};

class DrmVirtualVulkanLayer : public DrmOutputLayer
{
public:
    DrmVirtualVulkanLayer(DrmVulkanBackend *backend, DrmVirtualOutput *output);
    ~DrmVirtualVulkanLayer() override;

    std::optional<OutputLayerBeginFrameInfo> doBeginFrame() override;
    bool doEndFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame) override;
    DrmDevice *scanoutDevice() const override;
    FormatModifierMap supportedDrmFormats() const override;
    void releaseBuffers() override;

private:
    DrmVulkanBackend *const m_backend;
    std::unique_ptr<VulkanSwapchain> m_swapchain;
    std::shared_ptr<VulkanSwapchainSlot> m_current;
    std::unique_ptr<VulkanRenderTarget> m_target;
    DamageJournal m_damageJournal;
};

} // namespace KWin
