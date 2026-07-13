/*
    SPDX-FileCopyrightText: 2026 Xaver Hugl <xaver.hugl@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once
#include "scene/item.h"

namespace KWin
{

class KWIN_EXPORT TransformItem : public Item
{
    Q_OBJECT

public:
    explicit TransformItem(Item *toTransform);
    ~TransformItem() override;

private:
    void updateZ();
};

}
