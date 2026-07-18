/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "utils/filedescriptor.h"

struct wl_surface;
struct wp_linux_drm_syncobj_surface_v1;

namespace KWin
{

class GraphicsBuffer;

namespace Wayland
{

class WaylandBackend;

/** Client-side linux-drm-syncobj synchronization for one host surface. */
class WaylandExplicitSync
{
public:
    WaylandExplicitSync(WaylandBackend *backend, wl_surface *surface);
    ~WaylandExplicitSync();

    /**
     * Sets acquire and release points for the next surface commit. Returns
     * false when explicit synchronization is unavailable, in which case the
     * caller may continue using implicit synchronization only if isActive()
     * is also false.
     */
    bool setAcquireReleasePoints(GraphicsBuffer *buffer, FileDescriptor &&acquireFence);
    bool isActive() const;

private:
    WaylandBackend *const m_backend;
    wl_surface *const m_surface;
    wp_linux_drm_syncobj_surface_v1 *m_surfaceSync = nullptr;
};

} // namespace Wayland
} // namespace KWin
