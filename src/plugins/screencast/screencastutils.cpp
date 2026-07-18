/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "screencastutils.h"

#include "compositor.h"
#include "opengl/eglbackend.h"
#include "opengl/egldisplay.h"
#include "vulkan/vulkan_backend.h"
#include "vulkan/vulkan_device.h"

namespace KWin
{

FormatModifierMap screencastFormats()
{
    if (const auto eglBackend = dynamic_cast<EglBackend *>(Compositor::self()->backend())) {
        return eglBackend->openglContext()->displayObject()->nonExternalOnlySupportedDrmFormats();
    }
    if (const auto vulkanBackend = dynamic_cast<VulkanBackend *>(Compositor::self()->backend())) {
        return vulkanBackend->device()->computeOutputFormats();
    }
    return {};
}

} // namespace KWin
