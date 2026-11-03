"""Small native Qt plots. Drawing never interpolates across missing samples."""
from __future__ import annotations

import math
import uuid
from PySide6.QtCore import Qt, QMimeData, Signal, QRectF, QPointF
from PySide6.QtGui import QColor, QDrag, QFontMetricsF, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import (QFrame, QHBoxLayout, QLabel, QPushButton, QVBoxLayout, QWidget,
    QMenu, QDialog, QFormLayout, QLineEdit, QDialogButtonBox)

COLORS = ("#3569c8", "#198b7c", "#b27b2f")


def nice_ticks(lower, upper, target=6):
    """Readable ticks without moving or rounding any data coordinate."""
    raw = (upper-lower)/max(2, target)
    if not math.isfinite(raw) or raw <= 0:return [], 1.
    scale = 10**math.floor(math.log10(raw))
    step = next(n*scale for n in (1, 2, 2.5, 5, 10) if n*scale >= raw)
    first = math.ceil(lower/step-1e-9)*step
    values = [first+i*step for i in range(max(0, min(100, int((upper-first)/step+1e-8)+1)))]
    return values, step


def tick_text(value, step):
    decimals = max(0, min(10, -math.floor(math.log10(abs(step))) + (1 if abs(step/10**math.floor(math.log10(abs(step)))-2.5)<1e-8 else 0)))
    if abs(value)<abs(step)*1e-8:value=0.
    return f"{value:.{decimals}f}"


def finite(value):
    return isinstance(value, (int, float)) and math.isfinite(value)


def envelope(points, columns):
    """First/min/max/last per pixel bucket, retain each missing-data boundary."""
    if len(points) <= columns * 4:
        return points
    result, segment = [], []

    def flush():
        if not segment:
            return
        start, end = segment[0][0], segment[-1][0]
        buckets = {}
        for index, point in enumerate(segment):
            bucket = int((point[0] - start) / max(end - start, 1e-30) * max(1, columns - 1))
            buckets.setdefault(bucket, []).append(index)
        for indices in buckets.values():
            keep = sorted({indices[0], indices[-1], min(indices, key=lambda i: segment[i][1]), max(indices, key=lambda i: segment[i][1])})
            result.extend(segment[i] for i in keep)
        segment.clear()

    for point in points:
        if len(point) != 2 or not finite(point[0]) or not finite(point[1]):
            flush()
            result.append(point)
        else:
            segment.append(point)
    flush()
    return result


class Viewport:
    """Numeric navigation only; never transforms or replaces source samples."""
    def __init__(self):
        self.domain = (0., 1.)
        self.default_range = None
        self.x_range = None
        self.y_range = None

    @staticmethod
    def valid_range(lower, upper):
        return finite(lower) and finite(upper) and lower < upper and math.isfinite(upper-lower)

    def set_y(self, lower, upper):
        if not self.valid_range(lower, upper):
            return False
        self.y_range = (float(lower), float(upper))
        return True

    def set_x(self, lower, upper):
        if not self.valid_range(lower, upper):
            return False
        d0, d1 = self.domain
        width = min(upper-lower, d1-d0)
        minimum = min(d1-d0, max(1e-12, max(abs(d0), abs(d1)) * 1e-14))
        width = max(width, minimum)
        lower = max(d0, min(lower, d1-width))
        self.x_range = (lower, lower+width)
        return True

    def zoom(self, factor, anchor=None):
        if not finite(factor) or factor <= 0:
            return False
        lower, upper = self.x_range or self.default_range or self.domain
        anchor = (lower+upper)/2 if anchor is None else anchor
        if not finite(anchor):
            return False
        anchor = max(lower, min(anchor, upper))
        ratio = (anchor-lower)/(upper-lower)
        width = min((upper-lower)*factor, self.domain[1]-self.domain[0])
        return self.set_x(anchor-ratio*width, anchor+(1-ratio)*width)

    def pan(self, fraction):
        if not finite(fraction):
            return False
        lower, upper = self.x_range or self.default_range or self.domain
        return self.set_x(lower+(upper-lower)*fraction, upper+(upper-lower)*fraction)


