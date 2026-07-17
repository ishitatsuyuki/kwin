/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "vulkan_rendertarget.h"

#include "core/colorpipeline.h"
#include "core/renderbackend.h"

namespace KWin
{

VulkanRenderTarget::VulkanRenderTarget(VulkanTexture *texture, FileDescriptor &&acquireFence, OutputTransform transform, const ColorPipeline *outputColorPipeline)
    : m_texture(texture)
    , m_transform(transform)
    , m_outputColorPipeline(outputColorPipeline ? std::make_unique<ColorPipeline>(*outputColorPipeline) : nullptr)
    , m_acquireFence(std::move(acquireFence))
{
}

VulkanRenderTarget::~VulkanRenderTarget() = default;

VulkanTexture *VulkanRenderTarget::texture() const
{
    return m_texture;
}

OutputTransform VulkanRenderTarget::transform() const
{
    return m_transform;
}

const ColorPipeline *VulkanRenderTarget::outputColorPipeline() const
{
    return m_outputColorPipeline.get();
}

FileDescriptor VulkanRenderTarget::takeAcquireFence()
{
    return std::move(m_acquireFence);
}

void VulkanRenderTarget::setAcquireFence(FileDescriptor &&fence)
{
    m_acquireFence = std::move(fence);
}

FileDescriptor VulkanRenderTarget::takeCompletionFence()
{
    return std::move(m_completionFence);
}

void VulkanRenderTarget::setCompletionFence(FileDescriptor &&fence)
{
    m_completionFence = std::move(fence);
}

void VulkanRenderTarget::addRenderTimeQuery(std::unique_ptr<RenderTimeQuery> &&query)
{
    if (query) {
        m_renderTimeQueries.push_back(std::move(query));
    }
}

std::vector<std::unique_ptr<RenderTimeQuery>> VulkanRenderTarget::takeRenderTimeQueries()
{
    return std::move(m_renderTimeQueries);
}

} // namespace KWin
