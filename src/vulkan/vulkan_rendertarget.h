/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/output.h"
#include "kwin_export.h"
#include "utils/filedescriptor.h"

#include <memory>
#include <vector>

namespace KWin
{

class RenderTimeQuery;
class ColorPipeline;
class VulkanTexture;

/**
 * Synchronization and timing state associated with a Vulkan output target.
 * The object is owned by the output layer and referenced by RenderTarget for
 * the duration of a frame.
 */
class KWIN_EXPORT VulkanRenderTarget
{
public:
    explicit VulkanRenderTarget(VulkanTexture *texture,
                                FileDescriptor &&acquireFence = {},
                                OutputTransform transform = OutputTransform::Kind::Normal,
                                const ColorPipeline *outputColorPipeline = nullptr);
    ~VulkanRenderTarget();

    VulkanTexture *texture() const;
    OutputTransform transform() const;
    const ColorPipeline *outputColorPipeline() const;

    FileDescriptor takeAcquireFence();
    void setAcquireFence(FileDescriptor &&fence);

    FileDescriptor takeCompletionFence();
    void setCompletionFence(FileDescriptor &&fence);

    void addRenderTimeQuery(std::unique_ptr<RenderTimeQuery> &&query);
    std::vector<std::unique_ptr<RenderTimeQuery>> takeRenderTimeQueries();

private:
    VulkanTexture *const m_texture;
    const OutputTransform m_transform;
    const std::unique_ptr<ColorPipeline> m_outputColorPipeline;
    FileDescriptor m_acquireFence;
    FileDescriptor m_completionFence;
    std::vector<std::unique_ptr<RenderTimeQuery>> m_renderTimeQueries;
};

} // namespace KWin
