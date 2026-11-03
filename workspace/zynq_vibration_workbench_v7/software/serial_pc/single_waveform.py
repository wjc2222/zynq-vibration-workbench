"""One measurement canvas with explicit channel selection and separate RPM units."""
from PySide6.QtCore import Qt, Signal, QSignalBlocker, QPointF, QRectF
from PySide6.QtGui import QPainter, QColor, QPen
from PySide6.QtWidgets import QWidget, QHBoxLayout, QVBoxLayout, QGridLayout, QLabel, QCheckBox, QPushButton, QFrame, QDialog
from plots import Plot, COLORS, finite


class ChannelCheck(QCheckBox):
    # The entire channel row is clickable, including the clear space after text.
    def hitButton(self,position):return self.rect().contains(position)

    def paintEvent(self,event):
        # Native theme accents may fill an unchecked indicator. Draw both
        # states explicitly, including a high-contrast check mark.
        painter=QPainter(self);painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        box=QRectF(1,(self.height()-20)/2,20,20)
        painter.setPen(QPen(QColor('#3569c8' if self.isChecked() else '#8c98a8'),1.3))
        painter.setBrush(QColor('#3569c8' if self.isChecked() else '#ffffff'))
        painter.drawRoundedRect(box,3,3)
        if self.isChecked():
            painter.setPen(QPen(QColor('white'),2.2,Qt.PenStyle.SolidLine,Qt.PenCapStyle.RoundCap,Qt.PenJoinStyle.RoundJoin))
            painter.drawLine(QPointF(5,box.top()+10),QPointF(9,box.top()+14))
            painter.drawLine(QPointF(9,box.top()+14),QPointF(17,box.top()+6))
        painter.setPen(QColor('#20252b' if self.isEnabled() else '#818995'))
        painter.drawText(QRectF(30,0,self.width()-30,self.height()),Qt.AlignmentFlag.AlignVCenter,self.text())
        if self.hasFocus():
            painter.setPen(QPen(QColor('#3569c8'),1,Qt.PenStyle.DotLine));painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.drawRoundedRect(QRectF(self.rect()).adjusted(.5,.5,-.5,-.5),3,3)


