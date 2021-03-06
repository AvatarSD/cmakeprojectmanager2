/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#pragma once

#include "utils/algorithm.h"

#include <QByteArray>
#include <QList>
#include <QSet>

#include <functional>

namespace CMakeProjectManager {

class CMakeConfigItem {
public:
    enum Type { FILEPATH, PATH, BOOL, STRING, INTERNAL, STATIC };
    CMakeConfigItem();
    CMakeConfigItem(const CMakeConfigItem &other);
    CMakeConfigItem(const QByteArray &k, Type t, const QByteArray &d, const QByteArray &v);
    CMakeConfigItem(const QByteArray &k, const QByteArray &v);

    static QByteArray valueOf(const QByteArray &key, const QList<CMakeConfigItem> &input);

    bool isNull() const { return key.isEmpty(); }

    static std::function<bool(const CMakeConfigItem &a, const CMakeConfigItem &b)> sortOperator();
    static CMakeConfigItem fromString(const QString &s);
    QString toString() const;

    QByteArray key;
    Type type = STRING;
    bool isAdvanced = false;
    QByteArray value; // converted to string as needed
    QByteArray documentation;
};
using CMakeConfig = QList<CMakeConfigItem>;

static inline CMakeConfig removeDuplicates(const CMakeConfig &config)
{
    CMakeConfig result;
    // Remove duplicates (last value wins):
    QSet<QByteArray> knownKeys;
    for (int i = config.count() - 1; i >= 0; --i) {
        const CMakeConfigItem &item = config.at(i);
        if (knownKeys.contains(item.key))
            continue;
        result.append(item);
        knownKeys.insert(item.key);
    }
    Utils::sort(result, CMakeConfigItem::sortOperator());
    return result;
}

static inline CMakeConfig removeSubList(const CMakeConfig &config, const CMakeConfig &subConfig)
{
    CMakeConfig result = config;
    Utils::erase(result, [&subConfig](const CMakeConfigItem &item) {
        return std::find_if(subConfig.constBegin(), subConfig.constEnd(), [&item](const CMakeConfigItem &subItem) {
            if (!item.isAdvanced) {
                return item.key == subItem.key;
            }
            return false;
        }) != subConfig.end();
    });
    return result;
}

} // namespace CMakeProjectManager
