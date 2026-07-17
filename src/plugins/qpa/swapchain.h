/*
    SPDX-FileCopyrightText: 2023 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "core/graphicsbuffer.h"
#include "core/graphicsbufferallocator.h"

#include <vector>

namespace KWin
{

namespace QPA
{

class SwapchainSlot;

class Swapchain
{
public:
    Swapchain(GraphicsBufferAllocator *allocator, const GraphicsBufferOptions &options, GraphicsBuffer *initialBuffer);
    ~Swapchain();

    QSize size() const;

    GraphicsBuffer *acquire();
    std::shared_ptr<SyncReleasePoint> releasePoint(GraphicsBuffer *buffer) const;
    uint32_t format() const;
    const ModifierList &modifiers() const;

private:
    GraphicsBufferAllocator *m_allocator;
    GraphicsBufferOptions m_allocationOptions;
    std::vector<std::unique_ptr<SwapchainSlot>> m_slots;
};

}
}
