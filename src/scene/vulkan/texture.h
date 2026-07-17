/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "kwin_export.h"
#include "scene/texture.h"

#include <span>
#include <vector>

namespace KWin
{

class GraphicsBuffer;
class VulkanDevice;
class VulkanTexture;
class VulkanUploadManager;

class KWIN_EXPORT TextureVulkan : public Texture
{
public:
    VulkanTexture *nativeTexture() const;
    std::span<const std::shared_ptr<VulkanTexture>> nativeTextures() const;

protected:
    std::shared_ptr<VulkanTexture> m_texture;
    std::vector<std::shared_ptr<VulkanTexture>> m_planes;
};

class ImageTextureVulkan : public TextureVulkan
{
public:
    static std::unique_ptr<ImageTextureVulkan> create(VulkanDevice *device, const QImage &image, VulkanUploadManager *uploadManager = nullptr);

    explicit ImageTextureVulkan(VulkanDevice *device, VulkanUploadManager *uploadManager = nullptr);

    void attach(GraphicsBuffer *buffer, const Region &region, const std::shared_ptr<SyncReleasePoint> &releasePoint) override;
    void upload(const QImage &image, const Rect &region) override;

private:
    bool upload(const QImage &image);

    VulkanDevice *const m_device;
    VulkanUploadManager *const m_uploadManager;
};

class BufferTextureVulkan : public TextureVulkan
{
public:
    static std::unique_ptr<BufferTextureVulkan> create(VulkanDevice *device,
                                                       GraphicsBuffer *buffer,
                                                       const std::shared_ptr<SyncReleasePoint> &releasePoint,
                                                       VulkanUploadManager *uploadManager = nullptr);

    explicit BufferTextureVulkan(VulkanDevice *device, VulkanUploadManager *uploadManager = nullptr);

    void attach(GraphicsBuffer *buffer, const Region &region, const std::shared_ptr<SyncReleasePoint> &releasePoint) override;
    void upload(const QImage &image, const Rect &region) override;

private:
    bool attach(GraphicsBuffer *buffer, const std::shared_ptr<SyncReleasePoint> &releasePoint);

    VulkanDevice *const m_device;
    VulkanUploadManager *const m_uploadManager;
};

} // namespace KWin
