/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Xaver Hugl <xaver.hugl@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "scene/backgroundeffectitem.h"
#include "scene/windowitem.h"
#include "window.h"

namespace KWin
{

// TODO make this generic for all items
BackgroundEffectItem::BackgroundEffectItem(WindowItem *parentItem)
    : Item(parentItem)
    , m_windowItem(parentItem)
{
    setZ(-1'000'000);
    addEffect();
    connect(parentItem->windowContainer(), &Item::boundingRectChanged, this, &BackgroundEffectItem::updateGeometry);
    connect(parentItem->window(), &Window::damaged, this, [this]() {
        // BackgroundEffectItem is below the surface in the item tree, so
        // repaint accumulation visits it before it sees damage from that
        // surface. Schedule the effect explicitly when it needs a sampling
        // margin; this makes the expanded repaint available at the point
        // where the background item is traversed.
        if (m_pixelsToExpandRepaints > 0) {
            scheduleRepaint(rect());
        }
    });
}

uint32_t BackgroundEffectItem::pixelsToExpandRepaintsBelowOpaqueRegions() const
{
    return m_pixelsToExpandRepaintsBelowOpaqueRegions;
}

void BackgroundEffectItem::setPixelsToExpandRepaintsBelowOpaqueRegions(uint32_t pixels)
{
    m_pixelsToExpandRepaintsBelowOpaqueRegions = pixels;
}

uint32_t BackgroundEffectItem::pixelsToExpandRepaints() const
{
    return m_pixelsToExpandRepaints;
}

void BackgroundEffectItem::setPixelsToExpandRepaints(uint32_t pixels)
{
    m_pixelsToExpandRepaints = pixels;
}

void BackgroundEffectItem::setEffectBoundingRect(const RectF &boundingRect)
{
    m_effectBounds = boundingRect;
    updateGeometry();
}

void BackgroundEffectItem::updateGeometry()
{
    setGeometry(m_effectBounds & m_windowItem->windowContainer()->rect());
}

}

#include "moc_backgroundeffectitem.cpp"
