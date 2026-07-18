/*
    SPDX-FileCopyrightText: 2026 KWin Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "wayland_explicit_sync.h"

#include "core/drmdevice.h"
#include "core/graphicsbuffer.h"
#include "core/syncobjtimeline.h"
#include "wayland_backend.h"
#include "wayland_display.h"
#include "wayland_logging.h"

#include "wayland-linux-drm-syncobj-v1-client-protocol.h"

#include <QSocketNotifier>

namespace KWin::Wayland
{

namespace
{

class ReleaseNotifier : public QObject
{
public:
    ReleaseNotifier(WaylandBackend *backend,
                    GraphicsBuffer *buffer,
                    FileDescriptor &&eventFd,
                    std::shared_ptr<SyncTimeline> timeline)
        : QObject(backend)
        , m_backend(backend)
        , m_buffer(buffer)
        , m_eventFd(std::move(eventFd))
        , m_timeline(std::move(timeline))
        , m_notifier(m_eventFd.get(), QSocketNotifier::Read, this)
    {
        connect(&m_notifier, &QSocketNotifier::activated, this, [this]() {
            m_notifier.setEnabled(false);
            m_backend->releaseBuffer(m_buffer.buffer());
            deleteLater();
        });
    }

private:
    WaylandBackend *const m_backend;
    GraphicsBufferRef m_buffer;
    FileDescriptor m_eventFd;
    // Keep the local syncobj alive until the host signals its release point.
    std::shared_ptr<SyncTimeline> m_timeline;
    QSocketNotifier m_notifier;
};

}

WaylandExplicitSync::WaylandExplicitSync(WaylandBackend *backend, wl_surface *surface)
    : m_backend(backend)
    , m_surface(surface)
{
}

WaylandExplicitSync::~WaylandExplicitSync()
{
    if (m_surfaceSync) {
        wp_linux_drm_syncobj_surface_v1_destroy(m_surfaceSync);
    }
}

bool WaylandExplicitSync::setAcquireReleasePoints(GraphicsBuffer *buffer, FileDescriptor &&acquireFence)
{
    constexpr uint64_t acquirePoint = 1;
    constexpr uint64_t releasePoint = 2;

    auto manager = m_backend->display()->explicitSync();
    DrmDevice *drmDevice = m_backend->drmDevice();
    if (!buffer || !buffer->dmabufAttributes() || !manager || !drmDevice || !drmDevice->supportsSyncObjTimelines()) {
        return false;
    }

    // A separate timeline per commit is intentional. Host compositors may
    // release commits out of order, and signaling a timeline point also
    // signals every preceding point on that timeline.
    auto timeline = std::make_shared<SyncTimeline>(drmDevice->fileDescriptor());
    const FileDescriptor &timelineFd = timeline->fileDescriptor();
    FileDescriptor releaseEvent = timeline->eventFd(releasePoint);
    if (!timelineFd.isValid() || !releaseEvent.isValid()) {
        qCWarning(KWIN_WAYLAND_BACKEND) << "Failed to create a host explicit-sync timeline";
        return false;
    }

    auto protocolTimeline = wp_linux_drm_syncobj_manager_v1_import_timeline(manager, timelineFd.get());
    if (!protocolTimeline) {
        qCWarning(KWIN_WAYLAND_BACKEND) << "Failed to import a host explicit-sync timeline";
        return false;
    }
    if (!m_surfaceSync) {
        m_surfaceSync = wp_linux_drm_syncobj_manager_v1_get_surface(manager, m_surface);
        if (!m_surfaceSync) {
            wp_linux_drm_syncobj_timeline_v1_destroy(protocolTimeline);
            qCWarning(KWIN_WAYLAND_BACKEND) << "Failed to create host explicit synchronization for a surface";
            return false;
        }
    }

    if (acquireFence.isValid()) {
        timeline->moveInto(acquirePoint, acquireFence);
    } else {
        // Direct scanout buffers have already passed their client acquire
        // point before they reach the output backend.
        timeline->signal(acquirePoint);
    }
    wp_linux_drm_syncobj_surface_v1_set_acquire_point(m_surfaceSync, protocolTimeline, 0, acquirePoint);
    wp_linux_drm_syncobj_surface_v1_set_release_point(m_surfaceSync, protocolTimeline, 0, releasePoint);
    wp_linux_drm_syncobj_timeline_v1_destroy(protocolTimeline);

    new ReleaseNotifier(m_backend, buffer, std::move(releaseEvent), std::move(timeline));
    return true;
}

bool WaylandExplicitSync::isActive() const
{
    return m_surfaceSync;
}

} // namespace KWin::Wayland
