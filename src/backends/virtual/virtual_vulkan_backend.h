/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/outputlayer.h"
#include "utils/damagejournal.h"
#include "vulkan/vulkan_backend.h"

namespace KWin
{

class VirtualBackend;
class VulkanRenderTarget;
class VulkanSwapchain;
class VulkanSwapchainSlot;

class KWIN_EXPORT VirtualVulkanLayer : public OutputLayer
{
public:
    VirtualVulkanLayer(BackendOutput *output, VulkanBackend *backend);
    ~VirtualVulkanLayer() override;

    std::optional<OutputLayerBeginFrameInfo> doBeginFrame() override;
    bool doEndFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame) override;

    DrmDevice *scanoutDevice() const override;
    FormatModifierMap supportedDrmFormats() const override;
    void releaseBuffers() override;

    VulkanTexture *texture() const;

private:
    VulkanBackend *const m_backend;
    std::unique_ptr<VulkanSwapchain> m_swapchain;
    std::shared_ptr<VulkanSwapchainSlot> m_current;
    std::unique_ptr<VulkanRenderTarget> m_target;
    DamageJournal m_damageJournal;
};

class KWIN_EXPORT VirtualVulkanBackend : public VulkanBackend
{
    Q_OBJECT

public:
    explicit VirtualVulkanBackend(VirtualBackend *backend);
    ~VirtualVulkanBackend() override;

    QList<OutputLayer *> compatibleOutputLayers(BackendOutput *output) override;

private:
    void addOutput(BackendOutput *output);

    VirtualBackend *const m_backend;
};

} // namespace KWin
