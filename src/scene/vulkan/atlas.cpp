/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "atlas.h"

#include "scene/vulkan/texture.h"

#include <QImage>

namespace KWin
{

AtlasVulkan::AtlasVulkan(VulkanDevice *device)
    : m_device(device)
{
}

std::unique_ptr<AtlasVulkan> AtlasVulkan::create(VulkanDevice *device, const QList<QImage> &images)
{
    auto atlas = std::make_unique<AtlasVulkan>(device);
    return atlas->reset(images) ? std::move(atlas) : nullptr;
}

ImageTextureVulkan *AtlasVulkan::texture(uint spriteId) const
{
    return spriteId < m_textures.size() ? m_textures[spriteId].get() : nullptr;
}

Atlas::Sprite AtlasVulkan::sprite(uint spriteId) const
{
    return m_sprites.value(spriteId);
}

bool AtlasVulkan::update(uint spriteId, const QImage &image, const Rect &damage)
{
    if (spriteId >= m_textures.size() || image.isNull()) {
        return false;
    }
    if (m_textures[spriteId]) {
        m_textures[spriteId]->upload(image, damage);
    } else {
        m_textures[spriteId] = ImageTextureVulkan::create(m_device, image);
        if (!m_textures[spriteId]) {
            return false;
        }
    }
    m_sprites[spriteId] = Sprite{image.rect(), false};
    return true;
}

bool AtlasVulkan::reset(const QList<QImage> &images)
{
    std::vector<std::unique_ptr<ImageTextureVulkan>> textures;
    QList<Sprite> sprites;
    textures.reserve(images.size());
    sprites.reserve(images.size());
    for (const QImage &image : images) {
        if (image.isNull()) {
            textures.push_back(nullptr);
            sprites.push_back(Sprite{Rect(), false});
            continue;
        }
        auto texture = ImageTextureVulkan::create(m_device, image);
        if (!texture) {
            return false;
        }
        textures.push_back(std::move(texture));
        sprites.push_back(Sprite{image.rect(), false});
    }
    m_textures = std::move(textures);
    m_sprites = std::move(sprites);
    return true;
}

} // namespace KWin