class Plot(QWidget):
    navigation_started = Signal()
    view_changed = Signal()
    resume_requested = Signal()
    stats_changed = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.series = []
        self.x_label = "时间 / s"
        self.y_label = "V"
        self.viewport = Viewport()
        self.zero_centered_y = False
        self.navigation_mode = "pan"
        self.following_live = True
        self.live_mode = False
        self.last_error = ""
        self._drag = None
        self._drag_kind = "pan"
        self._pending_series = None
        self._source_id = None
        self.embedded=False
        self.empty_message=None
        self.interval_series=[]
        self.raw_provider=None
        self.samples_are_summary=False
        self._provider_cache=None
        self.setMinimumHeight(175)
        self.setMouseTracking(True)
        self.setToolTip("左键拖动平移 · 滚轮缩放 · Shift＋拖动框选 · 双击复位\n纵轴上拖动或滚轮调整幅值；右键设置精确范围")

    def set_series(self, series, x_label="时间 / s", y_label="V", source_id=None):
        changed = y_label != self.y_label or (source_id is not None and source_id != self._source_id)
        if changed:
            self.interval_series=[];self.raw_provider=None;self._provider_cache=None
            self.samples_are_summary=False;self.viewport.default_range=None
            self.viewport.y_range = None
            self.viewport.x_range = None
            self._source_id = source_id
            self._pending_series = None
        if self.live_mode and not self.following_live and not changed:
            self._pending_series = (series, x_label, y_label, source_id)
            return
        self.series, self.x_label, self.y_label = series, x_label, y_label
        times = [point[0] for _, points in series for point in points if len(point) == 2 and finite(point[0])]
        if times:
            lower, upper = min(times), max(times)
            if upper <= lower:
                lower, upper = lower-.5, lower+.5
            self.viewport.domain = (lower, upper)
            if self.viewport.x_range:
                self.viewport.set_x(*self.viewport.x_range)
        if self.live_mode and self.following_live:
            self.viewport.x_range = None
        self.view_changed.emit()
        self.update()

    @property
    def paused(self):
        return self.live_mode and not self.following_live

    def set_live_mode(self, enabled):
        self.live_mode, self.following_live = bool(enabled), True
        self._pending_series = None
        self.viewport.x_range = None
        self.view_changed.emit()

    def pause_live(self):
        if self.live_mode:
            self.following_live = False
            self.view_changed.emit()

    def resume_live(self):
        self.following_live = True
        pending, self._pending_series = self._pending_series, None
        if pending:
            self.set_series(*pending)
        self.viewport.x_range = None
        self.resume_requested.emit()
        self.view_changed.emit()
        self.update()

    def _navigate(self):
        self.navigation_started.emit()
        self.pause_live()

    def set_y_range(self, lower, upper):
        if self.zero_centered_y and self.viewport.valid_range(lower, upper):
            bound = max(abs(lower), abs(upper))
            lower, upper = -bound, bound
        if not self.viewport.set_y(lower, upper):
            self.last_error = "幅值下限和上限须为有限数，且下限小于上限"
            return False
        self.last_error = ""
        self.update(); self.view_changed.emit()
        return True

    def set_auto_y(self):
        self.viewport.y_range = None
        self.update(); self.view_changed.emit()

    def set_x_range(self, lower, upper, notify=True):
        if self.viewport.set_x(lower, upper):
            self.update()
            if notify: self.view_changed.emit()
            return True
        return False

    def zoom(self, factor, anchor=None):
        if not finite(factor) or factor <= 0 or (anchor is not None and not finite(anchor)):
            return False
        self._navigate()
        result = self.viewport.zoom(factor, anchor)
        self.update(); self.view_changed.emit()
        return result

    def pan(self, fraction):
        if not finite(fraction): return False
        self._navigate()
        result = self.viewport.pan(fraction)
        self.update(); self.view_changed.emit()
        return result

    def reset_view(self):
        self._navigate()
        self.viewport.x_range = None
        self.set_auto_y()

    def set_navigation_mode(self, mode):
        if mode not in ("cursor", "pan", "box"):
            raise ValueError("unknown navigation mode")
        self.navigation_mode = mode
        self._drag = None
        self.setCursor(Qt.CursorShape.CrossCursor if mode == "box" else Qt.CursorShape.OpenHandCursor)

    def wheelEvent(self, event):
        layout = self._axis_layout(); area = layout["area"]
        position=event.position(); delta=event.angleDelta().y()
        if self._on_y_axis(position,area) and delta:
            self._navigate()
            _,_,lower,upper=layout["ranges"]
            anchor=upper-(position.y()-area.top())/area.height()*(upper-lower)
            if self.zero_centered_y:anchor=0.
            factor=0.8 ** (delta/120)
            self.set_y_range(anchor+(lower-anchor)*factor,anchor+(upper-anchor)*factor)
            event.accept()
        elif area.contains(position) and delta:
            x0, x1, _, _ = layout["ranges"]
            anchor = x0+(event.position().x()-area.left())/area.width()*(x1-x0)
            self.zoom(0.8 ** (event.angleDelta().y()/120), anchor)
            event.accept()
        else: event.ignore()

    @staticmethod
    def _on_y_axis(position,area):
        return 0<=position.x()<area.left() and area.top()<=position.y()<=area.bottom()

    def mousePressEvent(self, event):
        layout=self._axis_layout(); area=layout["area"]
        if event.button() == Qt.MouseButton.LeftButton and (area.contains(event.position()) or self._on_y_axis(event.position(),area)):
            self._navigate()
            self._drag_kind=("y" if self._on_y_axis(event.position(),area) else
                "box" if event.modifiers() & Qt.KeyboardModifier.ShiftModifier or self.navigation_mode=="box" else "pan")
            bounds=layout["ranges"][2:] if self._drag_kind=="y" else self.viewport.x_range or self.viewport.default_range or self.viewport.domain
            self._drag = (QPointF(event.position()), QPointF(event.position()), bounds)
            self.setCursor(Qt.CursorShape.ClosedHandCursor if self._drag_kind=="pan" else Qt.CursorShape.SizeVerCursor if self._drag_kind=="y" else Qt.CursorShape.CrossCursor)
            event.accept()
        else: super().mousePressEvent(event)

    def mouseMoveEvent(self, event):
        if self._drag:
            origin, _, bounds = self._drag
            area = self._axis_layout()["area"]
            if self._drag_kind == "pan":
                shift = (origin.x()-event.position().x())/area.width()*(bounds[1]-bounds[0])
                self.set_x_range(bounds[0]+shift, bounds[1]+shift)
            elif self._drag_kind == "y":
                fraction=(event.position().y()-origin.y())/area.height()
                if self.zero_centered_y:
                    bound=max(abs(bounds[0]),abs(bounds[1]))*math.exp(max(-20.,min(20.,fraction*2.)))
                    self.set_y_range(-bound,bound)
                else:
                    shift=fraction*(bounds[1]-bounds[0])
                    self.set_y_range(bounds[0]+shift,bounds[1]+shift)
            self._drag = (origin, QPointF(event.position()), bounds)
            self.update(); event.accept()
        else:
            area=self._axis_layout()["area"]
            self.setCursor(Qt.CursorShape.SizeVerCursor if self._on_y_axis(event.position(),area) else Qt.CursorShape.OpenHandCursor if area.contains(event.position()) else Qt.CursorShape.ArrowCursor)
            super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event):
        if self._drag and event.button() == Qt.MouseButton.LeftButton:
            origin, _, bounds = self._drag; self._drag = None
            if self._drag_kind == "box" and abs(origin.x()-event.position().x()) >= 6:
                area = self._axis_layout()["area"]
                def at(position): return bounds[0]+max(0,min(1,(position.x()-area.left())/area.width()))*(bounds[1]-bounds[0])
                self.set_x_range(*sorted((at(origin), at(event.position()))))
            self.setCursor(Qt.CursorShape.OpenHandCursor); self.update(); event.accept()
        else: super().mouseReleaseEvent(event)

    def mouseDoubleClickEvent(self,event):
        if event.button()==Qt.MouseButton.LeftButton:
            self._drag=None; self.reset_view(); event.accept()
        else:super().mouseDoubleClickEvent(event)

    def contextMenuEvent(self,event):
        menu=QMenu(self)
        menu.addAction("恢复默认视图",self.reset_view)
        if self.interval_series or self.raw_provider:
            menu.addAction("窗口包络概览",lambda:self._show_overview())
        menu.addAction("自动幅值",self.set_auto_y)
        menu.addAction("设置幅值范围…",lambda:AmplitudeDialog(self).exec())
        if self.live_mode:
            menu.addSeparator()
            menu.addAction("回到最新" if self.paused else "暂停显示",self.resume_live if self.paused else self._navigate)
        menu.exec(event.globalPos())

    def _show_overview(self):
        self._navigate();self.set_x_range(*self.viewport.domain)

    def set_sampling_context(self, intervals=(), domain=None, detail=None, provider=None, summary=False):
        self.interval_series=list(intervals);self.raw_provider=provider;self._provider_cache=None
        self.samples_are_summary=summary
        if domain is not None:self.viewport.domain=tuple(domain)
        self.viewport.default_range=tuple(detail) if detail is not None else None
        self.update()

    def _visible_series(self):
        if self.raw_provider is None:return self.series,self.interval_series
        if not self.series:return [],[]
        bounds=self.viewport.x_range or self.viewport.default_range or self.viewport.domain
        # The file reader caches samples but still verifies file identity on
        # every call. Do not bypass that guard with a second paint cache.
        return self.raw_provider(bounds[0],bounds[1],max(64,self.width()-100))

    def _axis_layout(self):
        """Measure the actual tick text before reserving its drawing space."""
        try:
            series,interval_series=self._visible_series()
        except (OSError,ValueError,RuntimeError) as exc:
            self.last_error=str(exc);series=[];interval_series=[]
        all_points = [xy for _, data in series for xy in data if len(xy) == 2 and finite(xy[0]) and finite(xy[1])]
        all_times = [xy[0] for _, data in series for xy in data if len(xy) == 2 and finite(xy[0])]
        x0,x1=self.viewport.x_range or self.viewport.default_range or self.viewport.domain
        if x1<=x0:x1=x0+1
        all_points = [point for point in all_points if x0 <= point[0] <= x1]
        # Each interval is [start, end, minimum, maximum]. Never connect
        # extrema from separate buckets as if they were consecutive samples.
        bins=[b for _,values in interval_series for b in values if len(b)==4 and all(finite(v) for v in b) and b[0]<=x1 and b[1]>=x0]
        for begin,end,minimum,maximum in bins:all_points.extend(((max(x0,begin),minimum),(min(x1,end),maximum)))
        if all_points:
            y0, y1 = min(pt[1] for pt in all_points), max(pt[1] for pt in all_points)
            if y1 <= y0:
                y0 -= max(1e-6, abs(y0) * .1, .01)
                y1 += max(1e-6, abs(y1) * .1, .01)
            else:
                pad = (y1 - y0) * .08
                y0 -= pad
                y1 += pad
        else:
            y0, y1 = -1, 1
        if self.viewport.y_range is not None:
            y0, y1 = self.viewport.y_range
            if self.zero_centered_y:
                bound=max(abs(y0),abs(y1));y0,y1=-bound,bound
        elif self.x_label.startswith("频率") and all_points and all(point[1]>=0 for point in all_points):
            magnitude=max(y1,1e-12);scale=10**math.floor(math.log10(magnitude))
            y0=0.;y1=next(step*scale for step in (1,2,2.5,5,10) if step*scale>=magnitude)
        elif self.zero_centered_y or (y0 < 0 < y1 and not self.x_label.startswith("频率") and self.y_label not in ("rpm","RPM","Hz")):
            # A zero-centred vibration axis uses readable engineering bounds;
            # only the viewport changes, never the plotted or stored values.
            magnitude=max(abs(y0),abs(y1))
            scale=10**math.floor(math.log10(magnitude))
            bound=next(step*scale for step in (1,2,2.5,5,10) if step*scale>=magnitude)
            y0,y1=-bound,bound
        metrics = QFontMetricsF(self.font(), self)
        line_height = math.ceil(metrics.height())
        spectrum=self.x_label.startswith("频率")
        x_scale,x_origin,x_title=1.,0.,self.x_label
        if not spectrum and self.x_label.startswith("时间") and x1-x0<.2:
            x_scale=1000.
            x_origin=x0 if abs(x0)>max(x1-x0,1e-12)*10 else 0.
            x_title="时间 / ms"+(f"  ·  起点 {x_origin:.6f} s" if x_origin else "")
        y_scale,y_title=1.,self.y_label
        magnitude=max(abs(y0),abs(y1))
        if self.y_label=="V" and 0<magnitude<.01:y_scale,y_title=1000.,"mV"
        if self.y_label=="V" and 0<magnitude<.00001:y_scale,y_title=1e6,"µV"
        y_title=("峰值幅度 / " if spectrum else "幅值 / " if self.y_label not in ("rpm","Hz") else "")+y_title
        x_values,x_step=nice_ticks((x0-x_origin)*x_scale,(x1-x_origin)*x_scale,max(3,min(8,self.width()//130)))
        y_values,y_step=nice_ticks(y0*y_scale,y1*y_scale,6)
        x_labels=[tick_text(v,x_step) for v in x_values]
        y_labels=[tick_text(v,y_step) for v in y_values]
        def text_width(label):
            bounds = metrics.boundingRect(label)
            return max(metrics.horizontalAdvance(label), bounds.width()) + max(0, -bounds.left())
        left = max(64, math.ceil(max(map(text_width, y_labels),default=40)) + 16)
        top = 12 if self.embedded else max(26, math.ceil(line_height * 1.5 + 4))
        bottom = line_height * 2 + 10
        area = QRectF(left, top, max(1, self.width() - left - 20), max(1, self.height() - top - bottom))
        y_positions=[area.bottom()-(value/y_scale-y0)/(y1-y0)*area.height() for value in y_values]
        x_positions=[area.left()+(value/x_scale+x_origin-x0)/(x1-x0)*area.width() for value in x_values]
        y_ticks = [(label, QRectF(4, position-line_height/2,left-12,line_height)) for label,position in zip(y_labels,y_positions)]
        x_ticks = []
        for x, label in zip(x_positions,x_labels):
            width = max(70, math.ceil(text_width(label)) + 8)
            # End ticks may move within their row, but must never leave the widget.
            label_left = max(4, min(x - width / 2, self.width() - width - 4))
            x_ticks.append((label, QRectF(label_left, area.bottom() + 4, width, line_height)))
        unit_width = min(self.width() - 8, max(95, math.ceil(text_width(x_title)) + 8))
        return dict(area=area, ranges=(x0, x1, y0, y1), y_ticks=y_ticks, x_ticks=x_ticks,
                    x_positions=x_positions,y_positions=y_positions,x_title=x_title,y_title=y_title,series=series,interval_series=interval_series,
                    y_unit=QRectF(4, 1, self.width() - 8, line_height),
                    x_unit=QRectF(max(4, self.width() - unit_width - 20), self.height() - line_height - 2,
                    unit_width, line_height), has_data=bool(all_points), has_time=bool(all_times or bins))

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.fillRect(self.rect(), QColor("white"))
        layout = self._axis_layout()
        area = layout["area"]
        x0, x1, y0, y1 = layout["ranges"]
        p.setPen(QPen(QColor("#e9edf2"), 1))
        for x in layout["x_positions"]:
            p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()))
        for y in layout["y_positions"]:
            p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y))
        p.setPen(QColor("#4f5966"))
        for label, rect in layout["y_ticks"]:
            p.drawText(rect, Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter, label)
        for label, rect in layout["x_ticks"]:
            p.drawText(rect, Qt.AlignmentFlag.AlignCenter, label)
        p.drawText(layout["x_unit"], Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter, layout["x_title"])
        if not self.embedded:p.drawText(layout["y_unit"], Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter, layout["y_title"])
        if not layout["has_data"]:
            p.setPen(QColor("#56616e"))
            p.drawText(area, Qt.AlignmentFlag.AlignCenter, "文件已改变，请重新打开" if self.last_error and self.raw_provider else "当前窗口无有效数据" if layout["has_time"] else self.empty_message or "等待数据")
            return
        p.save()
        p.setClipRect(area.adjusted(-1, -1, 1, 1))
        raw_bounds=[(min(pt[0] for pt in data if finite(pt[0])),max(pt[0] for pt in data if finite(pt[0]))) for _,data in layout['series'] if data and any(finite(pt[0]) for pt in data)]
        detail_visible=bool(raw_bounds) and all(begin<=x0 and end>=x1 for begin,end in raw_bounds)
        if not detail_visible or self.raw_provider:
            for color,values in layout['interval_series']:
                shade=QColor(color);shade.setAlpha(60);p.setPen(Qt.PenStyle.NoPen)
                for begin,end,minimum,maximum in values:
                    if end<x0 or begin>x1:continue
                    left=area.left()+(max(x0,begin)-x0)/(x1-x0)*area.width()
                    right=area.left()+(min(x1,end)-x0)/(x1-x0)*area.width()
                    top=area.bottom()-(maximum-y0)/(y1-y0)*area.height()
                    bottom=area.bottom()-(minimum-y0)/(y1-y0)*area.height()
                    p.fillRect(QRectF(left,top,max(1.,right-left),max(1.,bottom-top)),shade)
        for color, data in layout['series']:
            path = QPainterPath()
            active = False
            # Clip before pixel reduction; every explicit gap remains a break.
            visible = [point for point in data if len(point) != 2 or not finite(point[0]) or not finite(point[1]) or x0 <= point[0] <= x1]
            for point in envelope(visible, int(area.width())):
                if len(point) != 2 or not finite(point[0]) or not finite(point[1]):
                    active = False
                    continue
                x = area.left() + (point[0] - x0) / (x1 - x0) * area.width()
                y = area.bottom() - (point[1] - y0) / (y1 - y0) * area.height()
                if self.samples_are_summary:
                    p.setPen(QPen(QColor(color),2));p.drawPoint(QPointF(x,y));active=False;continue
                if active: path.lineTo(x, y)
                else: path.moveTo(x, y)
                active = True
            p.setPen(QPen(QColor(color), 1.6))
            p.drawPath(path)
        p.restore()
        if self._drag and self._drag_kind == "box":
            origin, current, _ = self._drag
            box = QRectF(QPointF(min(origin.x(),current.x()),area.top()), QPointF(max(origin.x(),current.x()),area.bottom())).intersected(area)
            p.fillRect(box, QColor(53,105,200,30)); p.setPen(QPen(QColor("#3569c8"),1,Qt.PenStyle.DashLine)); p.drawRect(box)


