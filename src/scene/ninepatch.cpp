/*
    SPDX-FileCopyrightText: 2026 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "scene/ninepatch.h"

#include <QPainter>

#include <algorithm>

namespace KWin
{

QImage stitchNinePatch(const QImage &topLeftPatch,
                       const QImage &topPatch,
                       const QImage &topRightPatch,
                       const QImage &rightPatch,
                       const QImage &bottomRightPatch,
                       const QImage &bottomPatch,
                       const QImage &bottomLeftPatch,
                       const QImage &leftPatch,
                       QImage::Format format)
{
    const int leftWidth = std::max({topLeftPatch.width(), leftPatch.width(), bottomLeftPatch.width()});
    const int centerWidth = std::max(topPatch.width(), bottomPatch.width());
    const int rightWidth = std::max({topRightPatch.width(), rightPatch.width(), bottomRightPatch.width()});
    const int topHeight = std::max({topLeftPatch.height(), topPatch.height(), topRightPatch.height()});
    const int centerHeight = std::max(leftPatch.height(), rightPatch.height());
    const int bottomHeight = std::max({bottomLeftPatch.height(), bottomPatch.height(), bottomRightPatch.height()});
    const QSize size(leftWidth + centerWidth + rightWidth, topHeight + centerHeight + bottomHeight);
    if (size.isEmpty()) {
        return {};
    }

    QImage image(size, format);
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
    return image;
}

NinePatch::NinePatch()
{
}

NinePatch::~NinePatch()
{
}

} // namespace KWin
