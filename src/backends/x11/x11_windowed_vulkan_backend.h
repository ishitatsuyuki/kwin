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

class VulkanRenderTarget;
class VulkanSwapchain;
class VulkanSwapchainSlot;
class VulkanTexture;
class X11WindowedBackend;
class X11WindowedOutput;
class X11WindowedVulkanBackend;

class X11WindowedVulkanPrimaryLayer : public OutputLayer
{
public:
    X11WindowedVulkanPrimaryLayer(X11WindowedOutput *output, X11WindowedVulkanBackend *backend);
    ~X11WindowedVulkanPrimaryLayer() override;

    std::optional<OutputLayerBeginFrameInfo> doBeginFrame() override;
    bool doEndFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame) override;
    DrmDevice *scanoutDevice() const override;
    FormatModifierMap supportedDrmFormats() const override;
    void releaseBuffers() override;

private:
    X11WindowedOutput *const m_output;
    X11WindowedVulkanBackend *const m_backend;
    std::unique_ptr<VulkanSwapchain> m_swapchain;
    std::shared_ptr<VulkanSwapchainSlot> m_buffer;
    std::unique_ptr<VulkanRenderTarget> m_target;
    DamageJournal m_damageJournal;
};

class X11WindowedVulkanCursorLayer : public OutputLayer
{
public:
    X11WindowedVulkanCursorLayer(X11WindowedOutput *output, X11WindowedVulkanBackend *backend);
    ~X11WindowedVulkanCursorLayer() override;

    std::optional<OutputLayerBeginFrameInfo> doBeginFrame() override;
    bool doEndFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame) override;
    DrmDevice *scanoutDevice() const override;
    FormatModifierMap supportedDrmFormats() const override;
    void releaseBuffers() override;

private:
    X11WindowedOutput *const m_output;
    X11WindowedVulkanBackend *const m_backend;
    std::unique_ptr<VulkanTexture> m_texture;
    std::unique_ptr<VulkanRenderTarget> m_target;
};

class X11WindowedVulkanBackend : public VulkanBackend
{
    Q_OBJECT

public:
    explicit X11WindowedVulkanBackend(X11WindowedBackend *backend);
    ~X11WindowedVulkanBackend() override;

    X11WindowedBackend *backend() const;
    QList<OutputLayer *> compatibleOutputLayers(BackendOutput *output) override;

private:
    void createOutputLayers(BackendOutput *output);

    X11WindowedBackend *const m_backend;
};

} // namespace KWin
