/*
    SPDX-FileCopyrightText: 2023 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "kwin_export.h"
#include "utils/filedescriptor.h"

#include <QObject>
#include <QSize>
#include <sys/types.h>
#include <utility>

namespace KWin
{

class SyncReleasePoint;

struct DmaBufAttributes
{
    int planeCount = 0;
    int width = 0;
    int height = 0;
    uint32_t format = 0;
    uint64_t modifier = 0;
    dev_t device;

    std::array<FileDescriptor, 4> fd;
    std::array<uint32_t, 4> offset{0, 0, 0, 0};
    std::array<uint32_t, 4> pitch{0, 0, 0, 0};
};

struct ShmAttributes
{
    FileDescriptor fd;
    int stride;
    off_t offset;
    QSize size;
    uint32_t format;
};

struct SinglePixelAttributes
{
    uint32_t red;
    uint32_t green;
    uint32_t blue;
    uint32_t alpha;
};

/**
 * The GraphicsBuffer class represents a chunk of memory containing graphics data.
 *
 * A graphics buffer can be referenced. In which case, it won't be destroyed until all
 * references are dropped. You can use the isDropped() function to check whether the
 * buffer has been marked as destroyed.
 */
class KWIN_EXPORT GraphicsBuffer : public QObject, public std::enable_shared_from_this<GraphicsBuffer>
{
    Q_OBJECT

public:
    class Lock
    {
    public:
        ~Lock();

        const std::shared_ptr<GraphicsBuffer> &buffer() const;

    private:
        friend class GraphicsBuffer;
        explicit Lock(const std::shared_ptr<GraphicsBuffer> &ref);

        std::shared_ptr<GraphicsBuffer> m_buffer;
    };

    explicit GraphicsBuffer();
    ~GraphicsBuffer() override;

    bool isReferenced() const;
    std::shared_ptr<Lock> reference();

    enum MapFlag {
        Read = 0x1,
        Write = 0x2,
    };
    Q_DECLARE_FLAGS(MapFlags, MapFlag)

    struct Map
    {
        void *data = nullptr;
        uint32_t stride = 0;
    };
    virtual Map map(MapFlags flags);
    virtual void unmap();

    virtual QSize size() const = 0;
    virtual bool hasAlphaChannel() const = 0;

    virtual const DmaBufAttributes *udmabufAttributes() const;
    virtual const DmaBufAttributes *dmabufAttributes() const;
    virtual const ShmAttributes *shmAttributes() const;
    virtual const SinglePixelAttributes *singlePixelAttributes() const;

    /**
     * the added release point will be referenced as long as this buffer is referenced
     */
    void addReleasePoint(const std::shared_ptr<SyncReleasePoint> &releasePoint);

    static bool alphaChannelFromDrmFormat(uint32_t format);

protected:
    friend class Lock;

    /**
     * called the first time the buffer is referenced after it's created or released
     */
    virtual void referenced();
    virtual void released();

    std::weak_ptr<Lock> m_reference;
    std::vector<std::shared_ptr<SyncReleasePoint>> m_releasePoints;
};

/**
 * The GraphicsBufferRef type holds a reference to a GraphicsBuffer. While the reference
 * exists, the graphics buffer cannot be destroyed and the client cannot modify it.
 */
class GraphicsBufferRef
{
public:
    GraphicsBufferRef() = default;
    GraphicsBufferRef(const GraphicsBufferRef &copy) = default;
    GraphicsBufferRef(GraphicsBufferRef &&move) = default;
    GraphicsBufferRef(GraphicsBuffer *buffer)
        : m_lock(buffer ? buffer->reference() : nullptr)
    {
    }

    GraphicsBufferRef(const std::shared_ptr<GraphicsBuffer> &buffer)
        : m_lock(buffer ? buffer->reference() : nullptr)
    {
    }

    GraphicsBufferRef &operator=(const GraphicsBufferRef &other) = default;
    GraphicsBufferRef &operator=(GraphicsBufferRef &&other) = default;

    GraphicsBufferRef &operator=(GraphicsBuffer *buffer)
    {
        m_lock = buffer ? buffer->reference() : nullptr;
        return *this;
    }

    void reset()
    {
        m_lock.reset();
    }

    inline GraphicsBuffer *buffer() const
    {
        return m_lock ? m_lock->buffer().get() : nullptr;
    }

    inline GraphicsBuffer *operator*() const
    {
        return buffer();
    }

    inline GraphicsBuffer *operator->() const
    {
        return buffer();
    }

    inline operator bool() const
    {
        return m_lock != nullptr;
    }

private:
    std::shared_ptr<GraphicsBuffer::Lock> m_lock;
};

} // namespace KWin

Q_DECLARE_OPERATORS_FOR_FLAGS(KWin::GraphicsBuffer::MapFlags)
