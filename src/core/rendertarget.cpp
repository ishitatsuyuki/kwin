/*
    SPDX-FileCopyrightText: 2022 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "core/rendertarget.h"
#include "opengl/glutils.h"
#include "vulkan/vulkan_rendertarget.h"
#include "vulkan/vulkan_texture.h"

namespace KWin
{

RenderTarget::RenderTarget(GLFramebuffer *fbo, const std::shared_ptr<ColorDescription> &colorDescription)
    : m_framebuffer(fbo)
    , m_transform(fbo->colorAttachment() ? fbo->colorAttachment()->contentTransform() : OutputTransform())
    , m_colorDescription(colorDescription)
{
}

RenderTarget::RenderTarget(QImage *image, const std::shared_ptr<ColorDescription> &colorDescription)
    : m_image(image)
    , m_colorDescription(colorDescription)
{
}

RenderTarget::RenderTarget(VulkanRenderTarget *target, const std::shared_ptr<ColorDescription> &colorDescription)
    : m_vulkanTarget(target)
    , m_transform(target->transform())
    , m_colorDescription(colorDescription)
{
}

QSize RenderTarget::transformedSize() const
{
    return m_transform.map(size());
}

Rect RenderTarget::transformedRect() const
{
    return Rect(QPoint(0, 0), transformedSize());
}

QSize RenderTarget::size() const
{
    if (m_framebuffer) {
        return m_framebuffer->size();
    } else if (m_image) {
        return m_image->size();
    } else if (m_vulkanTarget) {
        return m_vulkanTarget->texture()->size();
    } else {
        Q_UNREACHABLE();
    }
}

OutputTransform RenderTarget::transform() const
{
    return m_transform;
}

GLFramebuffer *RenderTarget::framebuffer() const
{
    return m_framebuffer;
}

GLTexture *RenderTarget::texture() const
{
    return m_framebuffer->colorAttachment();
}

VulkanRenderTarget *RenderTarget::vulkanTarget() const
{
    return m_vulkanTarget;
}

VulkanTexture *RenderTarget::vulkanTexture() const
{
    return m_vulkanTarget ? m_vulkanTarget->texture() : nullptr;
}

QImage *RenderTarget::image() const
{
    return m_image;
}

const std::shared_ptr<ColorDescription> &RenderTarget::colorDescription() const
{
    return m_colorDescription;
}

} // namespace KWin
