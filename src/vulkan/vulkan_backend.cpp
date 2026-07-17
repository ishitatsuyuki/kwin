/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "vulkan_backend.h"

#include "core/drmdevice.h"
#include "core/renderdevice.h"
#include "opengl/eglcontext.h"
#include "vulkan_device.h"
#include "wayland/linuxdmabufv1clientbuffer.h"
#include "wayland_server.h"

namespace KWin
{

VulkanBackend::VulkanBackend(RenderDevice *renderDevice)
    : m_renderDevice(renderDevice)
    , m_device(renderDevice->vulkanDevice())
{
}

VulkanBackend::~VulkanBackend() = default;

CompositingType VulkanBackend::compositingType() const
{
    return VulkanCompositing;
}

bool VulkanBackend::checkGraphicsReset()
{
    // Device recreation is queued so that deviceLost observers can drop all
    // their resources while the old VkDevice is still alive. Keep rendering
    // disabled by the lost compositor during that short interval, then force
    // a full backend restart once the replacement is installed.
    return !m_renderDevice->isInReset() && m_renderDevice->vulkanDevice() != m_device;
}

DrmDevice *VulkanBackend::drmDevice() const
{
    return m_renderDevice->drmDevice();
}

bool VulkanBackend::testImportBuffer(GraphicsBuffer *buffer)
{
    return device() && device()->importBuffer(buffer, VK_IMAGE_USAGE_SAMPLED_BIT);
}

FormatModifierMap VulkanBackend::supportedFormats() const
{
    return device() ? device()->supportedFormats() : FormatModifierMap{};
}

void VulkanBackend::initWayland()
{
    if (!WaylandServer::self()) {
        return;
    }

    const QList<LinuxDmaBufV1Feedback::Tranche> tranches{{
        .device = drmDevice()->deviceId(),
        .flags = LinuxDmaBufV1Feedback::TrancheFlag::Sampling,
        .formatTable = supportedFormats(),
    }};
    LinuxDmaBufV1ClientBufferIntegration *dmabuf = waylandServer()->linuxDmabuf();
    dmabuf->setRenderBackend(this);
    dmabuf->setSupportedFormatsWithModifiers(tranches);

    waylandServer()->setRenderBackend(this);
}

RenderDevice *VulkanBackend::renderDevice() const
{
    return m_renderDevice;
}

VulkanDevice *VulkanBackend::device() const
{
    return m_device;
}

EglContext *VulkanBackend::openglContext() const
{
    if (!m_openglContext || m_openglContext->isFailed()) {
        m_openglContext = m_renderDevice->eglContext();
    }
    return m_openglContext && !m_openglContext->isFailed() ? m_openglContext.get() : nullptr;
}

} // namespace KWin

#include "moc_vulkan_backend.cpp"
