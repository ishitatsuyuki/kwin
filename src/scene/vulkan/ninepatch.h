/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "kwin_export.h"
#include "scene/ninepatch.h"

#include <memory>

class QImage;

namespace KWin
{

class ImageTextureVulkan;
class VulkanDevice;

class KWIN_EXPORT NinePatchVulkan : public NinePatch
{
public:
    static std::unique_ptr<NinePatchVulkan> create(VulkanDevice *device, const QImage &image);
    static std::unique_ptr<NinePatchVulkan> create(VulkanDevice *device,
                                                   const QImage &topLeftPatch,
                                                   const QImage &topPatch,
                                                   const QImage &topRightPatch,
                                                   const QImage &rightPatch,
                                                   const QImage &bottomRightPatch,
                                                   const QImage &bottomPatch,
                                                   const QImage &bottomLeftPatch,
                                                   const QImage &leftPatch);

    explicit NinePatchVulkan(std::unique_ptr<ImageTextureVulkan> &&texture);
    ~NinePatchVulkan() override;

    ImageTextureVulkan *texture() const;

private:
    std::unique_ptr<ImageTextureVulkan> m_texture;
};

} // namespace KWin
