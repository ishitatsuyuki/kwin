/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "vulkanscreencapture.h"

#include "scene/itemrenderer_vulkan.h"
#include "vulkan/vulkan_compositor.h"
#include "vulkan/vulkan_texture.h"

#include <array>
#include <cerrno>
#include <poll.h>

namespace KWin
{

class VulkanScreenCapturePrivate
{
public:
    struct Frame
    {
        std::unique_ptr<VulkanTexture> texture;
        std::optional<VulkanCompositorRenderResult> result;
    };

    VulkanDevice *device = nullptr;
    std::unique_ptr<VulkanCompositor> compositor;
    std::array<Frame, 3> frames;
    uint32_t nextFrame = 0;
};

VulkanScreenCapture::VulkanScreenCapture(VulkanDevice *device)
    : d(std::make_unique<VulkanScreenCapturePrivate>())
{
    d->device = device;
    d->compositor = VulkanCompositor::create(device);
}

VulkanScreenCapture::~VulkanScreenCapture() = default;

VulkanTexture *VulkanScreenCapture::capture(ItemRendererVulkan *renderer,
                                            const QSize &size,
                                            const std::shared_ptr<ColorDescription> &colorDescription)
{
    if (!renderer || !d->compositor || size.isEmpty()) {
        return nullptr;
    }

    VulkanScreenCapturePrivate::Frame *frame = nullptr;
    for (uint32_t offset = 0; offset < d->frames.size(); ++offset) {
        const uint32_t index = (d->nextFrame + offset) % d->frames.size();
        auto &candidate = d->frames[index];
        if (!candidate.result || candidate.result->completionFence.isReadable()) {
            frame = &candidate;
            d->nextFrame = (index + 1) % d->frames.size();
            break;
        }
    }
    if (!frame) {
        frame = &d->frames[d->nextFrame];
        pollfd descriptor{frame->result->completionFence.get(), POLLIN, 0};
        while (poll(&descriptor, 1, -1) < 0 && errno == EINTR) {
        }
        d->nextFrame = (d->nextFrame + 1) % d->frames.size();
    }
    frame->result.reset();

    if (!frame->texture || frame->texture->size() != size) {
        const auto usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
        frame->texture = VulkanTexture::allocate(d->device,
                                                 vk::Format::eR16G16B16A16Sfloat,
                                                 size,
                                                 usage,
                                                 VulkanQueueRole::Compute);
        if (!frame->texture) {
            frame->texture = VulkanTexture::allocate(d->device,
                                                     vk::Format::eR8G8B8A8Unorm,
                                                     size,
                                                     usage,
                                                     VulkanQueueRole::Compute);
        }
    }
    if (!frame->texture) {
        return nullptr;
    }

    frame->result = renderer->renderCurrentLayersTo(d->compositor.get(), frame->texture.get(), colorDescription);
    return frame->result ? frame->texture.get() : nullptr;
}

} // namespace KWin
