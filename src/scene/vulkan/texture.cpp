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
    return image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
}

std::shared_ptr<VulkanTexture> createImageTexture(VulkanDevice *device, const QImage &image)
{
    auto texture = VulkanTexture::upload(device,
                                         uploadImage(image),
                                         vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
                                         VulkanQueueRole::Compute);
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

ImageTextureVulkan::ImageTextureVulkan(VulkanDevice *device)
    : m_device(device)
{
}

std::unique_ptr<ImageTextureVulkan> ImageTextureVulkan::create(VulkanDevice *device, const QImage &image)
{
    auto texture = std::make_unique<ImageTextureVulkan>(device);
    if (!texture->upload(image)) {
        return nullptr;
    }
    return texture;
}

bool ImageTextureVulkan::upload(const QImage &image)
{
    m_texture = createImageTexture(m_device, image);
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
    if (!m_texture || converted.size() != m_size || !m_texture->update(converted, Region(region))) {
        upload(converted);
    }
}

BufferTextureVulkan::BufferTextureVulkan(VulkanDevice *device)
    : m_device(device)
{
}

std::unique_ptr<BufferTextureVulkan> BufferTextureVulkan::create(VulkanDevice *device,
                                                                 GraphicsBuffer *buffer,
                                                                 const std::shared_ptr<SyncReleasePoint> &releasePoint)
{
    auto texture = std::make_unique<BufferTextureVulkan>(device);
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
        m_texture = createImageTexture(m_device, *view.image());
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
    if (!m_texture || converted.size() != m_size || !m_texture->update(converted, region)) {
        attach(buffer, releasePoint);
    }
}

void BufferTextureVulkan::upload(const QImage &image, const Rect &region)
{
    Q_UNREACHABLE();
}

} // namespace KWin
