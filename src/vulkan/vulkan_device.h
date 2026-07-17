/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2023-2026 Xaver Hugl <xaver.hugl@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once
#include "core/drm_formats.h"
#include "kwin_export.h"
#include "utils/filedescriptor.h"

#include <QHash>
#include <QObject>
#include <QSize>
#include <QVector>
#include <deque>
#include <memory>
#include <optional>
#include <vulkan/vulkan_raii.hpp>

namespace KWin
{

class VulkanTexture;
class GraphicsBuffer;
struct DmaBufAttributes;
class RenderDevice;

enum class VulkanQueueRole {
    Graphics,
    Compute,
    Concurrent,
};

class KWIN_EXPORT VulkanDevice : public QObject
{
    Q_OBJECT

public:
    explicit VulkanDevice(vk::raii::PhysicalDevice physicalDevice, vk::raii::Device &&logicalDevice,
                          std::vector<VkQueueFamilyProperties> &&queueProperties, vk::PhysicalDeviceType type,
                          uint32_t computeQueueFamily, bool highPriorityComputeQueue, bool hostQueryReset);
    VulkanDevice(VulkanDevice &&other) = delete;
    VulkanDevice(const VulkanDevice &) = delete;
    ~VulkanDevice();

    std::shared_ptr<VulkanTexture> importBuffer(GraphicsBuffer *buffer, VkImageUsageFlags usage);
    std::shared_ptr<VulkanTexture> importBufferPlane(GraphicsBuffer *buffer, uint32_t plane, uint32_t drmFormat, const QSize &size, VkImageUsageFlags usage);

    bool isSoftwareRenderer() const;
    vk::PhysicalDeviceType type() const;

    vk::raii::DeviceMemory allocateMemory(const vk::ImageCreateInfo &imageInfo, vk::MemoryPropertyFlags memoryProperties);
    vk::raii::DeviceMemory allocateMemory(const vk::BufferCreateInfo &bufferInfo, vk::MemoryPropertyFlags memoryProperties);
    // Preferred properties are used when a compatible type exposes all of
    // them; otherwise allocation falls back to the first type satisfying the
    // required properties.
    vk::raii::DeviceMemory allocateMemory(const vk::BufferCreateInfo &bufferInfo,
                                          vk::MemoryPropertyFlags requiredMemoryProperties,
                                          vk::MemoryPropertyFlags preferredMemoryProperties);

    const FormatModifierMap &supportedFormats() const;
    const FormatModifierMap &computeOutputFormats() const;
    const vk::raii::Device &logicalDevice() const;

    const vk::raii::Queue &graphicsQueue() const;
    uint32_t graphicsQueueFamily() const;
    const vk::raii::Queue &computeQueue() const;
    uint32_t computeQueueFamily() const;
    bool hasDedicatedComputeQueue() const;
    bool hasHighPriorityComputeQueue() const;
    bool hasHostQueryReset() const;
    std::span<const VkQueueFamilyProperties> queueFamilyProperties() const;
    float nanosecondsPerQueryTick() const;

    vk::raii::CommandBuffer createCommandBuffer();
    vk::raii::CommandBuffer createComputeCommandBuffer();
    std::optional<vk::raii::Semaphore> importSemaphore(FileDescriptor &&syncFd) const;

    std::optional<FileDescriptor> submit(vk::raii::CommandBuffer &&buffer, FileDescriptor &&syncFd);
    std::optional<FileDescriptor> submitCompute(vk::raii::CommandBuffer &&buffer, FileDescriptor &&syncFd);
    /**
     * NOTE avoid using this if at all possible, it's obviously terrible for performance!
     */
    void waitIdle();
    /**
     * Waits only for compositor work. On devices with a dedicated compute queue,
     * graphics submissions remain able to make progress independently.
     */
    void waitComputeIdle();

    /**
     * Handle the "VK_ERROR_DEVICE_LOST" error by flagging this device as lost and releasing
     * all resources related to it. The render device will later delete this device and
     * (attempt to) create a new one.
     *
     * The error can happen with (swapchain and presentation related commands excluded):
     * - vkCreateDevice
     * - vkQueueSubmit
     * - vkGetFenceStatus
     * - vkWaitForFences
     * - vkGetSemaphoreCounterValue
     * - vkWaitSemaphoresKHR
     * - vkGetEventStatus
     * - vkQueueWaitIdle
     * - vkDeviceWaitIdle
     * - vkGetQueryPoolResults
     * - vkQueueBindSparse
     */
    void handleDeviceLoss();

Q_SIGNALS:
    /**
     * This signal is emitted when the associated Vulkan device has been
     * lost, and before it is deleted. In response, all Vulkan resources
     * of this device must be released.
     * If creating a new Vulkan device is successful, resources can be
     * re-created at a later time.
     */
    void deviceLost();

private:
    struct SubmittedCommand
    {
        vk::raii::Semaphore waitSemaphore;
        vk::raii::CommandBuffer buffer;
        FileDescriptor completionSyncFd;
    };

    void getQueues();
    void createCommandPools();
    vk::raii::CommandBuffer createCommandBuffer(vk::raii::CommandPool &pool, std::deque<SubmittedCommand> &submissions);
    std::optional<FileDescriptor> submit(vk::raii::CommandBuffer &&buffer, FileDescriptor &&syncFd,
                                         const vk::raii::Queue &queue, std::deque<SubmittedCommand> &submissions);
    FormatModifierMap queryFormats(VkImageUsageFlags flags) const;
    std::optional<uint32_t> findMemoryType(uint32_t typeBits,
                                           vk::MemoryPropertyFlags requiredMemoryProperties,
                                           vk::MemoryPropertyFlags preferredMemoryProperties = {}) const;
    std::shared_ptr<VulkanTexture> importDmabuf(const DmaBufAttributes *attributes, VkImageUsageFlags usage,
                                                int plane = -1, uint32_t planeFormat = 0, const QSize &planeSize = QSize{});

    vk::PhysicalDeviceType m_type;
    vk::raii::PhysicalDevice m_physical;
    vk::raii::Device m_logical;
    FormatModifierMap m_formats;
    FormatModifierMap m_computeOutputFormats;
    std::vector<VkQueueFamilyProperties> m_queueProperties;
    vk::raii::Queue m_graphicsQueue;
    vk::raii::Queue m_computeQueue;
    vk::raii::CommandPool m_graphicsCommandPool;
    vk::raii::CommandPool m_computeCommandPool;
    uint32_t m_graphicsQueueFamilyIndex;
    uint32_t m_computeQueueFamilyIndex;
    vk::PhysicalDeviceMemoryProperties m_memoryProperties;
    std::deque<SubmittedCommand> m_graphicsSubmissions;
    std::deque<SubmittedCommand> m_computeSubmissions;
    vk::PhysicalDeviceLimits m_deviceLimits;

    struct ImportedTexture
    {
        VkImageUsageFlags usage;
        int plane = -1;
        std::shared_ptr<VulkanTexture> texture;
    };
    QHash<GraphicsBuffer *, std::vector<ImportedTexture>> m_importedTextures;
    const bool m_highPriorityComputeQueue;
    const bool m_hostQueryReset;
    bool m_lost = false;
};

}
