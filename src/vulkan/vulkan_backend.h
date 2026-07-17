/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/renderbackend.h"

#include <memory>

namespace KWin
{

class RenderDevice;
class VulkanDevice;
class EglContext;

class KWIN_EXPORT VulkanBackend : public RenderBackend
{
    Q_OBJECT

public:
    explicit VulkanBackend(RenderDevice *renderDevice);
    ~VulkanBackend() override;

    CompositingType compositingType() const override final;
    bool checkGraphicsReset() override final;
    DrmDevice *drmDevice() const override final;
    bool testImportBuffer(GraphicsBuffer *buffer) override final;
    FormatModifierMap supportedFormats() const override final;

    void initWayland();

    RenderDevice *renderDevice() const;
    VulkanDevice *device() const;
    /** Compatibility context for legacy effect shaders; not used for scene composition. */
    EglContext *openglContext() const;

private:
    RenderDevice *const m_renderDevice;
    VulkanDevice *const m_device;
    mutable std::shared_ptr<EglContext> m_openglContext;
};

} // namespace KWin
