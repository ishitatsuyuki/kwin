/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "texture.h"

#include "core/drm_formats.h"
#include "core/graphicsbuffer.h"
#include "core/graphicsbufferview.h"
#include "core/region.h"
#include "vulkan/vulkan_device.h"
#include "vulkan/vulkan_texture.h"

namespace KWin
{

namespace
{

QImage uploadImage(const QImage &image)
{
    if (VulkanTexture::qImageToVulkanFormat(image.format())) {
        return image;
    }
    return image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
}

std::shared_ptr<VulkanTexture> createImageTexture(VulkanDevice *device, const QImage &image, VulkanUploadManager *uploadManager)
{
    const QImage source = uploadImage(image);
    const auto usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled;
    std::unique_ptr<VulkanTexture> texture;
    if (uploadManager) {
        const auto format = VulkanTexture::qImageToVulkanFormat(source.format());
        if (format) {
            texture = VulkanTexture::allocate(device,
                                              *format,
                                              source.size(),
                                              usage,
                                              VulkanQueueRole::Compute,
                                              VulkanTexture::qImageToComponentMapping(source.format()));
            if (texture && !uploadManager->upload(texture.get(), source, Region(0, 0, source.width(), source.height()))
                && !texture->update(source)) {
                texture.reset();
            }
        }
    } else {
        texture = VulkanTexture::upload(device, source, usage, VulkanQueueRole::Compute);
    }
    return texture ? std::shared_ptr<VulkanTexture>(std::move(texture)) : nullptr;
}

} // namespace

VulkanTexture *TextureVulkan::nativeTexture() const
{
    return m_texture.get();
}

std::span<const std::shared_ptr<VulkanTexture>> TextureVulkan::nativeTextures() const
{
    return m_planes;
}

ImageTextureVulkan::ImageTextureVulkan(VulkanDevice *device, VulkanUploadManager *uploadManager)
    : m_device(device)
    , m_uploadManager(uploadManager)
{
}

std::unique_ptr<ImageTextureVulkan> ImageTextureVulkan::create(VulkanDevice *device, const QImage &image, VulkanUploadManager *uploadManager)
{
    auto texture = std::make_unique<ImageTextureVulkan>(device, uploadManager);
    if (!texture->upload(image)) {
        return nullptr;
    }
    return texture;
}

bool ImageTextureVulkan::upload(const QImage &image)
{
    m_texture = createImageTexture(m_device, image, m_uploadManager);
    if (!m_texture) {
        return false;
    }
    m_planes = {m_texture};
    m_size = image.size();
    return true;
}

void ImageTextureVulkan::attach(GraphicsBuffer *buffer, const Region &region, const std::shared_ptr<SyncReleasePoint> &releasePoint)
{
    Q_UNREACHABLE();
}

void ImageTextureVulkan::upload(const QImage &image, const Rect &region)
{
    const QImage converted = uploadImage(image);
    const bool updated = m_texture && converted.size() == m_size
        && ((m_uploadManager && m_uploadManager->upload(m_texture.get(), converted, Region(region)))
            || (!m_uploadManager && m_texture->update(converted, Region(region))));
    if (!updated) {
        upload(converted);
    }
}

BufferTextureVulkan::BufferTextureVulkan(VulkanDevice *device, VulkanUploadManager *uploadManager)
    : m_device(device)
    , m_uploadManager(uploadManager)
{
}

std::unique_ptr<BufferTextureVulkan> BufferTextureVulkan::create(VulkanDevice *device,
                                                                 GraphicsBuffer *buffer,
                                                                 const std::shared_ptr<SyncReleasePoint> &releasePoint,
                                                                 VulkanUploadManager *uploadManager)
{
    auto texture = std::make_unique<BufferTextureVulkan>(device, uploadManager);
    if (!texture->attach(buffer, releasePoint)) {
        return nullptr;
    }
    return texture;
}

bool BufferTextureVulkan::attach(GraphicsBuffer *buffer, const std::shared_ptr<SyncReleasePoint> &releasePoint)
{
    if (buffer->dmabufAttributes()) {
        const DmaBufAttributes *attributes = buffer->dmabufAttributes();
        const auto format = FormatInfo::get(attributes->format);
        const auto conversion = format ? format->yuvConversion() : std::nullopt;
        m_planes.clear();
        if (conversion) {
            if (conversion->plane.size() != attributes->planeCount) {
                return false;
            }
            for (uint32_t i = 0; i < uint32_t(conversion->plane.size()); ++i) {
                const YuvFormat &plane = conversion->plane[i];
                const QSize size(buffer->size().width() / int(plane.widthDivisor),
                                 buffer->size().height() / int(plane.heightDivisor));
                auto texture = m_device->importBufferPlane(buffer, i, plane.format, size, VK_IMAGE_USAGE_SAMPLED_BIT);
                if (!texture) {
                    m_planes.clear();
                    return false;
                }
                m_planes.push_back(std::move(texture));
            }
            m_texture = m_planes.front();
        } else {
            m_texture = m_device->importBuffer(buffer, VK_IMAGE_USAGE_SAMPLED_BIT);
            if (m_texture) {
                m_planes = {m_texture};
            }
        }
    } else {
        const GraphicsBufferView view(buffer);
        if (view.isNull()) {
            return false;
        }
        m_texture = createImageTexture(m_device, *view.image(), m_uploadManager);
        if (m_texture) {
            m_planes = {m_texture};
        }
    }
    if (!m_texture) {
        return false;
    }
    m_size = buffer->size();
    std::optional<FormatInfo> format;
    if (buffer->dmabufAttributes()) {
        format = FormatInfo::get(buffer->dmabufAttributes()->format);
    } else if (buffer->shmAttributes()) {
        format = FormatInfo::get(buffer->shmAttributes()->format);
    }
    m_isFloatingPoint = format && format->floatingPoint;
    m_releasePoint = releasePoint;
    return true;
}

void BufferTextureVulkan::attach(GraphicsBuffer *buffer, const Region &region, const std::shared_ptr<SyncReleasePoint> &releasePoint)
{
    if (buffer->dmabufAttributes()) {
        attach(buffer, releasePoint);
        return;
    }
    const GraphicsBufferView view(buffer);
    if (view.isNull()) {
        return;
    }
    const QImage converted = uploadImage(*view.image());
    const bool updated = m_texture && converted.size() == m_size
        && ((m_uploadManager && m_uploadManager->upload(m_texture.get(), converted, region))
            || (!m_uploadManager && m_texture->update(converted, region)));
    if (!updated) {
        attach(buffer, releasePoint);
    }
}

void BufferTextureVulkan::upload(const QImage &image, const Rect &region)
{
    Q_UNREACHABLE();
}

} // namespace KWin
