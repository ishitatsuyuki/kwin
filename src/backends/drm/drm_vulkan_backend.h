/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "drm_render_backend.h"
#include "vulkan/vulkan_backend.h"

namespace KWin
{

class DrmBackend;

class DrmVulkanBackend : public VulkanBackend, public DrmRenderBackend
{
    Q_OBJECT

public:
    explicit DrmVulkanBackend(DrmBackend *backend);
    ~DrmVulkanBackend() override;

    DrmBackend *backend() const;
    QList<OutputLayer *> compatibleOutputLayers(BackendOutput *output) override;
    std::unique_ptr<DrmPipelineLayer> createDrmPlaneLayer(DrmPlane *plane) override;
    std::unique_ptr<DrmPipelineLayer> createDrmPlaneLayer(DrmGpu *gpu, DrmPlane::TypeIndex type) override;
    std::unique_ptr<DrmOutputLayer> createLayer(DrmVirtualOutput *output) override;

private:
    DrmBackend *const m_backend;
};

} // namespace KWin
