/*
    SPDX-FileCopyrightText: 2026 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QImage>

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
                       QImage::Format format);

class NinePatch
{
public:
    virtual ~NinePatch();

protected:
    NinePatch();
};

} // namespace KWin
