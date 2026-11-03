// SPDX-License-Identifier: LGPL-3.0-only
#pragma once
#include <QtFbSupport/private/qfbwindow_p.h>

QT_BEGIN_NAMESPACE
class QAx7zFbWindow final : public QFbWindow
{
public:
    explicit QAx7zFbWindow(QWindow *window) : QFbWindow(window) {}
    void repaint(const QRegion &region) override;
    void setGeometry(const QRect &rect) override;
};
QT_END_NAMESPACE
