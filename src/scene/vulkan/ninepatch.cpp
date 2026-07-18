/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "ninepatch.h"

#include "scene/vulkan/texture.h"

namespace KWin
{

std::unique_ptr<NinePatchVulkan> NinePatchVulkan::create(VulkanDevice *device, const QImage &image)
{
    auto texture = ImageTextureVulkan::create(device, image);
    return texture ? std::make_unique<NinePatchVulkan>(std::move(texture)) : nullptr;
}

std::unique_ptr<NinePatchVulkan> NinePatchVulkan::create(VulkanDevice *device,
                                                         const QImage &topLeftPatch,
                                                         const QImage &topPatch,
                                                         const QImage &topRightPatch,
                                                         const QImage &rightPatch,
                                                         const QImage &bottomRightPatch,
                                                         const QImage &bottomPatch,
                                                         const QImage &bottomLeftPatch,
                                                         const QImage &leftPatch)
{
    const QImage image = stitchNinePatch(topLeftPatch,
                                         topPatch,
                                         topRightPatch,
                                         rightPatch,
                                         bottomRightPatch,
                                         bottomPatch,
                                         bottomLeftPatch,
                                         leftPatch,
                                         QImage::Format_RGBA8888_Premultiplied);
    if (image.isNull()) {
        return nullptr;
    }
    return create(device, image);
}

NinePatchVulkan::NinePatchVulkan(std::unique_ptr<ImageTextureVulkan> &&texture)
    : m_texture(std::move(texture))
{
}

NinePatchVulkan::~NinePatchVulkan() = default;

ImageTextureVulkan *NinePatchVulkan::texture() const
{
    return m_texture.get();
}

} // namespace KWin
