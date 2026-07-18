/*
    SPDX-FileCopyrightText: 2026 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "scene/opengl/ninepatch.h"
#include "main.h"
#include "opengl/eglcontext.h"
#include "opengl/gltexture.h"
#include "scene/workspacescene.h"

namespace KWin
{

std::unique_ptr<NinePatchOpenGL> NinePatchOpenGL::create(const QImage &image)
{
    auto texture = GLTexture::upload(image);
    if (!texture) {
        return nullptr;
    }

    texture->setFilter(GL_LINEAR);
    texture->setWrapMode(GL_CLAMP_TO_EDGE);

    return std::make_unique<NinePatchOpenGL>(std::move(texture));
}

std::unique_ptr<NinePatchOpenGL> NinePatchOpenGL::create(const QImage &topLeftPatch,
                                                         const QImage &topPatch,
                                                         const QImage &topRightPatch,
                                                         const QImage &rightPatch,
                                                         const QImage &bottomRightPatch,
                                                         const QImage &bottomPatch,
                                                         const QImage &bottomLeftPatch,
                                                         const QImage &leftPatch)
{
    QImage image = stitchNinePatch(topLeftPatch,
                                   topPatch,
                                   topRightPatch,
                                   rightPatch,
                                   bottomRightPatch,
                                   bottomPatch,
                                   bottomLeftPatch,
                                   leftPatch,
                                   QImage::Format_ARGB32);
    if (image.isNull()) {
        return nullptr;
    }

    // Check if the image is alpha-only in practice, and if so convert it to an 8-bpp format
    const auto context = EglContext::currentContext();
    if (!context->isOpenGLES() && context->supportsTextureSwizzle() && context->supportsRGTextures()) {
        QImage alphaImage(image.size(), QImage::Format_Alpha8);
        bool alphaOnly = true;

        for (ptrdiff_t y = 0; alphaOnly && y < image.height(); y++) {
            const uint32_t *const src = reinterpret_cast<const uint32_t *>(image.scanLine(y));
            uint8_t *const dst = reinterpret_cast<uint8_t *>(alphaImage.scanLine(y));

            for (ptrdiff_t x = 0; x < image.width(); x++) {
                if (src[x] & 0x00ffffff) {
                    alphaOnly = false;
                }

                dst[x] = qAlpha(src[x]);
            }
        }

        if (alphaOnly) {
            image = alphaImage;
        }
    }

    auto texture = GLTexture::upload(image);
    if (!texture) {
        return nullptr;
    }

    texture->setFilter(GL_LINEAR);
    texture->setWrapMode(GL_CLAMP_TO_EDGE);

    if (texture->internalFormat() == GL_R8) {
        // Swizzle red to alpha and all other channels to zero
        texture->bind();
        texture->setSwizzle(GL_ZERO, GL_ZERO, GL_ZERO, GL_RED);
    }

    return std::make_unique<NinePatchOpenGL>(std::move(texture));
}

NinePatchOpenGL::NinePatchOpenGL(std::unique_ptr<GLTexture> &&texture)
    : m_texture(std::move(texture))
{
}

NinePatchOpenGL::~NinePatchOpenGL()
{
    // FIXME: It should not be attached to the workspace scene.
    kwinApp()->scene()->openglContext()->makeCurrent();
}

GLTexture *NinePatchOpenGL::texture() const
{
    return m_texture.get();
}

} // namespace KWin
