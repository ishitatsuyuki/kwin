/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2023-2026 Xaver Hugl <xaver.hugl@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once
#include "core/region.h"
#include "kwin_export.h"
#include "utils/filedescriptor.h"
#include "vulkan_device.h"

#include <QImage>
#include <QMetaObject>
#include <QSize>
#include <vulkan/vulkan_raii.hpp>

namespace KWin
{

class VulkanDevice;
class VulkanUploadManager;

class KWIN_EXPORT VulkanTexture
{
public:
    static std::optional<vk::Format> qImageToVulkanFormat(QImage::Format format);
    static vk::ComponentMapping qImageToComponentMapping(QImage::Format format);
    static std::unique_ptr<VulkanTexture> allocate(VulkanDevice *device, vk::Format format, const QSize &size, vk::ImageUsageFlags usage,
                                                   VulkanQueueRole queueRole = VulkanQueueRole::Graphics,
                                                   vk::ComponentMapping componentMapping = {});
    static std::unique_ptr<VulkanTexture> upload(VulkanDevice *device, const QImage &image, vk::ImageUsageFlags usage,
                                                 VulkanQueueRole queueRole = VulkanQueueRole::Graphics);

    explicit VulkanTexture(VulkanDevice *device, vk::Format format, vk::raii::Image &&image,
                           std::vector<vk::raii::DeviceMemory> &&memory, const QSize &size,
                           VulkanQueueRole queueRole = VulkanQueueRole::Graphics,
                           bool external = false,
                           vk::ComponentMapping componentMapping = {});
    VulkanTexture(VulkanTexture &&other) = delete;
    VulkanTexture(const VulkanTexture &) = delete;
    ~VulkanTexture();

    /**
     * NOTE the format and size have to match in order for the update to work
     */
    bool update(const QImage &img, const Region &region, const QPoint &offset = QPoint());
    /**
     * NOTE the format and size have to match in order for the update to work
     */
    bool update(const QImage &img);

    QImage download() const;

    const vk::raii::Image &handle() const;
    vk::Format format() const;
    QSize size() const;
    VulkanQueueRole queueRole() const;
    bool isExternal() const;
    vk::ComponentMapping componentMapping() const;
    bool supportsUpload(const QImage &image) const;
    VulkanUploadManager *pendingUploadManager() const;

private:
    friend class VulkanUploadManager;

    VulkanDevice *m_device;
    vk::Format m_format;
    std::vector<vk::raii::DeviceMemory> m_memory;
    vk::raii::Image m_image;
    QSize m_size;
    VulkanQueueRole m_queueRole;
    bool m_external;
    vk::ComponentMapping m_componentMapping;
    VulkanUploadManager *m_pendingUploadManager = nullptr;
    QMetaObject::Connection m_deviceLostConnection;
};

/**
 * Damage-sized, persistently mapped staging uploads recorded into a compositor
 * submission. Three independently fenced slots allow normal frame overlap.
 */
class KWIN_EXPORT VulkanUploadManager
{
public:
    explicit VulkanUploadManager(VulkanDevice *device);
    ~VulkanUploadManager();

    VulkanUploadManager(const VulkanUploadManager &) = delete;
    VulkanUploadManager &operator=(const VulkanUploadManager &) = delete;

    bool upload(VulkanTexture *texture, const QImage &image, const Region &region);
    bool record(vk::raii::CommandBuffer &commandBuffer);
    void submitted(const FileDescriptor &completionFence);
    void submissionFailed();
    bool hasPendingUploads() const;

private:
    friend class VulkanTexture;

    void forget(VulkanTexture *texture);

    struct Private;
    std::unique_ptr<Private> d;
};

}
