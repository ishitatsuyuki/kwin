/*
    SPDX-FileCopyrightText: 2023 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "plugins/qpa/swapchain.h"
#include "core/syncobjtimeline.h"

namespace KWin
{
namespace QPA
{

class SwapchainSlot
{
public:
    explicit SwapchainSlot(GraphicsBuffer *buffer)
        : buffer(buffer)
        , releasePoint(std::make_shared<GraphicsBufferReleasePoint>())
    {
    }

    GraphicsBuffer *buffer;
    std::shared_ptr<GraphicsBufferReleasePoint> releasePoint;
};

Swapchain::Swapchain(GraphicsBufferAllocator *allocator, const GraphicsBufferOptions &options, GraphicsBuffer *initialBuffer)
    : m_allocator(allocator)
    , m_allocationOptions(options)
{
    m_slots.push_back(std::make_unique<SwapchainSlot>(initialBuffer));
}

Swapchain::~Swapchain()
{
    for (const auto &slot : std::as_const(m_slots)) {
        slot->releasePoint->setBuffer(slot->buffer);
        slot->buffer->drop();
    }
}

QSize Swapchain::size() const
{
    return m_allocationOptions.size;
}

GraphicsBuffer *Swapchain::acquire()
{
    for (const auto &slot : std::as_const(m_slots)) {
        const FileDescriptor &releaseFence = slot->releasePoint->releaseFd();
        if (!slot->buffer->isReferenced()
            && slot->releasePoint.use_count() == 1
            && (!releaseFence.isValid() || releaseFence.isReadable())) {
            return slot->buffer;
        }
    }

    GraphicsBuffer *buffer = m_allocator->allocate(m_allocationOptions);
    if (!buffer) {
        return nullptr;
    }

    m_slots.push_back(std::make_unique<SwapchainSlot>(buffer));
    return buffer;
}

std::shared_ptr<SyncReleasePoint> Swapchain::releasePoint(GraphicsBuffer *buffer) const
{
    const auto it = std::ranges::find_if(m_slots, [buffer](const auto &slot) {
        return slot->buffer == buffer;
    });
    return it == m_slots.end() ? nullptr : (*it)->releasePoint;
}

uint32_t Swapchain::format() const
{
    return m_allocationOptions.format;
}

const ModifierList &Swapchain::modifiers() const
{
    return m_allocationOptions.modifiers;
}

}
}