class AmplitudeDialog(QDialog):
    def __init__(self,target):
        super().__init__(target)
        self.target=target; self.setWindowTitle("幅值范围"); self.setMinimumWidth(340)
        box=QVBoxLayout(self); form=QFormLayout()
        lower,upper=target._axis_layout()["ranges"][2:]
        self.lower=QLineEdit(f"{lower:.12g}"); self.upper=QLineEdit(f"{upper:.12g}")
        form.addRow("下限 / "+target.y_label,self.lower); form.addRow("上限 / "+target.y_label,self.upper); box.addLayout(form)
        self.note=QLabel(); self.note.setWordWrap(True); box.addWidget(self.note)
        buttons=QDialogButtonBox(QDialogButtonBox.StandardButton.Ok|QDialogButtonBox.StandardButton.Cancel)
        buttons.button(QDialogButtonBox.StandardButton.Ok).setText("应用")
        buttons.button(QDialogButtonBox.StandardButton.Cancel).setText("取消")
        buttons.accepted.connect(self.apply_range); buttons.rejected.connect(self.reject); box.addWidget(buttons)

    def apply_range(self):
        try: lower,upper=float(self.lower.text()),float(self.upper.text())
        except ValueError:
            self.note.setText("请输入有限数，下限须小于上限"); return
        if not Viewport.valid_range(lower,upper):
            self.note.setText("请输入有限数，下限须小于上限"); return
        self.target.set_y_range(lower,upper); self.accept()


