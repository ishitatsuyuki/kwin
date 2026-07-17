/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Xaver Hugl <xaver.hugl@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <chrono>

#include "core/renderbackend.h"
#include "kwin_export.h"
#include "vulkan/vulkan_device.h"

namespace KWin
{

class KWIN_EXPORT VulkanRenderTimeQuery : public QObject, public RenderTimeQuery
{
    Q_OBJECT

public:
    explicit VulkanRenderTimeQuery(VulkanDevice *device, vk::raii::QueryPool &&pool, vk::PipelineStageFlags2 stage);
    ~VulkanRenderTimeQuery();

    void end(vk::raii::CommandBuffer &buffer);

    /**
     * fetches the result of the query. If rendering is not done yet, this will block!
     */
    std::optional<RenderTimeSpan> query() override;
    /**
     * Returns only the elapsed GPU timestamp interval. Unlike query(), this
     * does not include command-recording time and is intended for profiling.
     */
    std::optional<std::chrono::nanoseconds> gpuDuration();

    static std::unique_ptr<VulkanRenderTimeQuery> begin(VulkanDevice *device,
                                                        vk::raii::CommandBuffer &buffer,
                                                        uint32_t queueFamily,
                                                        vk::PipelineStageFlags2 stage);

private:
    void reset();

    VulkanDevice *m_device = nullptr;
    vk::raii::QueryPool m_pool;
    const vk::PipelineStageFlags2 m_stage;

    struct
    {
        std::chrono::steady_clock::time_point start;
        std::chrono::steady_clock::time_point end;
    } m_cpuProbe;
    std::optional<RenderTimeSpan> m_result;
    std::optional<std::chrono::nanoseconds> m_gpuDuration;
};

}
