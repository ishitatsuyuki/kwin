/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "kwin_export.h"

#include <QSize>
#include <memory>

namespace KWin
{

class ColorDescription;
class ItemRendererVulkan;
class VulkanDevice;
class VulkanScreenCapturePrivate;
class VulkanTexture;

/** Reusable, frame-overlapped scratch capture for Vulkan screen effects. */
class KWIN_EXPORT VulkanScreenCapture
{
public:
    explicit VulkanScreenCapture(VulkanDevice *device);
    ~VulkanScreenCapture();

    VulkanTexture *capture(ItemRendererVulkan *renderer,
                           const QSize &size,
                           const std::shared_ptr<ColorDescription> &colorDescription);

private:
    std::unique_ptr<VulkanScreenCapturePrivate> d;
};

} // namespace KWin
