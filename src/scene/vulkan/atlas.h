/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "scene/atlas.h"

#include <memory>
#include <vector>

namespace KWin
{

class ImageTextureVulkan;
class VulkanDevice;

class AtlasVulkan : public Atlas
{
public:
    static std::unique_ptr<AtlasVulkan> create(VulkanDevice *device, const QList<QImage> &images);

    explicit AtlasVulkan(VulkanDevice *device);

    ImageTextureVulkan *texture(uint spriteId) const;

    Sprite sprite(uint spriteId) const override;
    bool update(uint spriteId, const QImage &image, const Rect &damage) override;
    bool reset(const QList<QImage> &images) override;

private:
    VulkanDevice *const m_device;
    std::vector<std::unique_ptr<ImageTextureVulkan>> m_textures;
    QList<Sprite> m_sprites;
};

} // namespace KWin
