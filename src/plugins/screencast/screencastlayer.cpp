/*
    SPDX-FileCopyrightText: 2025 Xaver Hugl <xaver.hugl@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "screencastlayer.h"

namespace KWin
{

ScreencastLayer::ScreencastLayer(LogicalOutput *output, const FormatModifierMap &formats)
    : OutputLayer(output->backendOutput(), OutputLayerType::Primary)
    , m_formats(formats)
{
    // prevent the layer from scheduling frames on the actual output
    setRenderLoop(nullptr);
}

void ScreencastLayer::setFramebuffer(GLFramebuffer *buffer, const Region &bufferDamage)
{
    setRenderTarget(RenderTarget(buffer), bufferDamage);
}

void ScreencastLayer::setRenderTarget(const RenderTarget &target, const Region &bufferDamage)
{
    // TODO is there a better way to deal with this?
    m_renderTarget.emplace(target);
    m_bufferDamage = bufferDamage;
}

DrmDevice *ScreencastLayer::scanoutDevice() const
{
    return nullptr;
}

FormatModifierMap ScreencastLayer::supportedDrmFormats() const
{
    return m_formats;
}

std::optional<OutputLayerBeginFrameInfo> ScreencastLayer::doBeginFrame()
{
    if (!m_renderTarget) {
        return std::nullopt;
    }
    return OutputLayerBeginFrameInfo{
        .renderTarget = *m_renderTarget,
        .repaint = m_bufferDamage,
    };
}

bool ScreencastLayer::doEndFrame(const Region &renderedRegion, const Region &damagedRegion, OutputFrame *frame)
{
    return true;
}

void ScreencastLayer::releaseBuffers()
{
}

}
