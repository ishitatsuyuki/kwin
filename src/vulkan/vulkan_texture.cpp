/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2023-2026 Xaver Hugl <xaver.hugl@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "vulkan_texture.h"
#include "vulkan_device.h"
#include "vulkan_logging.h"

#include <cerrno>
#include <sys/poll.h>
#include <utility>
#include <vulkan/vulkan_to_string.hpp>

namespace KWin
{

static bool waitForSubmission(const std::optional<FileDescriptor> &completion)
{
    if (!completion) {
        return false;
    }

    pollfd pfd{
        .fd = completion->get(),
        .events = POLLIN,
        .revents = 0,
    };
    int ret;
    do {
        ret = poll(&pfd, 1, -1);
    } while (ret < 0 && errno == EINTR);

    return ret == 1 && (pfd.revents & POLLIN);
}

std::optional<vk::Format> VulkanTexture::qImageToVulkanFormat(QImage::Format format)
{
    // TODO support the rest of the formats using swizzles?
    switch (format) {
    case QImage::Format_Alpha8:
    case QImage::Format_Grayscale8:
        return vk::Format::eR8Unorm;
    case QImage::Format_Grayscale16:
        return vk::Format::eR16Unorm;
    case QImage::Format_RGB16:
        return vk::Format::eR5G6B5UnormPack16;
    case QImage::Format_RGB555:
        return vk::Format::eA1R5G5B5UnormPack16;
    case QImage::Format_RGB888:
        return vk::Format::eR8G8B8Unorm;
    case QImage::Format_RGB444:
    case QImage::Format_ARGB4444_Premultiplied:
        return vk::Format::eR4G4B4A4UnormPack16;
    case QImage::Format_RGBX8888:
    case QImage::Format_RGBA8888_Premultiplied:
        return vk::Format::eR8G8B8A8Unorm;
    case QImage::Format_BGR30:
    case QImage::Format_A2BGR30_Premultiplied:
        return vk::Format::eA2B10G10R10UnormPack32;
    case QImage::Format_RGBX64:
    case QImage::Format_RGBA64_Premultiplied:
        return vk::Format::eR16G16B16A16Unorm;
    default:
        return std::nullopt;
    }
}

VulkanTexture::VulkanTexture(VulkanDevice *device, vk::Format format, vk::raii::Image &&image,
                             std::vector<vk::raii::DeviceMemory> &&memory, const QSize &size,
                             VulkanQueueRole queueRole, bool external)
    : m_device(device)
    , m_format(format)
    , m_memory(std::move(memory))
    , m_image(std::move(image))
    , m_size(size)
    , m_queueRole(queueRole)
    , m_external(external)
{
    m_deviceLostConnection = QObject::connect(device, &VulkanDevice::deviceLost, device, [this]() {
        m_image.clear();
        m_memory.clear();
        m_device = nullptr;
    });
}

VulkanTexture::~VulkanTexture()
{
    QObject::disconnect(m_deviceLostConnection);
}

QSize VulkanTexture::size() const
{
    return m_size;
}

const vk::raii::Image &VulkanTexture::handle() const
{
    return m_image;
}

vk::Format VulkanTexture::format() const
{
    return m_format;
}

VulkanQueueRole VulkanTexture::queueRole() const
{
    return m_queueRole;
}

bool VulkanTexture::isExternal() const
{
    return m_external;
}

static QImage::Format vulkanToQImageFormat(vk::Format format)
{
    switch (format) {
    case vk::Format::eR8Unorm:
        return QImage::Format_Grayscale8;
    case vk::Format::eR16Unorm:
        return QImage::Format_Grayscale16;
    case vk::Format::eR8G8B8A8Unorm:
        return QImage::Format_RGBA8888_Premultiplied;
    case vk::Format::eR16G16B16A16Unorm:
        return QImage::Format_RGBA64_Premultiplied;
    default:
        return QImage::Format_Invalid;
    }
}

QImage VulkanTexture::download() const
{
    if (!m_device || !*m_image) {
        return {};
    }
    const QImage::Format qFormat = vulkanToQImageFormat(m_format);
    if (qFormat == QImage::Format_Invalid) {
        qCWarning(KWIN_VULKAN) << "Unsupported format for download:" << vk::to_string(m_format);
        return {};
    }

    QImage result(m_size, qFormat);
    const uint32_t bytesPerPixel = result.depth() / 8;
    const vk::DeviceSize bufferSize = m_size.width() * m_size.height() * bytesPerPixel;

    // Concurrent and imported images may most recently have been used by a
    // different queue. The regular Graphics and Compute cases are ordered by
    // submission on their respective queue and don't need a device-wide wait.
    if (m_queueRole == VulkanQueueRole::Concurrent || m_external) {
        m_device->waitIdle();
    }

    vk::BufferCreateInfo bufferInfo{
        vk::BufferCreateFlags(),
        bufferSize,
        vk::BufferUsageFlagBits::eTransferDst,
    };
    auto stagingMemory = m_device->allocateMemory(bufferInfo, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    if (!*stagingMemory) {
        return {};
    }
    auto [bufResult, stagingBuffer] = m_device->logicalDevice().createBuffer(bufferInfo);
    if (bufResult != vk::Result::eSuccess) {
        return {};
    }
    stagingBuffer.bindMemory(stagingMemory, 0);

    auto commandBuffer = m_queueRole == VulkanQueueRole::Compute ? m_device->createComputeCommandBuffer() : m_device->createCommandBuffer();
    commandBuffer.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    const uint32_t queueFamily = m_queueRole == VulkanQueueRole::Compute
        ? m_device->computeQueueFamily()
        : m_device->graphicsQueueFamily();
    if (m_external) {
        const vk::ImageMemoryBarrier2 acquireBarrier{
            vk::PipelineStageFlagBits2::eNone,
            vk::AccessFlags2{},
            vk::PipelineStageFlagBits2::eTransfer,
            vk::AccessFlagBits2::eTransferRead,
            vk::ImageLayout::eGeneral,
            vk::ImageLayout::eGeneral,
            vk::QueueFamilyExternal,
            queueFamily,
            *m_image,
            vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        commandBuffer.pipelineBarrier2(vk::DependencyInfo{{}, {}, {}, acquireBarrier});
    }
    vk::BufferImageCopy2 copyRegion{
        0,
        uint32_t(m_size.width()),
        uint32_t(m_size.height()),
        vk::ImageSubresourceLayers{
            vk::ImageAspectFlagBits::eColor,
            0,
            0,
            1,
        },
        vk::Offset3D(0, 0, 0),
        vk::Extent3D(m_size.width(), m_size.height(), 1),
    };
    commandBuffer.copyImageToBuffer2(vk::CopyImageToBufferInfo2{
        *m_image,
        vk::ImageLayout::eGeneral,
        *stagingBuffer,
        copyRegion,
    });
    if (m_external) {
        const vk::ImageMemoryBarrier2 releaseBarrier{
            vk::PipelineStageFlagBits2::eTransfer,
            vk::AccessFlagBits2::eTransferRead,
            vk::PipelineStageFlagBits2::eNone,
            vk::AccessFlags2{},
            vk::ImageLayout::eGeneral,
            vk::ImageLayout::eGeneral,
            queueFamily,
            vk::QueueFamilyExternal,
            *m_image,
            vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        commandBuffer.pipelineBarrier2(vk::DependencyInfo{{}, {}, {}, releaseBarrier});
    }
    commandBuffer.end();

    const auto completion = m_queueRole == VulkanQueueRole::Compute
        ? m_device->submitCompute(std::move(commandBuffer), FileDescriptor{})
        : m_device->submit(std::move(commandBuffer), FileDescriptor{});
    if (!waitForSubmission(completion)) {
        return {};
    }

    // use mapMemory/unmapMemory (Vulkan 1.0) instead of mapMemory2/unmapMemory2 (Vulkan 1.4)
    // for compatibility with lavapipe and other drivers that don't support 1.4
    auto [mapResult, dataPtr] = stagingMemory.mapMemory(0, bufferSize);
    if (mapResult != vk::Result::eSuccess) {
        return {};
    }

    std::memcpy(result.bits(), dataPtr, bufferSize);
    stagingMemory.unmapMemory();

    return result;
}

bool VulkanTexture::update(const QImage &img, const Region &region, const QPoint &offset)
{
    if (!m_device || !*m_image || img.size() != m_size || qImageToVulkanFormat(img.format()) != m_format) {
        return false;
    }
    if (m_queueRole == VulkanQueueRole::Concurrent || m_external) {
        m_device->waitIdle();
    }

    vk::BufferCreateInfo bufferInfo{
        vk::BufferCreateFlags(),
        vk::DeviceSize(img.sizeInBytes()),
        vk::BufferUsageFlagBits::eTransferSrc,
    };
    auto stagingMemory = m_device->allocateMemory(bufferInfo, vk::MemoryPropertyFlagBits::eHostVisible);
    if (!*stagingMemory) {
        return false;
    }
    auto [result, stagingBuffer] = m_device->logicalDevice().createBuffer(bufferInfo);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    stagingBuffer.bindMemory(stagingMemory, 0);
    auto [mapResult, dataPtr] = stagingMemory.mapMemory(0, vk::DeviceSize(img.sizeInBytes()));
    if (mapResult != vk::Result::eSuccess) {
        return false;
    }
    std::memcpy(dataPtr, img.constBits(), img.sizeInBytes());
    stagingMemory.unmapMemory();

    auto commandBuffer = m_queueRole == VulkanQueueRole::Compute ? m_device->createComputeCommandBuffer() : m_device->createCommandBuffer();
    commandBuffer.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    const uint32_t queueFamily = m_queueRole == VulkanQueueRole::Compute
        ? m_device->computeQueueFamily()
        : m_device->graphicsQueueFamily();
    if (m_external) {
        const vk::ImageMemoryBarrier2 acquireBarrier{
            vk::PipelineStageFlagBits2::eNone,
            vk::AccessFlags2{},
            vk::PipelineStageFlagBits2::eTransfer,
            vk::AccessFlagBits2::eTransferWrite,
            vk::ImageLayout::eGeneral,
            vk::ImageLayout::eGeneral,
            vk::QueueFamilyExternal,
            queueFamily,
            *m_image,
            vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        commandBuffer.pipelineBarrier2(vk::DependencyInfo{{}, {}, {}, acquireBarrier});
    }
    const uint32_t bytesPerPixel = img.depth() / 8;
    const auto regions = region.rects() | std::views::transform([&img, &offset, bytesPerPixel](const Rect &rect) {
        return vk::BufferImageCopy2{
            vk::DeviceSize(rect.y() * img.bytesPerLine() + rect.x() * bytesPerPixel), // offset in bytes
            uint32_t(img.width()), // row length
            uint32_t(img.height()), // image height
            vk::ImageSubresourceLayers{
                vk::ImageAspectFlagBits::eColor,
                0, // mip map level
                0, // base array layer
                1, // layer count
            },
            vk::Offset3D(rect.x() + offset.x(), rect.y() + offset.y(), 0),
            vk::Extent3D(rect.width(), rect.height(), 1),
        };
    }) | std::ranges::to<std::vector>();
    commandBuffer.copyBufferToImage2(vk::CopyBufferToImageInfo2{
        *stagingBuffer,
        *m_image,
        vk::ImageLayout::eGeneral,
        regions,
    });
    if (m_external) {
        const vk::ImageMemoryBarrier2 releaseBarrier{
            vk::PipelineStageFlagBits2::eTransfer,
            vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eNone,
            vk::AccessFlags2{},
            vk::ImageLayout::eGeneral,
            vk::ImageLayout::eGeneral,
            queueFamily,
            vk::QueueFamilyExternal,
            *m_image,
            vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        commandBuffer.pipelineBarrier2(vk::DependencyInfo{{}, {}, {}, releaseBarrier});
    }
    commandBuffer.end();
    const auto completion = m_queueRole == VulkanQueueRole::Compute
        ? m_device->submitCompute(std::move(commandBuffer), FileDescriptor{})
        : m_device->submit(std::move(commandBuffer), FileDescriptor{});
    return waitForSubmission(completion);
}

bool VulkanTexture::update(const QImage &img)
{
    return update(img, Region(0, 0, img.width(), img.height()));
}

std::unique_ptr<VulkanTexture> VulkanTexture::allocate(VulkanDevice *device, vk::Format format, const QSize &size, vk::ImageUsageFlags usage,
                                                       VulkanQueueRole queueRole)
{
    vk::ImageCreateInfo info{
        vk::ImageCreateFlags(),
        vk::ImageType::e2D,
        format,
        vk::Extent3D(size.width(), size.height(), 1),
        1, // mipmap levels
        1, // array layers
        vk::SampleCountFlagBits::e1,
        vk::ImageTiling::eOptimal,
        usage,
        vk::SharingMode::eExclusive,
        {}, // queue family indices
        vk::ImageLayout::eUndefined,
    };
    auto memory = device->allocateMemory(info, vk::MemoryPropertyFlagBits::eDeviceLocal);
    if (!*memory) {
        return nullptr;
    }
    auto [result, image] = device->logicalDevice().createImage(info);
    if (result != vk::Result::eSuccess) {
        qCWarning(KWIN_VULKAN) << "creating image failed!" << vk::to_string(result);
        return nullptr;
    }
    image.bindMemory(memory, 0);

    // we will only use the general image layout everywhere else,
    // so transition the image here once and then never again.
    auto commandBuffer = queueRole == VulkanQueueRole::Compute ? device->createComputeCommandBuffer() : device->createCommandBuffer();
    commandBuffer.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    vk::ImageMemoryBarrier toTransferSrc{
        vk::AccessFlags{},
        vk::AccessFlagBits::eTransferWrite,
        vk::ImageLayout::eUndefined,
        vk::ImageLayout::eGeneral,
        vk::QueueFamilyIgnored,
        vk::QueueFamilyIgnored,
        *image,
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, toTransferSrc);
    commandBuffer.end();
    const auto completion = queueRole == VulkanQueueRole::Compute
        ? device->submitCompute(std::move(commandBuffer), FileDescriptor{})
        : device->submit(std::move(commandBuffer), FileDescriptor{});
    if (!waitForSubmission(completion)) {
        return nullptr;
    }

    std::vector<vk::raii::DeviceMemory> mem;
    mem.push_back(std::move(memory));
    return std::make_unique<VulkanTexture>(device, format, std::move(image), std::move(mem), size, queueRole);
}

std::unique_ptr<VulkanTexture> VulkanTexture::upload(VulkanDevice *device, const QImage &image, vk::ImageUsageFlags usage,
                                                     VulkanQueueRole queueRole)
{
    const auto format = qImageToVulkanFormat(image.format());
    if (!format) {
        return nullptr;
    }
    auto ret = allocate(device, *format, image.size(), usage, queueRole);
    if (!ret) {
        return nullptr;
    }
    ret->update(image);
    return ret;
}

}
