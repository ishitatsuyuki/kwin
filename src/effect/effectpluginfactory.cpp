/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2006 Lubos Lunak <l.lunak@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "effect/effectpluginfactory.h"

namespace KWin
{

EffectPluginFactory::EffectPluginFactory() = default;
EffectPluginFactory::~EffectPluginFactory() = default;

bool EffectPluginFactory::enabledByDefault() const
{
    return true;
}

bool EffectPluginFactory::isSupported() const
{
    return true;
}

} // namespace KWin

#include "moc_effectpluginfactory.cpp"
