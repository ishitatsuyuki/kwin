/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "drm_vulkan_backend.h"

#include "drm_backend.h"
#include "drm_gpu.h"
#include "drm_output.h"
#include "drm_pipeline.h"
#include "drm_virtual_output.h"
#include "drm_vulkan_layer.h"

namespace KWin
{

DrmVulkanBackend::DrmVulkanBackend(DrmBackend *backend)
    : VulkanBackend(backend->primaryGpu()->renderDevice())
    , m_backend(backend)
{
    m_backend->setRenderBackend(this);
    m_backend->createLayers();
}

DrmVulkanBackend::~DrmVulkanBackend()
{
    m_backend->releaseBuffers();
    m_backend->setRenderBackend(nullptr);
}

DrmBackend *DrmVulkanBackend::backend() const
{
    return m_backend;
}

QList<OutputLayer *> DrmVulkanBackend::compatibleOutputLayers(BackendOutput *output)
{
    if (auto virtualOutput = qobject_cast<DrmVirtualOutput *>(output)) {
        return {virtualOutput->primaryLayer()};
    }
    return static_cast<DrmOutput *>(output)->pipeline()->gpu()->compatibleOutputLayers(output);
}

std::unique_ptr<DrmPipelineLayer> DrmVulkanBackend::createDrmPlaneLayer(DrmPlane *plane)
{
    return std::make_unique<DrmVulkanLayer>(this, plane);
}

std::unique_ptr<DrmPipelineLayer> DrmVulkanBackend::createDrmPlaneLayer(DrmGpu *gpu, DrmPlane::TypeIndex type)
{
    return std::make_unique<DrmVulkanLayer>(this, gpu, type);
}

std::unique_ptr<DrmOutputLayer> DrmVulkanBackend::createLayer(DrmVirtualOutput *output)
{
    return std::make_unique<DrmVirtualVulkanLayer>(this, output);
}

} // namespace KWin

#include "moc_drm_vulkan_backend.cpp"
