/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "ninepatch.h"

#include "scene/vulkan/texture.h"

#include <QPainter>
#include <algorithm>

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
    const int leftWidth = std::max({topLeftPatch.width(), leftPatch.width(), bottomLeftPatch.width()});
    const int centerWidth = std::max(topPatch.width(), bottomPatch.width());
    const int rightWidth = std::max({topRightPatch.width(), rightPatch.width(), bottomRightPatch.width()});
    const int topHeight = std::max({topLeftPatch.height(), topPatch.height(), topRightPatch.height()});
    const int centerHeight = std::max(leftPatch.height(), rightPatch.height());
    const int bottomHeight = std::max({bottomLeftPatch.height(), bottomPatch.height(), bottomRightPatch.height()});
    const QSize size(leftWidth + centerWidth + rightWidth, topHeight + centerHeight + bottomHeight);
    if (size.isEmpty()) {
        return nullptr;
    }

    QImage image(size, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(QPoint(0, 0), topLeftPatch);
    painter.drawImage(QPoint(leftWidth, 0), topPatch);
    painter.drawImage(QPoint(size.width() - topRightPatch.width(), 0), topRightPatch);
    painter.drawImage(QPoint(0, topHeight), leftPatch);
    painter.drawImage(QPoint(size.width() - rightPatch.width(), topHeight), rightPatch);
    painter.drawImage(QPoint(0, size.height() - bottomLeftPatch.height()), bottomLeftPatch);
    painter.drawImage(QPoint(leftWidth, size.height() - bottomPatch.height()), bottomPatch);
    painter.drawImage(QPoint(size.width() - bottomRightPatch.width(), size.height() - bottomRightPatch.height()), bottomRightPatch);
    painter.end();
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
