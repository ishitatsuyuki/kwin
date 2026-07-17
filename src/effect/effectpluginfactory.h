/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2006 Lubos Lunak <l.lunak@kde.org>
    SPDX-FileCopyrightText: 2009 Lucas Murray <lmurray@undefinedfire.com>
    SPDX-FileCopyrightText: 2010, 2011 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2018 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "config-kwin.h"
#include "kwin_export.h"

#include <KPluginFactory>

namespace KWin
{

class Effect;

/*!
 * \class KWin::EffectPluginFactory
 * \inmodule KWin
 * \inheaderfile effect/effectpluginfactory.h
 *
 * Prefer the KWIN_EFFECT_FACTORY macros.
 */
class KWIN_EXPORT EffectPluginFactory : public KPluginFactory
{
    Q_OBJECT

public:
    EffectPluginFactory();
    ~EffectPluginFactory() override;

    virtual bool isSupported() const;
    virtual bool enabledByDefault() const;
    virtual Effect *createEffect() const = 0;
};

#define EffectPluginFactory_iid "org.kde.kwin.EffectPluginFactory" KWIN_PLUGIN_VERSION_STRING
#define KWIN_PLUGIN_FACTORY_NAME KPLUGINFACTORY_PLUGIN_CLASS_INTERNAL_NAME

#define KWIN_EFFECT_FACTORY_SUPPORTED_ENABLED(className, jsonFile, supported, enabled) \
    class KWIN_PLUGIN_FACTORY_NAME : public KWin::EffectPluginFactory                  \
    {                                                                                  \
        Q_OBJECT                                                                       \
        Q_PLUGIN_METADATA(IID EffectPluginFactory_iid FILE jsonFile)                   \
        Q_INTERFACES(KPluginFactory)                                                   \
                                                                                       \
    public:                                                                            \
        explicit KWIN_PLUGIN_FACTORY_NAME()                                            \
        {                                                                              \
        }                                                                              \
        ~KWIN_PLUGIN_FACTORY_NAME()                                                    \
        {                                                                              \
        }                                                                              \
        bool isSupported() const override                                              \
        {                                                                              \
            supported                                                                  \
        }                                                                              \
        bool enabledByDefault() const override{                                        \
            enabled} KWin::Effect *createEffect() const override                       \
        {                                                                              \
            return new className();                                                    \
        }                                                                              \
    };

#define KWIN_EFFECT_FACTORY_ENABLED(className, jsonFile, enabled) \
    KWIN_EFFECT_FACTORY_SUPPORTED_ENABLED(className, jsonFile, return true;, enabled)

#define KWIN_EFFECT_FACTORY_SUPPORTED(className, jsonFile, supported) \
    KWIN_EFFECT_FACTORY_SUPPORTED_ENABLED(className, jsonFile, supported, return true;)

#define KWIN_EFFECT_FACTORY(className, jsonFile) \
    KWIN_EFFECT_FACTORY_SUPPORTED_ENABLED(className, jsonFile, return true;, return true;)

} // namespace KWin
