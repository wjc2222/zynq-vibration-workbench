#pragma once

// Native, CPU-painted icons for the Qt Widgets workstation. No SVG parser,
// font glyph, image file, QtSvg library, or GPU is needed. QPixmap creation and
// this small icon cache belong to the QApplication GUI thread.

#include <QColor>
#include <QFont>
#include <QHash>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPixmap>
#include <QPointF>
#include <QPushButton>
#include <QRectF>
#include <QSizePolicy>
#include <QString>
#include <QtGlobal>
#include <initializer_list>

namespace vibration::gui {

enum class NativeIcon {
    Record, Wave, Spectrum, Speed, Detection, Settings, Export, Folder,
    Refresh, Plus, Minus, Left, Right, Fit, Clock, Info, Alert, Down,
    Check, Source, Calibration, More, Sidebar
};

// Coordinates are in a 24 x 24 viewBox. Letterboxing keeps strokes and
// proportions identical when the destination rectangle is not square.
inline void paintNativeIcon(QPainter& painter, NativeIcon icon,
                            const QRectF& destination, const QColor& color,
                            qreal strokeWidth = 1.7) {
    const qreal side = qMin(destination.width(), destination.height());
    if (side <= 0) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.translate(destination.center().x() - side / 2,
                      destination.center().y() - side / 2);
    painter.scale(side / 24.0, side / 24.0);
    painter.setPen(QPen(color, strokeWidth, Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);

    const auto line = [&](qreal x1, qreal y1, qreal x2, qreal y2) {
        painter.drawLine(QPointF(x1, y1), QPointF(x2, y2));
    };
    const auto polyline = [&](std::initializer_list<QPointF> points,
                              bool close = false) {
        QPainterPath path;
        bool first = true;
        for (const auto& point : points) {
            if (first) { path.moveTo(point); first = false; }
            else path.lineTo(point);
        }
        if (close && !first) path.closeSubpath();
        painter.drawPath(path);
    };
    const auto dot = [&](qreal x, qreal y, qreal radius = 1.0) {
        painter.save();
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(QPointF(x, y), radius, radius);
        painter.restore();
    };
    const auto shield = [&] {
        QPainterPath path;
        path.moveTo(12, 2); path.lineTo(20, 6); path.lineTo(20, 13);
        path.cubicTo(20, 18, 12, 22, 12, 22);
        path.cubicTo(12, 22, 4, 18, 4, 13);
        path.lineTo(4, 6); path.closeSubpath();
        painter.drawPath(path);
    };

    switch (icon) {
    case NativeIcon::Record:
        polyline({{5, 3}, {15, 3}, {19, 7}, {19, 21}, {5, 21}}, true);
        polyline({{15, 3}, {15, 8}, {19, 8}});
        line(8, 12, 16, 12); line(8, 16, 14, 16);
        break;
    case NativeIcon::Wave:
        polyline({{2, 12}, {6, 12}, {8, 4}, {12, 20},
                  {16, 4}, {18, 12}, {22, 12}});
        break;
    case NativeIcon::Spectrum:
        line(3, 20, 21, 20); line(5, 17, 5, 13); line(9, 17, 9, 7);
        line(13, 17, 13, 3); line(17, 17, 17, 9); line(21, 17, 21, 12);
        break;
    case NativeIcon::Speed: {
        // The upper 240-degree dial leaves a deliberate opening at the bottom.
        QPainterPath path;
        path.arcMoveTo(QRectF(3, 3, 18, 18), -30);
        path.arcTo(QRectF(3, 3, 18, 18), -30, 240);
        painter.drawPath(path);
        line(12, 12, 17, 7); line(5, 15, 7, 14); line(5, 8, 7, 9);
        line(12, 3, 12, 6); line(19, 8, 17, 9);
        painter.drawEllipse(QPointF(12, 12), 2, 2);
        break;
    }
    case NativeIcon::Detection:
        shield(); polyline({{8, 12}, {11, 15}, {16, 9}});
        break;
    case NativeIcon::Settings:
        line(4, 7, 20, 7); line(4, 17, 20, 17);
        painter.save(); painter.setBrush(color);
        painter.drawEllipse(QPointF(9, 7), 3, 3);
        painter.drawEllipse(QPointF(16, 17), 3, 3);
        painter.restore();
        break;
    case NativeIcon::Export:
        line(12, 3, 12, 15); polyline({{8, 7}, {12, 3}, {16, 7}});
        polyline({{4, 14}, {4, 20}, {20, 20}, {20, 14}});
        break;
    case NativeIcon::Folder:
        polyline({{3, 6}, {10, 6}, {12, 9}, {21, 9}, {21, 21}, {3, 21}}, true);
        polyline({{3, 6}, {3, 4}, {10, 4}, {12, 6}, {19, 6}, {19, 9}});
        break;
    case NativeIcon::Refresh: {
        QPainterPath path;
        path.arcMoveTo(QRectF(3, 3, 18, 18), 34);
        path.arcTo(QRectF(3, 3, 18, 18), 34, 300);
        painter.drawPath(path);
        polyline({{20, 3}, {20, 8}, {15, 8}});
        break;
    }
    case NativeIcon::Plus:
        line(5, 12, 19, 12); line(12, 5, 12, 19);
        break;
    case NativeIcon::Minus:
        line(5, 12, 19, 12);
        break;
    case NativeIcon::Left:
        polyline({{14, 5}, {7, 12}, {14, 19}});
        break;
    case NativeIcon::Right:
        polyline({{10, 5}, {17, 12}, {10, 19}});
        break;
    case NativeIcon::Fit:
        polyline({{8, 3}, {3, 3}, {3, 8}});
        polyline({{16, 3}, {21, 3}, {21, 8}});
        polyline({{3, 16}, {3, 21}, {8, 21}});
        polyline({{21, 16}, {21, 21}, {16, 21}});
        line(7, 12, 17, 12);
        break;
    case NativeIcon::Clock:
        painter.drawEllipse(QPointF(12, 12), 9, 9);
        polyline({{12, 6}, {12, 12}, {16, 14}});
        break;
    case NativeIcon::Info:
        painter.drawEllipse(QPointF(12, 12), 9, 9);
        line(12, 11, 12, 17); dot(12, 7, 0.9);
        break;
    case NativeIcon::Alert:
        polyline({{12, 3}, {2, 21}, {22, 21}}, true);
        line(12, 9, 12, 14); dot(12, 18, 0.9);
        break;
    case NativeIcon::Down:
        polyline({{6, 9}, {12, 15}, {18, 9}});
        break;
    case NativeIcon::Check:
        polyline({{5, 12}, {10, 17}, {20, 7}});
        break;
    case NativeIcon::Source:
        polyline({{3, 12}, {6, 12}, {8, 5}, {12, 19},
                  {16, 5}, {18, 12}, {21, 12}});
        line(4, 21, 20, 3);
        break;
    case NativeIcon::Calibration:
        polyline({{5, 3}, {5, 21}, {21, 21}});
        line(5, 8, 9, 8); line(5, 13, 9, 13); line(5, 18, 9, 18);
        line(10, 21, 10, 17); line(15, 21, 15, 17); line(20, 21, 20, 17);
        polyline({{12, 8}, {15, 11}, {21, 4}});
        break;
    case NativeIcon::Sidebar:
        painter.drawRoundedRect(QRectF(3,4,18,16),2,2);
        line(9,4,9,20);
        break;
    case NativeIcon::More:
        dot(5, 12); dot(12, 12); dot(19, 12);
        break;
    }
    painter.restore();
}

// Render once at 2x resolution, then reuse an implicit-shared QIcon. Keeping a
// transparent background lets the same icon work on white cards and blue
// selected controls. QIcon handles scaling for the actual paint destination.
inline QIcon nativeIcon(NativeIcon icon, const QColor& color = QColor("#526479"),
                        int size = 24) {
    const int logicalSize = qBound(1, size, 256);
    const QString key = QString::number(static_cast<int>(icon)) + QLatin1Char(':')
        + QString::number(color.rgba(), 16) + QLatin1Char(':')
        + QString::number(logicalSize);
    static QHash<QString, QIcon> cache;
    const auto found = cache.constFind(key);
    if (found != cache.cend()) return found.value();

    const auto render = [&](const QColor& foreground) {
        QPixmap image(logicalSize * 2, logicalSize * 2);
        image.setDevicePixelRatio(2.0);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        paintNativeIcon(painter, icon, QRectF(0, 0, logicalSize, logicalSize), foreground);
        painter.end();
        return image;
    };
    QIcon result;
    const QPixmap normal = render(color);
    QColor disabled = color;
    disabled.setAlpha(qRound(color.alpha() * 0.43));
    const QPixmap inactive = render(disabled);
    result.addPixmap(normal, QIcon::Normal, QIcon::Off);
    result.addPixmap(normal, QIcon::Normal, QIcon::On);
    result.addPixmap(inactive, QIcon::Disabled, QIcon::Off);
    result.addPixmap(inactive, QIcon::Disabled, QIcon::On);
    cache.insert(key, result);
    return result;
}

// Ordinary QPushButton semantics (including group exclusivity, keyboard
// activation and accessibility) in a quiet, horizontal workspace sidebar.
class NavigationButton final : public QPushButton {
public:
    explicit NavigationButton(const QString& label, NativeIcon icon,
                              QWidget* parent = nullptr)
        : QPushButton(label, parent), icon_(icon) {
        setCheckable(true);
        setMinimumSize(140, 44);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_Hover, true);
    }

    QSize sizeHint() const override { return QSize(168, 44); }
    QSize minimumSizeHint() const override { return QSize(140, 44); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF bounds = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const bool selected = isChecked();
        QColor fill = Qt::transparent;
        if (selected) fill = QColor("#e8ebef");
        else if (underMouse()) fill = QColor("#eff1f4");
        if (isDown()) fill = QColor("#e1e5ea");
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(bounds, 8, 8);

        QColor iconColor(selected ? "#2c333c" : "#7c8591");
        QColor textColor(selected ? "#2c333c" : "#596371");
        if (!isEnabled()) { iconColor.setAlpha(105); textColor.setAlpha(105); }
        paintNativeIcon(painter, icon_, QRectF(12, (height()-20)/2.0, 20, 20), iconColor);

        QFont labelFont = font();
        labelFont.setPixelSize(14);
        labelFont.setWeight(selected ? QFont::DemiBold : QFont::Normal);
        painter.setFont(labelFont);
        painter.setPen(textColor);
        painter.drawText(QRectF(44, 0, width() - 54, height()),
                         Qt::AlignLeft | Qt::AlignVCenter, text());
        if (hasFocus()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor("#a9b4c3"), 1));
            painter.drawRoundedRect(bounds.adjusted(1, 1, -1, -1), 7, 7);
        }
    }

private:
    NativeIcon icon_;
};

} // namespace vibration::gui