class DragButton(QPushButton):
    def __init__(self, text, kind, token, parent=None):
        super().__init__(text, parent)
        self.kind, self.token, self.origin = kind, token, None

    def mousePressEvent(self, event):
        self.origin = event.position().toPoint()
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event):
        if self.origin is not None and (event.position().toPoint() - self.origin).manhattanLength() >= 8:
            mime = QMimeData()
            mime.setData("application/x-vibration-" + self.kind, str(self.token).encode())
            drag = QDrag(self)
            drag.setMimeData(mime)
            # A successful drop rebuilds the rows and may delete this button
            # while the native drag runs its nested event loop.
            self.origin = None
            self.setDown(False)
            drag.exec(Qt.DropAction.MoveAction)


class RowGrip(DragButton):
    def __init__(self,token):
        super().__init__("","row",token)
        self.setFixedSize(24,28);self.setToolTip("拖动调整顺序");self.setCursor(Qt.CursorShape.OpenHandCursor)

    def paintEvent(self,event):
        painter=QPainter(self);painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(Qt.PenStyle.NoPen);painter.setBrush(QColor("#adb3bd"))
        for x in (9,15):
            for y in (8,14,20):painter.drawEllipse(QPointF(x,y),1.2,1.2)


class PlotRow(QFrame):
    merge_requested = Signal(str, int)
    reorder_requested = Signal(str, str)
    split_requested = Signal(str)

    def __init__(self, row_id, channels, overview=False, parent=None):
        super().__init__(parent)
        self.row_id, self.channels, self.overview = row_id, list(channels), overview
        self.is_speed=row_id=="speed"
        self.setAcceptDrops(True)
        self.setObjectName("plotRow")
        box = QVBoxLayout(self)
        box.setContentsMargins(10, 12, 14, 10);box.setSpacing(8)
        head = QHBoxLayout()
        head.setSpacing(6);head.addWidget(RowGrip(row_id))
        if overview or self.is_speed:
            label=QLabel("三轴叠加" if overview else "转速");label.setObjectName("rowTitle");head.addWidget(label)
        else:
            for channel in channels:
                chip = DragButton(f"CH{channel + 1}", "channel", channel)
                chip.setStyleSheet(f"color:{COLORS[channel]}; background:transparent; border:0; padding:2px 6px;font-weight:600;")
                chip.setToolTip("拖入其他振动栏叠加显示")
                head.addWidget(chip)
            if len(channels) > 1:
                split = QPushButton("拆分")
                split.clicked.connect(lambda: self.split_requested.emit(self.row_id))
                head.addWidget(split)
        head.addStretch()
        self.readout=QLabel();self.readout.setObjectName("muted");head.addWidget(self.readout)
        box.addLayout(head)
        self.plot = Plot();self.plot.embedded=True;self.plot.setMinimumHeight(190)
        box.addWidget(self.plot)

    def dragEnterEvent(self, event):
        if event.mimeData().hasFormat("application/x-vibration-row") or (not self.overview and not self.is_speed and event.mimeData().hasFormat("application/x-vibration-channel")):
            event.acceptProposedAction()

    def dropEvent(self, event):
        if event.mimeData().hasFormat("application/x-vibration-row"):
            self.reorder_requested.emit(bytes(event.mimeData().data("application/x-vibration-row")).decode(), self.row_id)
        elif not self.overview and not self.is_speed:
            self.merge_requested.emit(self.row_id, int(bytes(event.mimeData().data("application/x-vibration-channel"))))
        else:event.ignore();return
        event.acceptProposedAction()


