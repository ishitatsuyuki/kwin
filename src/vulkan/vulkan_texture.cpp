/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2023-2026 Xaver Hugl <xaver.hugl@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "vulkan_texture.h"
#include "vulkan_device.h"
#include "vulkan_logging.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstring>
#include <limits>
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
    switch (format) {
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    case QImage::Format_RGB32:
    case QImage::Format_ARGB32_Premultiplied:
        return vk::Format::eB8G8R8A8Unorm;
#endif
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

vk::ComponentMapping VulkanTexture::qImageToComponentMapping(QImage::Format format)
{
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    if (format == QImage::Format_RGB32) {
        return vk::ComponentMapping{
            vk::ComponentSwizzle::eIdentity,
            vk::ComponentSwizzle::eIdentity,
            vk::ComponentSwizzle::eIdentity,
            vk::ComponentSwizzle::eOne,
        };
    }
#endif
    return {};
}

VulkanTexture::VulkanTexture(VulkanDevice *device, vk::Format format, vk::raii::Image &&image,
                             std::vector<vk::raii::DeviceMemory> &&memory, const QSize &size,
                             VulkanQueueRole queueRole, bool external, vk::ComponentMapping componentMapping)
    : m_device(device)
    , m_format(format)
    , m_memory(std::move(memory))
    , m_image(std::move(image))
    , m_imageView(nullptr)
    , m_size(size)
    , m_queueRole(queueRole)
    , m_external(external)
    , m_componentMapping(componentMapping)
{
    m_deviceLostConnection = QObject::connect(device, &VulkanDevice::deviceLost, device, [this]() {
        m_imageView.clear();
        m_image.clear();
        m_memory.clear();
        m_device = nullptr;
    });
}

VulkanTexture::~VulkanTexture()
{
    if (m_pendingUploadManager) {
        m_pendingUploadManager->forget(this);
    }
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

vk::ImageView VulkanTexture::imageView() const
{
    if (!m_device || !*m_image) {
        return {};
    }
    if (!*m_imageView) {
        auto [result, imageView] = m_device->logicalDevice().createImageView(vk::ImageViewCreateInfo{
            vk::ImageViewCreateFlags{},
            m_image,
            vk::ImageViewType::e2D,
            m_format,
            m_componentMapping,
            vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        });
        if (result != vk::Result::eSuccess) {
            qCWarning(KWIN_VULKAN) << "creating a Vulkan texture image view failed:" << vk::to_string(result);
            return {};
        }
        m_imageView = std::move(imageView);
    }
    return *m_imageView;
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

vk::ComponentMapping VulkanTexture::componentMapping() const
{
    return m_componentMapping;
}

static bool componentMappingsEqual(const vk::ComponentMapping &left, const vk::ComponentMapping &right)
{
    return left.r == right.r && left.g == right.g && left.b == right.b && left.a == right.a;
}

bool VulkanTexture::supportsUpload(const QImage &image) const
{
    return qImageToVulkanFormat(image.format()) == m_format
        && componentMappingsEqual(qImageToComponentMapping(image.format()), m_componentMapping);
}

VulkanUploadManager *VulkanTexture::pendingUploadManager() const
{
    return m_pendingUploadManager;
}

static QImage::Format vulkanToQImageFormat(vk::Format format, const vk::ComponentMapping &componentMapping)
{
    switch (format) {
    case vk::Format::eR8Unorm:
        return QImage::Format_Grayscale8;
    case vk::Format::eR16Unorm:
        return QImage::Format_Grayscale16;
    case vk::Format::eR8G8B8A8Unorm:
        return QImage::Format_RGBA8888_Premultiplied;
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    case vk::Format::eB8G8R8A8Unorm:
        return componentMapping.a == vk::ComponentSwizzle::eOne
            ? QImage::Format_RGB32
            : QImage::Format_ARGB32_Premultiplied;
#endif
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
    const QImage::Format qFormat = vulkanToQImageFormat(m_format, m_componentMapping);
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
    // CPU reads from uncached host-visible mappings are extremely slow on
    // discrete GPUs. Prefer cached memory, and invalidate it explicitly so a
    // non-coherent cached memory type is usable too.
    auto stagingMemory = m_device->allocateMemory(bufferInfo,
                                                  vk::MemoryPropertyFlagBits::eHostVisible,
                                                  vk::MemoryPropertyFlagBits::eHostCached);
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
    auto [mapResult, dataPtr] = stagingMemory.mapMemory(0, VK_WHOLE_SIZE);
    if (mapResult != vk::Result::eSuccess) {
        return {};
    }

    const vk::Result invalidateResult = m_device->logicalDevice().invalidateMappedMemoryRanges(vk::MappedMemoryRange{
        *stagingMemory,
        0,
        VK_WHOLE_SIZE,
    });
    if (invalidateResult != vk::Result::eSuccess) {
        stagingMemory.unmapMemory();
        return {};
    }

    std::memcpy(result.bits(), dataPtr, bufferSize);
    stagingMemory.unmapMemory();

    return result;
}

namespace
{

struct PackedUpload
{
    vk::DeviceSize byteSize = 0;
    std::vector<vk::BufferImageCopy2> regions;
};

std::optional<PackedUpload> makePackedUpload(const QImage &image, const Region &damage, const QPoint &offset, vk::DeviceSize baseOffset = 0)
{
    if (image.depth() <= 0 || image.depth() % 8 != 0) {
        return std::nullopt;
    }
    const Region clipped = damage.intersected(Rect(0, 0, image.width(), image.height()));
    const vk::DeviceSize bytesPerPixel = image.depth() / 8;
    PackedUpload upload;
    upload.regions.reserve(clipped.rects().size());
    vk::DeviceSize cursor = baseOffset;
    for (const Rect &rect : clipped.rects()) {
        cursor = (cursor + 3) & ~vk::DeviceSize(3);
        const vk::DeviceSize rowBytes = vk::DeviceSize(rect.width()) * bytesPerPixel;
        if (rect.height() > 0 && rowBytes > (std::numeric_limits<vk::DeviceSize>::max() - cursor) / vk::DeviceSize(rect.height())) {
            return std::nullopt;
        }
        upload.regions.push_back(vk::BufferImageCopy2{
            cursor,
            0,
            0,
            vk::ImageSubresourceLayers{
                vk::ImageAspectFlagBits::eColor,
                0,
                0,
                1,
            },
            vk::Offset3D(rect.x() + offset.x(), rect.y() + offset.y(), 0),
            vk::Extent3D(rect.width(), rect.height(), 1),
        });
        cursor += rowBytes * vk::DeviceSize(rect.height());
    }
    upload.byteSize = cursor - baseOffset;
    return upload;
}

void copyPackedUpload(const QImage &image, const Region &damage, std::span<const vk::BufferImageCopy2> regions, void *mapped)
{
    const Region clipped = damage.intersected(Rect(0, 0, image.width(), image.height()));
    const size_t bytesPerPixel = image.depth() / 8;
    const auto rects = clipped.rects();
    Q_ASSERT(rects.size() == qsizetype(regions.size()));
    for (qsizetype i = 0; i < rects.size(); ++i) {
        const Rect &rect = rects[i];
        const size_t rowBytes = size_t(rect.width()) * bytesPerPixel;
        uint8_t *destination = static_cast<uint8_t *>(mapped) + regions[size_t(i)].bufferOffset;
        for (int y = 0; y < rect.height(); ++y) {
            const uint8_t *source = image.constScanLine(rect.y() + y) + size_t(rect.x()) * bytesPerPixel;
            std::memcpy(destination + size_t(y) * rowBytes, source, rowBytes);
        }
    }
}

vk::ImageMemoryBarrier2 transferWriteBarrier(const VulkanTexture *texture)
{
    return vk::ImageMemoryBarrier2{
        vk::PipelineStageFlagBits2::eAllCommands,
        vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        vk::PipelineStageFlagBits2::eTransfer,
        vk::AccessFlagBits2::eTransferWrite,
        vk::ImageLayout::eGeneral,
        vk::ImageLayout::eGeneral,
        vk::QueueFamilyIgnored,
        vk::QueueFamilyIgnored,
        texture->handle(),
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
}

vk::ImageMemoryBarrier2 transferReadBarrier(const VulkanTexture *texture)
{
    return vk::ImageMemoryBarrier2{
        vk::PipelineStageFlagBits2::eTransfer,
        vk::AccessFlagBits2::eTransferWrite,
        vk::PipelineStageFlagBits2::eAllCommands,
        vk::AccessFlagBits2::eMemoryRead,
        vk::ImageLayout::eGeneral,
        vk::ImageLayout::eGeneral,
        vk::QueueFamilyIgnored,
        vk::QueueFamilyIgnored,
        texture->handle(),
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
}

} // namespace

bool VulkanTexture::update(const QImage &img, const Region &region, const QPoint &offset)
{
    if (!m_device || !*m_image || img.size() != m_size || !supportsUpload(img)) {
        return false;
    }
    if (m_queueRole == VulkanQueueRole::Concurrent || m_external) {
        m_device->waitIdle();
    }

    const auto packed = makePackedUpload(img, region, offset);
    if (!packed) {
        return false;
    }
    if (packed->regions.empty()) {
        return true;
    }
    const vk::BufferCreateInfo bufferInfo{
        vk::BufferCreateFlags(),
        packed->byteSize,
        vk::BufferUsageFlagBits::eTransferSrc,
    };
    auto stagingMemory = m_device->allocateMemory(bufferInfo,
                                                  vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    if (!*stagingMemory) {
        return false;
    }
    auto [result, stagingBuffer] = m_device->logicalDevice().createBuffer(bufferInfo);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    stagingBuffer.bindMemory(stagingMemory, 0);
    auto [mapResult, dataPtr] = stagingMemory.mapMemory(0, packed->byteSize);
    if (mapResult != vk::Result::eSuccess) {
        return false;
    }
    copyPackedUpload(img, region, packed->regions, dataPtr);
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
    const vk::ImageMemoryBarrier2 beforeCopy = transferWriteBarrier(this);
    commandBuffer.pipelineBarrier2(vk::DependencyInfo{{}, {}, {}, beforeCopy});
    commandBuffer.copyBufferToImage2(vk::CopyBufferToImageInfo2{
        *stagingBuffer,
        *m_image,
        vk::ImageLayout::eGeneral,
        packed->regions,
    });
    const vk::ImageMemoryBarrier2 afterCopy = transferReadBarrier(this);
    commandBuffer.pipelineBarrier2(vk::DependencyInfo{{}, {}, {}, afterCopy});
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
                                                       VulkanQueueRole queueRole, vk::ComponentMapping componentMapping)
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
    return std::make_unique<VulkanTexture>(device, format, std::move(image), std::move(mem), size, queueRole, false, componentMapping);
}

std::unique_ptr<VulkanTexture> VulkanTexture::upload(VulkanDevice *device, const QImage &image, vk::ImageUsageFlags usage,
                                                     VulkanQueueRole queueRole)
{
    const auto format = qImageToVulkanFormat(image.format());
    if (!format) {
        return nullptr;
    }
    auto ret = allocate(device, *format, image.size(), usage, queueRole, qImageToComponentMapping(image.format()));
    if (!ret) {
        return nullptr;
    }
    if (!ret->update(image)) {
        return nullptr;
    }
    return ret;
}

struct VulkanUploadManager::Private
{
    static constexpr vk::DeviceSize InitialCapacity = 16 * 1024 * 1024;

    struct Upload
    {
        VulkanTexture *texture = nullptr;
        std::vector<vk::BufferImageCopy2> regions;
    };

    struct Slot
    {
        Slot()
            : buffer(nullptr)
            , memory(nullptr)
        {
        }

        vk::raii::Buffer buffer;
        vk::raii::DeviceMemory memory;
        void *mapped = nullptr;
        vk::DeviceSize capacity = 0;
        vk::DeviceSize used = 0;
        std::vector<Upload> uploads;
        FileDescriptor completionFence;
        bool recorded = false;
    };

    explicit Private(VulkanDevice *device, VulkanUploadManager *owner)
        : device(device)
        , owner(owner)
    {
    }

    void resetSlot(Slot &slot)
    {
        for (const Upload &upload : slot.uploads) {
            if (upload.texture->m_pendingUploadManager == owner) {
                upload.texture->m_pendingUploadManager = nullptr;
            }
        }
        slot.used = 0;
        slot.uploads.clear();
        slot.completionFence = {};
        slot.recorded = false;
    }

    void releaseSlot(Slot &slot)
    {
        if (slot.mapped && *slot.memory) {
            slot.memory.unmapMemory();
        }
        slot.mapped = nullptr;
        slot.buffer.clear();
        slot.memory.clear();
        slot.capacity = 0;
        resetSlot(slot);
    }

    void release()
    {
        current = nullptr;
        for (Slot &slot : slots) {
            releaseSlot(slot);
        }
    }

    Slot *acquireSlot()
    {
        if (current) {
            return current;
        }
        for (Slot &slot : slots) {
            if (!slot.completionFence.isValid() || slot.completionFence.isReadable()) {
                resetSlot(slot);
                current = &slot;
                return current;
            }
        }
        device->waitComputeIdle();
        for (Slot &slot : slots) {
            resetSlot(slot);
        }
        current = &slots.front();
        return current;
    }

    bool ensureCapacity(Slot &slot, vk::DeviceSize required)
    {
        if (required <= slot.capacity) {
            return true;
        }
        const vk::DeviceSize capacity = std::max(InitialCapacity, std::bit_ceil(required));
        const vk::BufferCreateInfo bufferInfo{
            {},
            capacity,
            vk::BufferUsageFlagBits::eTransferSrc,
        };
        auto memory = device->allocateMemory(bufferInfo,
                                             vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        if (!*memory) {
            return false;
        }
        auto [bufferResult, buffer] = device->logicalDevice().createBuffer(bufferInfo);
        if (bufferResult != vk::Result::eSuccess) {
            return false;
        }
        buffer.bindMemory(memory, 0);
        auto [mapResult, mapped] = memory.mapMemory(0, capacity);
        if (mapResult != vk::Result::eSuccess) {
            return false;
        }
        if (slot.used > 0) {
            std::memcpy(mapped, slot.mapped, size_t(slot.used));
        }
        if (slot.mapped && *slot.memory) {
            slot.memory.unmapMemory();
        }
        slot.buffer = std::move(buffer);
        slot.memory = std::move(memory);
        slot.mapped = mapped;
        slot.capacity = capacity;
        return true;
    }

    VulkanDevice *device;
    VulkanUploadManager *owner;
    std::array<Slot, 3> slots;
    Slot *current = nullptr;
    bool deviceLost = false;
    QMetaObject::Connection deviceLostConnection;
};

VulkanUploadManager::VulkanUploadManager(VulkanDevice *device)
    : d(std::make_unique<Private>(device, this))
{
    d->deviceLostConnection = QObject::connect(device, &VulkanDevice::deviceLost, device, [priv = d.get()]() {
        priv->deviceLost = true;
        priv->release();
    });
}

VulkanUploadManager::~VulkanUploadManager()
{
    QObject::disconnect(d->deviceLostConnection);
    if (!d->deviceLost && std::ranges::any_of(d->slots, [](const Private::Slot &slot) {
        return slot.completionFence.isValid() && !slot.completionFence.isReadable();
    })) {
        d->device->waitComputeIdle();
    }
    d->release();
}

bool VulkanUploadManager::upload(VulkanTexture *texture, const QImage &image, const Region &region)
{
    if (d->deviceLost || !texture || texture->queueRole() != VulkanQueueRole::Compute
        || (texture->m_pendingUploadManager && texture->m_pendingUploadManager != this)
        || texture->isExternal() || texture->size() != image.size() || !texture->supportsUpload(image)) {
        return false;
    }
    Private::Slot *slot = d->acquireSlot();
    const auto packed = makePackedUpload(image, region, {}, slot->used);
    if (!packed) {
        return false;
    }
    if (packed->regions.empty()) {
        return true;
    }
    const vk::DeviceSize required = slot->used + packed->byteSize;
    if (!d->ensureCapacity(*slot, required)) {
        return false;
    }
    copyPackedUpload(image, region, packed->regions, slot->mapped);
    slot->used = required;
    slot->uploads.push_back(Private::Upload{
        .texture = texture,
        .regions = packed->regions,
    });
    texture->m_pendingUploadManager = this;
    return true;
}

bool VulkanUploadManager::record(vk::raii::CommandBuffer &commandBuffer)
{
    Private::Slot *slot = d->current;
    if (!slot || slot->uploads.empty() || slot->recorded) {
        return true;
    }
    const vk::BufferMemoryBarrier2 hostBarrier{
        vk::PipelineStageFlagBits2::eHost,
        vk::AccessFlagBits2::eHostWrite,
        vk::PipelineStageFlagBits2::eTransfer,
        vk::AccessFlagBits2::eTransferRead,
        vk::QueueFamilyIgnored,
        vk::QueueFamilyIgnored,
        *slot->buffer,
        0,
        slot->used,
    };
    commandBuffer.pipelineBarrier2(vk::DependencyInfo{{}, {}, hostBarrier, {}});
    for (const Private::Upload &upload : slot->uploads) {
        const vk::ImageMemoryBarrier2 beforeCopy = transferWriteBarrier(upload.texture);
        commandBuffer.pipelineBarrier2(vk::DependencyInfo{{}, {}, {}, beforeCopy});
        commandBuffer.copyBufferToImage2(vk::CopyBufferToImageInfo2{
            *slot->buffer,
            upload.texture->handle(),
            vk::ImageLayout::eGeneral,
            upload.regions,
        });
        const vk::ImageMemoryBarrier2 afterCopy = transferReadBarrier(upload.texture);
        commandBuffer.pipelineBarrier2(vk::DependencyInfo{{}, {}, {}, afterCopy});
    }
    slot->recorded = true;
    return true;
}

void VulkanUploadManager::submitted(const FileDescriptor &completionFence)
{
    if (!d->current || !d->current->recorded) {
        return;
    }
    for (const Private::Upload &upload : d->current->uploads) {
        if (upload.texture->m_pendingUploadManager == this) {
            upload.texture->m_pendingUploadManager = nullptr;
        }
    }
    d->current->completionFence = completionFence.duplicate();
    d->current = nullptr;
}

void VulkanUploadManager::submissionFailed()
{
    if (d->current) {
        d->current->recorded = false;
    }
}

bool VulkanUploadManager::hasPendingUploads() const
{
    return d->current && !d->current->uploads.empty();
}

void VulkanUploadManager::forget(VulkanTexture *texture)
{
    for (Private::Slot &slot : d->slots) {
        std::erase_if(slot.uploads, [texture](const Private::Upload &upload) {
            return upload.texture == texture;
        });
    }
    texture->m_pendingUploadManager = nullptr;
}

}
