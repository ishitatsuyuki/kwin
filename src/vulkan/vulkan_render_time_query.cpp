/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Xaver Hugl <xaver.hugl@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "vulkan_render_time_query.h"

#include <array>

namespace KWin
{

VulkanRenderTimeQuery::VulkanRenderTimeQuery(VulkanDevice *device, vk::raii::QueryPool &&pool, vk::PipelineStageFlags2 stage)
    : m_device(device)
    , m_pool(std::move(pool))
    , m_stage(stage)
{
    m_cpuProbe.start = std::chrono::steady_clock::now();
    connect(device, &VulkanDevice::deviceLost, this, &VulkanRenderTimeQuery::reset);
}

VulkanRenderTimeQuery::~VulkanRenderTimeQuery()
{
}

void VulkanRenderTimeQuery::end(vk::raii::CommandBuffer &buffer)
{
    if (!m_device) {
        return;
    }
    m_cpuProbe.end = std::chrono::steady_clock::now();
    buffer.writeTimestamp2(m_stage, m_pool, 1);
}

std::optional<RenderTimeSpan> VulkanRenderTimeQuery::query()
{
    if (!m_device) {
        return std::nullopt;
    }
    if (!m_result) {
        std::array<uint64_t, 4> timestamps{};
        const vk::Result result = m_pool.getResults(0,
                                                    2,
                                                    sizeof(timestamps),
                                                    timestamps.data(),
                                                    2 * sizeof(uint64_t),
                                                    vk::QueryResultFlagBits::e64
                                                        | vk::QueryResultFlagBits::eWait
                                                        | vk::QueryResultFlagBits::eWithAvailability);
        if (result != vk::Result::eSuccess) {
            reset();
            return std::nullopt;
        }
        const uint64_t gpuTicks = timestamps[2] - timestamps[0];
        m_gpuDuration = std::chrono::nanoseconds(uint64_t(std::round(gpuTicks * m_device->nanosecondsPerQueryTick())));
        m_result = RenderTimeSpan{
            .start = m_cpuProbe.start,
            .end = std::max(m_cpuProbe.end, m_cpuProbe.start + *m_gpuDuration),
        };
    }
    return m_result;
}

std::optional<std::chrono::nanoseconds> VulkanRenderTimeQuery::gpuDuration()
{
    if (!m_result && !query()) {
        return std::nullopt;
    }
    return m_gpuDuration;
}

void VulkanRenderTimeQuery::reset()
{
    m_pool.clear();
    m_device = nullptr;
}

std::unique_ptr<VulkanRenderTimeQuery> VulkanRenderTimeQuery::begin(VulkanDevice *device,
                                                                    vk::raii::CommandBuffer &buffer,
                                                                    uint32_t queueFamily,
                                                                    vk::PipelineStageFlags2 stage)
{
    if (!device->hasHostQueryReset() || !device->queueFamilyProperties()[queueFamily].timestampValidBits) {
        return nullptr;
    }
    auto [result, query] = device->logicalDevice().createQueryPool(vk::QueryPoolCreateInfo{
        vk::QueryPoolCreateFlags{},
        vk::QueryType::eTimestamp,
        2,
    });
    if (result != vk::Result::eSuccess) {
        return nullptr;
    }
    // A command-buffer reset has not necessarily executed when a caller enters
    // vkGetQueryPoolResults with WAIT_BIT. Reset the fresh one-shot pool on the
    // host so both queries are initialized before they can be observed.
    device->logicalDevice().getDispatcher()->vkResetQueryPool(static_cast<VkDevice>(*device->logicalDevice()),
                                                              static_cast<VkQueryPool>(*query),
                                                              0,
                                                              2);
    buffer.writeTimestamp2(stage, query, 0);
    return std::make_unique<VulkanRenderTimeQuery>(device, std::move(query), stage);
}

}
#include "moc_vulkan_render_time_query.cpp"
