// SPDX-License-Identifier: LGPL-3.0-only
#include "qax7zfbwindow.h"
#include "qax7zfbscreen.h"

QT_BEGIN_NAMESPACE
void QAx7zFbWindow::repaint(const QRegion &region)
{
    // The explicit private integration creates this screen/window pair only.
    auto *screen = static_cast<QAx7zFbScreen *>(platformScreen());
    if (screen->directSingleEnabled()) screen->recordSourceFlush(this, region);
    // Preserve the baseline damage/compositor behavior, including default-off.
    QFbWindow::repaint(region);
}
void QAx7zFbWindow::setGeometry(const QRect &rect)
{
    if (rect != geometry()) {
        auto *screen = static_cast<QAx7zFbScreen *>(platformScreen());
        if (screen->directSingleEnabled()) screen->invalidateSource(this);
    }
    QFbWindow::setGeometry(rect);
}
QT_END_NAMESPACE
