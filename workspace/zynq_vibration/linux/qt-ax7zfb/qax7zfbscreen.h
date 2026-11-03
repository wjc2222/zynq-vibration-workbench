// SPDX-License-Identifier: LGPL-3.0-only
#pragma once
#include <QtFbSupport/private/qfbscreen_p.h>
#include "ax7zflipbuffer.h"

QT_BEGIN_NAMESPACE
class QAx7zFbScreen : public QFbScreen
{
    Q_OBJECT
public:
    explicit QAx7zFbScreen(const QStringList &args);
    ~QAx7zFbScreen() override;
    bool initialize() override;
    QRegion doRedraw() override;
    QPixmap grabWindow(WId wid, int x, int y, int width, int height) const override;
private:
    bool pan(int page);
    void logStats(const char *reason) const;
    QStringList mArgs;
    int mFbFd = -1;
    int mTtyFd = -1;
    int mOldTtyMode = -1;
    uchar *mMapping = nullptr;
    fb_var_screeninfo mVar = {};
    Ax7zFb::FlipBuffer mFlip;
    quint64 mSubmits = 0, mPixels = 0, mRects = 0;
    qint64 mCopyNs = 0, mPanNs = 0, mMaxPanNs = 0;
    int mStatsInterval = 0;
};
QT_END_NAMESPACE