class WaveformStack(QWidget):
    navigation_started = Signal()
    view_changed = Signal()
    resume_requested = Signal()
    stats_changed = Signal()

    def __init__(self):
        super().__init__()
        self.box = QVBoxLayout(self)
        self.box.setContentsMargins(0, 0, 0, 0)
        self.box.setSpacing(8)
        self.data = [[], [], []]
        self.speed_data=[];self.speed_unit="rpm";self.speed_y_range=None
        self.unit = "V"
        self.viewport = Viewport()
        self.live_mode = False
        self.following_live = True
        self.navigation_mode = "pan"
        self.last_error = ""
        self._source_id = None
        self._pending_data = None
        self._updating = False
        self._stats = []
        self._stats_scope = ""
        self._stats_count = None
        self.reset_layout()

    def reset_layout(self):
        self.groups = [("ch1", [0]), ("ch2", [1]), ("ch3", [2]), ("overview", [0, 1, 2]),("speed",[])]
        self.rebuild()

    def rebuild(self):
        while self.box.count():
            widget = self.box.takeAt(0).widget()
            if widget: widget.deleteLater()
        self.rows = []
        for row_id, channels in self.groups:
            row = PlotRow(row_id, channels, row_id == "overview")
            row.merge_requested.connect(self.merge)
            row.reorder_requested.connect(self.reorder)
            row.split_requested.connect(self.split)
            row.plot.viewport.y_range = self.viewport.y_range
            row.plot.viewport.x_range = self.viewport.x_range
            row.plot.set_live_mode(self.live_mode)
            row.plot.following_live = self.following_live
            row.plot.set_navigation_mode(self.navigation_mode)
            row.plot.navigation_started.connect(self._navigate)
            row.plot.resume_requested.connect(self.resume_live)
            row.plot.view_changed.connect(lambda plot=row.plot:self._sync_plot(plot))
            self.rows.append(row)
            self.box.addWidget(row)
        self.set_data(self.data, self.unit, self._source_id, _force=True)
        self.set_speed(self.speed_data,self.speed_unit,self._source_id)
        self.refresh_readouts()

    def set_data(self, data, unit="V", source_id=None, _force=False):
        changed = unit != self.unit or (source_id is not None and source_id != self._source_id)
        if changed:
            self.viewport.y_range = self.viewport.x_range = None
            self._source_id = source_id
            self._pending_data = None
            self._stats = []
            self.speed_y_range=None
        if self.live_mode and not self.following_live and not changed and not _force:
            self._pending_data = (data, unit, source_id)
            return
        self.data, self.unit = data, unit
        self._updating = True
        try:
            for row in self.rows:
                if row.is_speed:continue
                row.plot.following_live = True
                row.plot.set_series([(COLORS[ch], data[ch]) for ch in row.channels], y_label=unit, source_id=source_id)
                row.plot.following_live = self.following_live
                row.plot.viewport.y_range = self.viewport.y_range
                if self.live_mode and self.following_live: self.viewport.x_range = None
                row.plot.viewport.x_range = self.viewport.x_range
            vibration=next((r for r in self.rows if not r.is_speed),None)
            if vibration:self.viewport.domain=vibration.plot.viewport.domain
            for row in self.rows:
                if row.is_speed:
                    row.plot.viewport.domain=self.viewport.domain;row.plot.viewport.x_range=self.viewport.x_range
        finally: self._updating = False
        self.view_changed.emit()

    def set_speed(self,points,unit="rpm",source_id=None):
        if unit!=self.speed_unit:self.speed_y_range=None
        self.speed_data,self.speed_unit=points,unit
        self._updating=True
        try:
            for row in self.rows:
                if row.is_speed:
                    row.plot.set_series([("#7b6ab2",points)],y_label=unit,source_id=source_id)
                    row.plot.viewport.domain=self.viewport.domain;row.plot.viewport.x_range=self.viewport.x_range
                    row.plot.viewport.y_range=self.speed_y_range
                    row.readout.setText(unit)
        finally:self._updating=False

    def refresh_readouts(self):
        def fmt(value):return f"{value:.4g}" if finite(value) else "—"
        for row in self.rows:
            if row.is_speed:row.readout.setText(self.speed_unit);continue
            if len(row.channels)==1 and self._stats:
                stats=self._stats[row.channels[0]]
                row.readout.setText(f"RMS {fmt(stats.get('rms'))}   峰峰 {fmt(stats.get('peak_to_peak'))}  {self.unit}")
            else:row.readout.setText(self.unit)
            row.readout.setToolTip(self.stats_text())

    @property
    def paused(self): return self.live_mode and not self.following_live

    @property
    def y_label(self): return self.unit

    def _sync_plot(self, plot):
        if self._updating: return
        self.viewport.x_range = plot.viewport.x_range
        is_speed=any(row.is_speed and row.plot is plot for row in self.rows)
        if is_speed:self.speed_y_range=plot.viewport.y_range
        else:self.viewport.y_range = plot.viewport.y_range
        self._updating = True
        try:
            for row in self.rows:
                if row.plot is not plot:
                    row.plot.viewport.x_range = self.viewport.x_range
                    if not row.is_speed and not is_speed:row.plot.viewport.y_range = self.viewport.y_range
                    row.plot.update()
        finally: self._updating = False
        self.view_changed.emit()

    def _navigate(self):
        self.navigation_started.emit()
        self.pause_live()

    def set_live_mode(self, enabled):
        self.live_mode, self.following_live = bool(enabled), True
        self._pending_data = None
        self.viewport.x_range = None
        for row in self.rows: row.plot.set_live_mode(enabled)
        self.view_changed.emit()

    def pause_live(self):
        if self.live_mode:
            self.following_live = False
            for row in self.rows: row.plot.pause_live()
            self.view_changed.emit()

    def resume_live(self):
        self.following_live = True
        for row in self.rows: row.plot.following_live = True
        self.viewport.x_range = None
        pending, self._pending_data = self._pending_data, None
        if pending: self.set_data(*pending)
        else:
            for row in self.rows: row.plot.viewport.x_range = None; row.plot.update()
        self.resume_requested.emit(); self.view_changed.emit()

    def set_y_range(self, lower, upper):
        if not self.viewport.set_y(lower, upper):
            self.last_error = "幅值下限和上限须为有限数，且下限小于上限"
            return False
        self.last_error = ""
        for row in self.rows:
            if not row.is_speed:row.plot.set_y_range(lower, upper)
        self.view_changed.emit()
        return True

    def set_auto_y(self):
        self.viewport.y_range = None
        for row in self.rows:
            if not row.is_speed:row.plot.set_auto_y()
        self.view_changed.emit()

    def set_x_range(self, lower, upper):
        if not self.viewport.set_x(lower, upper): return False
        for row in self.rows: row.plot.viewport.x_range = self.viewport.x_range; row.plot.update()
        self.view_changed.emit()
        return True

    def zoom(self, factor, anchor=None):
        if not finite(factor) or factor <= 0 or (anchor is not None and not finite(anchor)): return False
        self._navigate()
        if not self.viewport.zoom(factor, anchor): return False
        return self.set_x_range(*self.viewport.x_range)

    def pan(self, fraction):
        if not finite(fraction): return False
        self._navigate()
        if not self.viewport.pan(fraction): return False
        return self.set_x_range(*self.viewport.x_range)

    def reset_view(self):
        self._navigate()
        self.viewport.x_range = None
        for row in self.rows: row.plot.viewport.x_range = None
        self.set_auto_y()

    def set_navigation_mode(self, mode):
        for row in self.rows: row.plot.set_navigation_mode(mode)
        self.navigation_mode = mode

    def set_authoritative_stats(self, stats, provenance="", unit=None, sample_count=None):
        """Accept raw-window/full-record measurements, never estimate from preview."""
        valid = provenance in ("raw_window", "complete_record") and (unit is None or unit == self.unit)
        self._stats = stats if valid and isinstance(stats, list) and len(stats) == 3 else []
        self._stats_scope = provenance if self._stats else ""
        self._stats_count = sample_count
        self.refresh_readouts()
        self.stats_changed.emit()

    def stats_text(self):
        if not self._stats: return "幅值统计：—（等待原始窗口或全记录统计）"
        scope = "原始预览窗口" if self._stats_scope == "raw_window" else "全记录"
        count = f" · 原始采样 {self._stats_count} 点" if isinstance(self._stats_count,int) else ""
        def fmt(value): return f"{value:.5g}" if finite(value) else "—"
        values = [f"CH{i+1}  峰峰值 {fmt(stats.get('peak_to_peak'))} / RMS {fmt(stats.get('rms'))} {self.unit}" for i, stats in enumerate(self._stats)]
        return scope+count+" · "+"； ".join(values)

    def merge(self, target, channel):
        if target in ("overview","speed") or channel not in (0,1,2) or target not in dict(self.groups): return
        groups = [(name, list(chs)) for name, chs in self.groups]
        target_channels = next(chs for name, chs in groups if name == target)
        if channel in target_channels: return
        for name, channels in groups:
            if name != "overview" and channel in channels:
                channels.remove(channel)
        target_channels.append(channel)
        self.groups = [(name, chs) for name, chs in groups if chs or name=="speed"]
        self.rebuild()

    def reorder(self, source, target):
        if source == target or source not in dict(self.groups) or target not in dict(self.groups): return
        moved = next(pair for pair in self.groups if pair[0] == source)
        self.groups.remove(moved)
        self.groups.insert(next(i for i, pair in enumerate(self.groups) if pair[0] == target), moved)
        self.rebuild()

    def split(self, name):
        if name in ("overview","speed") or name not in dict(self.groups):return
        index = next(i for i, pair in enumerate(self.groups) if pair[0] == name)
        _, channels = self.groups.pop(index)
        for channel in reversed(channels):
            self.groups.insert(index, ("group_"+uuid.uuid4().hex, [channel]))
        self.rebuild()
