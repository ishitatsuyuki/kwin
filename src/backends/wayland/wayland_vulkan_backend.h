/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "utils/damagejournal.h"
#include "vulkan/vulkan_backend.h"
#include "wayland_layer.h"

namespace KWin
{

class VulkanRenderTarget;
class VulkanSwapchain;
class VulkanSwapchainSlot;

namespace Wayland
{

class WaylandBackend;
class WaylandOutput;
class WaylandVulkanBackend;

class WaylandVulkanLayer : public WaylandLayer
{
public:
    WaylandVulkanLayer(WaylandOutput *output, WaylandVulkanBackend *backend, OutputLayerType type, int zpos);
    ~WaylandVulkanLayer() override;

    std::optional<OutputLayerBeginFrameInfo> doBeginFrame() override;
    bool doEndFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame) override;
    bool earlyScanoutChecks() override;
    bool importScanoutBuffer(GraphicsBuffer *buffer, const std::shared_ptr<OutputFrame> &frame) override;
    DrmDevice *scanoutDevice() const override;
    FormatModifierMap supportedDrmFormats() const override;
    void releaseBuffers() override;

private:
    WaylandVulkanBackend *const m_backend;
    std::unique_ptr<VulkanSwapchain> m_swapchain;
    std::shared_ptr<VulkanSwapchainSlot> m_buffer;
    std::unique_ptr<VulkanRenderTarget> m_target;
    DamageJournal m_damageJournal;
};

class WaylandVulkanCursorLayer : public OutputLayer
{
public:
    WaylandVulkanCursorLayer(WaylandOutput *output, WaylandVulkanBackend *backend);
    ~WaylandVulkanCursorLayer() override;

    std::optional<OutputLayerBeginFrameInfo> doBeginFrame() override;
    bool doEndFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame) override;
    DrmDevice *scanoutDevice() const override;
    FormatModifierMap supportedDrmFormats() const override;
    void releaseBuffers() override;

private:
    WaylandVulkanBackend *const m_backend;
    std::unique_ptr<VulkanSwapchain> m_swapchain;
    std::shared_ptr<VulkanSwapchainSlot> m_buffer;
    std::unique_ptr<VulkanRenderTarget> m_target;
};

class WaylandVulkanBackend : public VulkanBackend
{
    Q_OBJECT

public:
    explicit WaylandVulkanBackend(WaylandBackend *backend);
    ~WaylandVulkanBackend() override;

    WaylandBackend *backend() const;
    QList<OutputLayer *> compatibleOutputLayers(BackendOutput *output) override;

private:
    void createOutputLayers(BackendOutput *output);

    WaylandBackend *const m_backend;
};

} // namespace Wayland
} // namespace KWin