class SingleWaveformView(QWidget):
    navigation_started=Signal()
    view_changed=Signal()
    resume_requested=Signal()
    stats_changed=Signal()

    def __init__(self,parent=None,allow_speed=True,x_label="时间 / s"):
        super().__init__(parent)
        self.data=[[],[],[]];self.unit="V";self.speed_data=[];self.speed_unit="rpm"
        self._source_id=None;self._stats=[];self._stats_scope="";self._stats_count=None
        self.live_mode=False;self.following_live=True;self._pending_data=None
        self._updating=False;self._resuming=False;self._y_ranges={"vibration":None,"speed":None}
        self._selected=[0];self._remembered=[0];self._speed=False;self.x_label=x_label;self._show_amplitude_stats=allow_speed
        self._speed_mean=None;self._speed_scope="";self.speed_dialog=None
        self._sampling_context={}
        box=QHBoxLayout(self);box.setContentsMargins(0,0,0,0);box.setSpacing(0)
        self.plot=Plot();self.plot.setObjectName("singleMeasurementCanvas");box.addWidget(self.plot,1)
        self.panel=QFrame();self.panel.setObjectName("channelPanel");self.panel.setFixedWidth(276 if allow_speed else 180)
        panel=QVBoxLayout(self.panel);panel.setContentsMargins(14,12,0,8);panel.setSpacing(12)
        self.unit_label=QLabel();self.unit_label.setObjectName("rowTitle");panel.addWidget(self.unit_label)
        table=QGridLayout();table.setHorizontalSpacing(8);table.setVerticalSpacing(8)
        for col,title in enumerate(("通道", "RMS", "峰峰值")):
            label=QLabel(title);label.setAlignment(Qt.AlignmentFlag.AlignCenter);table.addWidget(label,0,col);label.setVisible(allow_speed)
        self.channel_boxes=[];self.channel_readouts=[]
        for ch,color in enumerate(COLORS):
            check=ChannelCheck(f"CH{ch+1}");check.setObjectName(f"showChannel{ch+1}")
            check.setStyleSheet("QCheckBox {font-size:16px;spacing:6px;}")
            check.setFixedHeight(44)
            check.setChecked(ch==0);check.toggled.connect(lambda checked,index=ch:self.select_channel(index,checked))
            channel=QWidget();row=QHBoxLayout(channel);row.setContentsMargins(0,0,0,0);row.setSpacing(6);row.addWidget(check)
            swatch=QFrame();swatch.setFixedSize(14,20);swatch.setStyleSheet(f"background:{color};border-radius:2px;");row.addWidget(swatch);table.addWidget(channel,ch+1,0);self.channel_boxes.append(check)
            values=[]
            for col in (1,2):
                value=QLabel("—");value.setObjectName("channelReadout");value.setAlignment(Qt.AlignmentFlag.AlignCenter);value.setMinimumSize(68,40)
                value.setStyleSheet("QLabel {font-size:16px;color:#20252b;background:#f8fafc;border:1px solid #aeb7c3;border-radius:4px;padding:0 3px;}")
                table.addWidget(value,ch+1,col);value.setVisible(allow_speed);values.append(value)
            self.channel_readouts.append(values)
        panel.addLayout(table)
        self.speed_box=ChannelCheck("转速波形");self.speed_box.setObjectName("showSpeed");self.speed_box.setFixedHeight(40)
        self.speed_box.setStyleSheet("QCheckBox {font-size:16px;spacing:10px;}")
        self.speed_box.toggled.connect(self.select_speed);panel.addWidget(self.speed_box);self.speed_box.setVisible(allow_speed)
        self.speed_button=QPushButton("转速数值");self.speed_button.setObjectName("speedValueButton")
        self.speed_button.clicked.connect(self.show_speed_value);panel.addWidget(self.speed_button);self.speed_button.setVisible(allow_speed)
        panel.addStretch()
        self.range_label=QLabel(self.panel);self.range_label.setObjectName("muted");self.range_label.hide()
        self.scope_label=QLabel();self.scope_label.setObjectName("muted");self.scope_label.setWordWrap(True);panel.addWidget(self.scope_label)
        box.addWidget(self.panel)
        self.plot.navigation_started.connect(self._navigation)
        self.plot.resume_requested.connect(self.resume_live)
        self.plot.view_changed.connect(self._plot_changed)
        self.refresh_readouts();self._refresh()

    @property
    def viewport(self):return self.plot.viewport
    @property
    def paused(self):return self.live_mode and not self.following_live
    @property
    def y_label(self):return self.speed_unit if self._speed else self.unit
    @property
    def last_error(self):return self.plot.last_error

    def select_channel(self,index,checked):
        if self._speed:
            self._speed=False;self._selected=[]
            with QSignalBlocker(self.speed_box):self.speed_box.setChecked(False)
        if checked and index not in self._selected:self._selected.append(index)
        if not checked and index in self._selected:self._selected.remove(index)
        self._selected.sort();self._remembered=list(self._selected)
        with QSignalBlocker(self.channel_boxes[index]):self.channel_boxes[index].setChecked(checked)
        self._refresh()

    def select_speed(self,checked):
        self._speed=checked
        if checked:self._remembered=list(self._selected);self._selected=[]
        else:self._selected=list(self._remembered)
        with QSignalBlocker(self.speed_box):self.speed_box.setChecked(checked)
        for index,check in enumerate(self.channel_boxes):
            with QSignalBlocker(check):check.setChecked(index in self._selected)
        self._refresh()

    def _refresh(self):
        self._updating=True
        try:
            x_range=self.viewport.x_range
            self.plot.following_live=True
            series=[("#7b6ab2",self.speed_data)] if self._speed else [(COLORS[i],self.data[i]) for i in self._selected]
            self.plot.empty_message=("暂无有效测速数据" if self._speed else "请在右侧选择通道" if not self._selected else "等待数据")
            self.plot.zero_centered_y=self._show_amplitude_stats and not self._speed
            self.plot.set_series(series,self.x_label,self.y_label,self._source_id)
            all_times=[p[0] for values in self.data for p in values if len(p)==2 and finite(p[0])]
            if all_times and max(all_times)>min(all_times):self.viewport.domain=(min(all_times),max(all_times))
            if self._sampling_context and not self._speed:
                context=self._sampling_context
                intervals=[(COLORS[i],context.get('intervals',[[],[],[]])[i]) for i in self._selected]
                provider=context.get('provider')
                adapter=(lambda a,b,n:provider(a,b,n,tuple(self._selected))) if provider else None
                self.plot.set_sampling_context(intervals,context.get('domain'),context.get('detail'),adapter,context.get('summary',False))
            else:self.plot.set_sampling_context()
            self.viewport.x_range=x_range
            self.viewport.y_range=self._y_ranges["speed" if self._speed else "vibration"]
            self.plot.following_live=self.following_live
            self.plot.update()
        finally:self._updating=False
        self.refresh_readouts();self.view_changed.emit()

    def set_data(self,data,unit="V",source_id=None):
        changed=unit!=self.unit or source_id is not None and source_id!=self._source_id
        if changed:
            self._sampling_context={}
            self._source_id=source_id;self._stats=[];self._stats_scope=""
            self._y_ranges={"vibration":None,"speed":None};self.viewport.x_range=None;self._pending_data=None
        if self.paused and not changed:self._pending_data=(data,unit,source_id);return
        self.data,self.unit=data,unit
        if self.live_mode and self.following_live:self.viewport.x_range=None
        self._refresh()

    def set_sampling_context(self,**context):
        self._sampling_context=context;self._refresh()

    def set_speed(self,points,unit="rpm",source_id=None):
        if unit!=self.speed_unit:self._y_ranges["speed"]=None
        self.speed_data,self.speed_unit=points,unit
        if self._speed:self._refresh()

    def set_speed_summary(self,mean,scope):
        self._speed_mean=mean if finite(mean) and mean>=0 else None;self._speed_scope=scope
        self._update_speed_dialog()

    def show_speed_value(self):
        if self.speed_dialog is None:
            self.speed_dialog=QDialog(self);self.speed_dialog.setWindowTitle("转速数值");self.speed_dialog.setMinimumSize(300,180)
            box=QVBoxLayout(self.speed_dialog);box.setContentsMargins(28,24,28,24)
            self.speed_value=QLabel();self.speed_value.setStyleSheet("font-size:36px;font-weight:600;");box.addWidget(self.speed_value)
            self.speed_description=QLabel();self.speed_description.setWordWrap(True);box.addWidget(self.speed_description)
        self._update_speed_dialog();self.speed_dialog.show();self.speed_dialog.raise_()

    def _update_speed_dialog(self):
        if self.speed_dialog is None:return
        value=self._speed_mean
        self.speed_value.setText(f"{value:.1f} rpm" if value is not None and self.speed_unit=="rpm" else "— rpm")
        note="PPR 未知，不能换算转速" if self.speed_unit!="rpm" else "无有效测速数据" if value is None else self._speed_scope
        self.speed_description.setText(note+(" · 显示已暂停" if self.paused else ""))

    def set_authoritative_stats(self,stats,provenance="",unit=None,sample_count=None):
        valid=provenance in ("raw_window","complete_record") and (unit is None or unit==self.unit)
        self._stats=stats if valid and isinstance(stats,list) and len(stats)==3 else []
        self._stats_scope=provenance if self._stats else "";self._stats_count=sample_count
        self.refresh_readouts();self.stats_changed.emit()

    def refresh_readouts(self):
        def fmt(value):return f"{value:.3g}" if finite(value) else "—"
        for index,labels in enumerate(self.channel_readouts):
            stats=self._stats[index] if self._stats and not self._speed else {}
            for label,key in zip(labels,("rms","peak_to_peak")):
                value=stats.get(key);label.setText(fmt(value));label.setToolTip(f"{value:.8g} {self.unit}" if finite(value) else "暂无有效数据")
            self.channel_boxes[index].setToolTip("显示或隐藏此通道；数值为振动原始数据统计")
        bounds=self.viewport.x_range or self.viewport.domain
        self.unit_label.setText(("转速 / " if self.speed_unit=="rpm" else "脉冲频率 / ")+self.speed_unit if self._speed else "幅值 / "+self.unit)
        if not self._show_amplitude_stats:self.unit_label.setText("显示通道")
        self.range_label.setText(f"{bounds[0]:.5g}–{bounds[1]:.5g} "+("Hz" if self.x_label.startswith('频率') else "s"))
        scope="原始窗口统计" if self._stats_scope=="raw_window" else "全记录统计" if self._stats_scope=="complete_record" else ""
        self.scope_label.setText("显示已暂停" if self.paused else scope)
        self.scope_label.setVisible(self.paused)
        self.panel.setToolTip(scope)
        self.scope_label.setToolTip("拖动或缩放只影响画面，不修改原始数据；右键可回到最新" if self.live_mode else "统计取自完整原始记录，缩放不改变统计范围")
        self._update_speed_dialog()

    def _plot_changed(self):
        if self._updating:return
        self._y_ranges["speed" if self._speed else "vibration"]=self.viewport.y_range
        self.refresh_readouts();self.view_changed.emit()

    def _navigation(self):
        self.pause_live();self.navigation_started.emit()

    def set_live_mode(self,enabled):
        self.live_mode=bool(enabled);self.following_live=True;self._pending_data=None
        self.plot.set_live_mode(enabled);self.refresh_readouts()

    def pause_live(self):
        if self.live_mode:self.following_live=False;self.plot.pause_live();self.refresh_readouts()

    def resume_live(self):
        if self._resuming:return
        self._resuming=True
        try:
            self.following_live=True
            pending,self._pending_data=self._pending_data,None
            if pending:self.set_data(*pending)
            self.plot.resume_live();self.resume_requested.emit();self.refresh_readouts()
        finally:self._resuming=False

    def zoom(self,factor,anchor=None):return self.plot.zoom(factor,anchor)
    def pan(self,fraction):return self.plot.pan(fraction)
    def set_y_range(self,lower,upper):return self.plot.set_y_range(lower,upper)
    def set_auto_y(self):self.plot.set_auto_y()
    def reset_view(self):self.plot.reset_view()
