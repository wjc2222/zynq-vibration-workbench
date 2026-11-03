"""Windows serial workbench. Run with Python+PySide6 or the bundled executable."""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
import struct
from pathlib import Path

from PySide6.QtCore import Qt, QProcess, QTimer, QStandardPaths, QSignalBlocker
from PySide6.QtGui import QFont,QFontDatabase
from PySide6.QtWidgets import (QApplication, QCheckBox, QComboBox, QDialog, QDialogButtonBox,
    QFileDialog, QFormLayout, QGridLayout, QHBoxLayout, QHeaderView, QLabel, QLineEdit, QMainWindow,
    QMessageBox, QProgressBar, QPushButton, QScrollArea, QSpinBox, QTableWidget,
    QTableWidgetItem, QTabWidget, QVBoxLayout, QWidget)

from plots import finite, COLORS
from local_waveform import LocalWaveform, capture_file_identity
from preview_payload import decode_detail, decode_envelope, decode_spectrum
from single_waveform import SingleWaveformView
from protocol import ProtocolError, RecordDownload
from session import BoardSession

STYLE = """
QMainWindow,QDialog {background:#fff;color:#20252b;}
QWidget {font-family:'Microsoft YaHei UI','Segoe UI';font-size:16px;color:#20252b;}
QPushButton {background:#fff;border:1px solid #919ba8;border-radius:6px;padding:9px 16px;min-height:24px;font-weight:500;}
QPushButton:hover {background:#eef3fa;border-color:#3569c8;}
QPushButton:pressed {background:#e4ecf7;}
QPushButton:disabled {color:#818995;border-color:#c5cbd3;background:#f3f4f6;}
QPushButton#primary {background:#3569c8;color:white;border-color:#3569c8;}
QPushButton#primary:hover {background:#285ab2;}
QPushButton#primary:disabled {background:#bbc9e2;border-color:#bbc9e2;}
QPushButton#quietButton {background:#fff;border:1px solid #919ba8;color:#303842;}
QComboBox,QSpinBox,QLineEdit {background:#fff;border:1px solid #919ba8;border-radius:6px;padding:8px;min-height:26px;}
QTabWidget::pane {border:1px solid #c5cbd3;background:white;}
QTabBar::tab {padding:10px 28px;background:#fff;color:#303842;border:1px solid #c5cbd3;margin-right:6px;border-top-left-radius:6px;border-top-right-radius:6px;}
QTabBar::tab:selected {color:#1f55a9;background:#edf3ff;border:1px solid #3569c8;font-weight:600;}
QScrollArea {border:0;background:white;}
QFrame#plotRow {background:white;border-bottom:1px solid #e5e9ef;}
QLabel#rowTitle {font-weight:600;}
QLabel#title {font-size:22px;font-weight:600;}
QLabel#muted {color:#56616e;font-size:15px;}
QLabel#notice {color:#915e16;background:#fff7e8;padding:8px;border-radius:4px;}
QProgressBar {border:1px solid #dce3ed;border-radius:3px;text-align:center;min-height:18px;background:white;}
QProgressBar::chunk {background:#c7d8f4;}
QTableWidget {background:#fff;border:1px solid #e0e5ed;gridline-color:#eef1f5;selection-background-color:#e9f0fc;selection-color:#20252b;}
QHeaderView::section {background:#f4f6fa;border:0;padding:9px;font-weight:500;}
QScrollBar:vertical {background:#f4f6f9;width:12px;}
QScrollBar::handle:vertical {background:#bec9d8;border-radius:5px;min-height:30px;}
QFrame#channelPanel {background:#fff;border-left:1px solid #d5dbe2;}
QLabel#channelReadout {font-size:16px;color:#303842;}
QCheckBox::indicator {width:20px;height:20px;}
"""

STATES = {"preparing":"准备采集", "running":"正在采集", "stopping":"正在停止并保存", "saving":"正在写入 SD",
          "completed":"定时采集完成", "stopped":"已停止采集", "failed":"采集异常", "cancelled":"已取消采集"}


def configure_application(application):
    # Use fonts installed on the recipient's Windows system. Never redistribute
    # proprietary font files inside the application or its firmware bundle.
    font_directory=Path(os.environ.get("WINDIR",r"C:\Windows"))/"Fonts"
    for name in ("segoeui.ttf","seguisb.ttf","msyh.ttc","msyhbd.ttc"):
        path=font_directory/name
        if path.is_file():QFontDatabase.addApplicationFont(str(path))
    application.setStyle("Fusion")
    font=QFont("Microsoft YaHei UI");font.setPixelSize(16);application.setFont(font)
    application.setStyleSheet(STYLE)


def text_number(value, suffix=""):
    return f"{value:.4g}{suffix}" if finite(value) else "—"


def unit_name(unit):
    return {"g_nominal_unverified":"名义 g", "g_nominal_uncalibrated":"名义 g", "g":"g", "V":"V"}.get(unit, unit)


def display_conversion(result):
    """Uncalibrated electrical input stays in volts in live and file views."""
    config=result.get('configuration',{})
    if result.get('amplitude_unit')=='g' and config.get('calibration_confirmed') is True:return 'g',1.
    if result.get('amplitude_unit')=='V':return 'V',1.
    scale=config.get('sensor_sensitivity_v_per_g',.5)*config.get('analog_gain',1.)
    if not finite(scale) or scale==0:raise ProtocolError('幅值换算参数无效')
    return 'V',scale


def display_stats(stats,factor):
    return [dict(item,rms=item['rms']*abs(factor),peak_to_peak=item['peak_to_peak']*abs(factor)) for item in stats]


def sample_count_error(raw, rate):
    if not raw or not raw.isascii() or not raw.isdigit() or int(raw) <= 0:
        return "每通道采样点数须为正整数。"
    count = int(raw)
    if rate not in (5000, 10000, 25000, 50000) or not rate <= count <= rate * 86400 or count % rate:
        return f"点数须为 {rate:,} 的整数倍，对应 1–86,400 秒；不自动取整。"
    return ""


def capture_plan_error(plan):
    mode = plan.get("mode")
    fields = {"mode", "sample_rate_hz", "pulses_per_rev"}
    if mode == "timed": fields.add("duration_seconds")
    if mode not in ("timed", "continuous") or set(plan) != fields:
        return "采集参数字段无效，请重新设置。"
    rate, duration, ppr = plan["sample_rate_hz"], plan.get("duration_seconds", 1), plan["pulses_per_rev"]
    if any(type(value) is not int for value in (rate, duration, ppr)):
        return "采集参数须为整数，请重新设置。"
    if rate not in (5000, 10000, 25000, 50000) or not 1 <= duration <= 86400 or not 1 <= ppr <= 4096:
        return "采集参数超出设备支持范围，请重新设置。"
    return sample_count_error(str(rate * duration), rate)


class CaptureDialog(QDialog):
    def __init__(self, plan, parent=None):
        super().__init__(parent)
        self.setWindowTitle("采集参数")
        self.setObjectName("captureParameterDialog")
        self.setMinimumWidth(620)
        self.acquisition_active = False
        box = QVBoxLayout(self); box.setContentsMargins(18, 18, 18, 18); box.setSpacing(16)
        self.mode = QComboBox(); self.mode.setObjectName("captureMode")
        self.mode.addItem("定时采集", "timed"); self.mode.addItem("连续采集 · 手动停止", "continuous")
        self.mode.setCurrentIndex(self.mode.findData(plan.get("mode", "timed")))
        box.addWidget(QLabel("采集方式")); box.addWidget(self.mode)
        form = QGridLayout(); self.parameter_grid=form; form.setHorizontalSpacing(16); form.setVerticalSpacing(8)
        self.rate = QComboBox()
        self.rate.setObjectName("captureRate")
        for rate in (5000,10000,25000,50000): self.rate.addItem(f"{rate // 1000} kHz", rate)
        self.rate.setCurrentIndex(self.rate.findData(plan["sample_rate_hz"]))
        self.duration = QSpinBox(); self.duration.setRange(1,86400); self.duration.setSuffix(" 秒"); self.duration.setValue(plan.get("duration_seconds",10))
        self.duration.setObjectName("captureDuration")
        self.ppr = QSpinBox(); self.ppr.setRange(1,4096); self.ppr.setValue(plan["pulses_per_rev"])
        self.ppr.setObjectName("capturePpr")
        self.samples = QLineEdit(str(plan["sample_rate_hz"] * plan.get("duration_seconds",10))); self.samples.setObjectName("captureSamples")
        self.samples.setMaxLength(28); self.samples.setInputMethodHints(Qt.InputMethodHint.ImhDigitsOnly)
        self.samples.setToolTip("每通道点数 = 采样率 × 整数秒数；三路数值总数为其 3 倍。采集点数与离线分析的 FFT 点数分别设置。")
        self.timed_labels = []
        for index, (label, field) in enumerate((("采样率", self.rate), ("每通道采样点数", self.samples), ("采集时长", self.duration), ("每转脉冲 PPR", self.ppr))):
            row, column = (index // 2) * 2, index % 2
            title = QLabel(label)
            if field is self.ppr:self.ppr_title=title
            if field in (self.samples, self.duration): self.timed_labels.append(title)
            form.addWidget(title, row, column); form.addWidget(field, row + 1, column); form.setColumnStretch(column, 1)
        box.addLayout(form)
        self.error = QLabel(); self.error.setObjectName("captureParameterError"); self.error.setWordWrap(True)
        self.error.setStyleSheet("font-size:16px;color:#9a4b32;"); box.addWidget(self.error)
        self.buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        self.apply_button = self.buttons.button(QDialogButtonBox.StandardButton.Ok); self.apply_button.setText("应用"); self.apply_button.setObjectName("captureApply")
        self.buttons.button(QDialogButtonBox.StandardButton.Cancel).setText("取消")
        self.buttons.accepted.connect(self.accept); self.buttons.rejected.connect(self.reject); box.addWidget(self.buttons)
        self.rate.currentIndexChanged.connect(self.samples_from_duration); self.duration.valueChanged.connect(self.samples_from_duration)
        self.samples.textChanged.connect(self.duration_from_samples)
        self.mode.currentIndexChanged.connect(self.mode_changed)
        self.mode_changed()
        self.update_parameter_state()

    def mode_changed(self):
        timed=self.mode.currentData()=="timed"
        self.parameter_grid.removeWidget(self.ppr_title);self.parameter_grid.removeWidget(self.ppr)
        self.parameter_grid.addWidget(self.ppr_title,2 if timed else 0,1)
        self.parameter_grid.addWidget(self.ppr,3 if timed else 1,1)
        self.update_parameter_state();self.adjustSize()

    def samples_from_duration(self):
        with QSignalBlocker(self.samples):
            self.samples.setText(str(self.rate.currentData() * self.duration.value()))
        self.update_parameter_state()

    def duration_from_samples(self):
        if not sample_count_error(self.samples.text(), self.rate.currentData()):
            with QSignalBlocker(self.duration):
                self.duration.setValue(int(self.samples.text()) // self.rate.currentData())
        self.update_parameter_state()

    def update_parameter_state(self):
        timed = self.mode.currentData() == "timed"
        for field in (self.samples, self.duration, *self.timed_labels): field.setVisible(timed)
        reason = sample_count_error(self.samples.text(), self.rate.currentData()) if timed else ""
        self.error.setText(reason); self.error.setVisible(bool(reason)); self.setProperty("captureParametersValid", not reason)
        self.apply_button.setEnabled(not reason and not self.acquisition_active)

    def set_acquisition_active(self, active):
        self.acquisition_active = active
        for field in (self.mode, self.rate, self.samples, self.duration, self.ppr): field.setEnabled(not active)
        self.update_parameter_state()

    def accept(self):
        if self.acquisition_active or (self.mode.currentData() == "timed" and sample_count_error(self.samples.text(), self.rate.currentData())):
            self.update_parameter_state(); return
        super().accept()

    def plan(self):
        reason = sample_count_error(self.samples.text(), self.rate.currentData()) if self.mode.currentData() == "timed" else ""
        if reason or self.acquisition_active: raise ValueError(reason or "采集中不能更改参数。")
        plan = dict(mode=self.mode.currentData(), sample_rate_hz=self.rate.currentData(), pulses_per_rev=self.ppr.value())
        if plan["mode"] == "timed": plan["duration_seconds"] = self.duration.value()
        reason = capture_plan_error(plan)
        if reason: raise ValueError(reason)
        return plan


class AnalysisDialog(QDialog):
    def __init__(self, config, parent=None):
        super().__init__(parent)
        self.setWindowTitle("离线分析参数")
        box = QVBoxLayout(self); form = QFormLayout(); form.setVerticalSpacing(12)
        self.ppr = QSpinBox(); self.ppr.setRange(1,4096); self.ppr.setValue(config["ppr"])
        self.fft = QComboBox()
        for n in (4096,8192,16384,32768,65536,131072,262144): self.fft.addItem(str(n), n)
        self.fft.setCurrentIndex(self.fft.findData(config["fft"]))
        self.maximum = QSpinBox(); self.maximum.setRange(10,25000); self.maximum.setValue(config["max_hz"]); self.maximum.setSuffix(" Hz")
        form.addRow("PPR（文件未知时使用）", self.ppr); form.addRow("FFT 点数", self.fft); form.addRow("频谱上限", self.maximum)
        box.addLayout(form)
        note = QLabel("离线分析使用同一套 C++ 分析核心。当前尚无经过实测验证的故障分类模型。")
        note.setWordWrap(True); note.setObjectName("muted"); box.addWidget(note)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self.accept); buttons.rejected.connect(self.reject); box.addWidget(buttons)


class Window(QMainWindow):
    def __init__(self, analyzer=None):
        super().__init__()
        self.setWindowTitle("振动测量 · 串口工作台")
        self.resize(1280,760); self.setMinimumSize(1024,600)
        self.analyzer = Path(analyzer) if analyzer else Path(sys.executable if getattr(sys,"frozen",False) else __file__).parent / "vibration_analyze.exe"
        self.plan = dict(mode="timed",sample_rate_hz=50000,duration_seconds=10,pulses_per_rev=1)
        self.config = dict(ppr=1,fft=65536,max_hz=2000)
        docs = QStandardPaths.writableLocation(QStandardPaths.StandardLocation.DocumentsLocation)
        self.save_directory = Path(docs or str(Path.home())) / "振动采集"
        self.session = BoardSession(self)
        self.session.state_changed.connect(self.connection_state)
        self.session.message.connect(self.on_message)
        self.session.failed.connect(self.connection_failed)
        self.session.traffic.connect(lambda rx,tx: self.traffic.setText(f"接收 {rx/1024:.0f} KiB"))
        self.active = False; self.current_job = ""; self.record_id = ""; self.records = []
        self.result = None; self.last_preview = None; self.paused = False; self.preview_failed = False
        self.local_waveform=None;self.analysis_identity=None
        self.download = None; self.analysis_process = None; self.local_path = None; self.analysis_output = bytearray()
        self.result_origin = ""; self.available = False; self.generation = ""; self.preview_revision = 0
        self.remote_busy = False; self.closing_window = False
        self.capture_dialog = None
        self._resuming_display = False
        self._notice_message = ""; self._display_warning = ""
        self.download_pending=False; self.download_request_id=None
        self.records_request_id=None;self.remote_analysis_request_id=None;self.delete_request_id=None
        self.stop_pending=False; self.record_dialog=None; self.record_cursors=[""]; self.record_page=0
        center = QWidget(); self.setCentralWidget(center); box = QVBoxLayout(center); box.setContentsMargins(20,16,20,12); box.setSpacing(12)
        header = QHBoxLayout()
        title=QLabel("振动测量"); title.setObjectName("title"); header.addWidget(title)
        header.addStretch()
        self.ports=QComboBox(); self.ports.setFixedWidth(160); self.ports.setToolTip("开发板串口"); header.addWidget(self.ports)
        self.baud=QComboBox()
        for rate in (115200,460800,921600):self.baud.addItem(str(rate),rate)
        self.baud.setToolTip("高速模式需USB串口与线缆稳定；连接握手从115200开始，退出后恢复")
        header.addWidget(self.baud)
        self.refresh_btn=QPushButton("刷新"); self.refresh_btn.clicked.connect(self.refresh_ports); header.addWidget(self.refresh_btn)
        self.connect_btn=QPushButton("连接设备"); self.connect_btn.clicked.connect(self.toggle_connection); header.addWidget(self.connect_btn)
        box.addLayout(header)
        actions=QHBoxLayout(); actions.setSpacing(10)
        self.start_btn=QPushButton("开始采集"); self.start_btn.setObjectName("primary"); self.start_btn.clicked.connect(self.start_capture); actions.addWidget(self.start_btn,1)
        self.cancel_btn=QPushButton("停止采集"); self.cancel_btn.clicked.connect(self.stop_capture); actions.addWidget(self.cancel_btn,1)
        self.download_btn=QPushButton("导出到电脑"); self.download_btn.clicked.connect(self.begin_download); actions.addWidget(self.download_btn,1)
        self.open_btn=QPushButton("打开数据"); self.open_btn.clicked.connect(self.open_file); actions.addWidget(self.open_btn,1)
        self.params_btn=QPushButton("采集参数"); self.params_btn.clicked.connect(self.parameters); actions.addWidget(self.params_btn,1)
        self.records_btn=QPushButton("SD 记录"); self.records_btn.clicked.connect(self.request_records); actions.addWidget(self.records_btn,1)
        box.addLayout(actions)
        status=QHBoxLayout(); self.status=QLabel("未连接 · 可打开已有 BIN 文件分析"); status.addWidget(self.status); status.addStretch()
        self.plan_label=QLabel(); self.plan_label.setObjectName("muted"); status.addWidget(self.plan_label); box.addLayout(status)
        self.notice=QLabel(); self.notice.setObjectName("notice"); self.notice.setWordWrap(True); self.notice.hide(); box.addWidget(self.notice)
        self.progress=QProgressBar(); self.progress.setRange(0,1000); self.progress.hide(); box.addWidget(self.progress)
        self.tabs=QTabWidget(); box.addWidget(self.tabs,1)
        timepage=QWidget(); timebox=QVBoxLayout(timepage); timebox.setContentsMargins(12,10,12,4)
        toolbar=QHBoxLayout(); self.pause_btn=QPushButton("暂停显示"); self.pause_btn.clicked.connect(self.pause); toolbar.addWidget(self.pause_btn)
        toolbar.addStretch()
        self.window_label=QLabel(); self.window_label.setObjectName("muted"); toolbar.addWidget(self.window_label); timebox.addLayout(toolbar)
        self.waveforms=SingleWaveformView();timebox.addWidget(self.waveforms,1)
        self.waveforms.view_changed.connect(self.update_waveform_caption)
        self.tabs.addTab(timepage,"时域")
        spectrum_page=QWidget(); spectrum_box=QVBoxLayout(spectrum_page);spectrum_box.setContentsMargins(12,10,12,4)
        spectrum_header=QHBoxLayout();self.spectrum_label=QLabel("等待频谱");self.spectrum_label.setObjectName("muted");spectrum_header.addWidget(self.spectrum_label);spectrum_header.addStretch()
        self.analysis_btn=QPushButton("分析参数");self.analysis_btn.clicked.connect(self.analysis_parameters);spectrum_header.addWidget(self.analysis_btn);spectrum_box.addLayout(spectrum_header)
        self.spectrum_view=SingleWaveformView(allow_speed=False,x_label="频率 / Hz");spectrum_box.addWidget(self.spectrum_view,1)
        self.tabs.addTab(spectrum_page,"频谱")
        self.plot_targets=[self.waveforms,self.spectrum_view]
        for target in self.plot_targets:
            target.navigation_started.connect(self.freeze_display)
            target.resume_requested.connect(self.resume_display)
        detection=QWidget(); detection_box=QVBoxLayout(detection); detection_box.setSpacing(15)
        self.diagnosis_title=QLabel("等待数据"); self.diagnosis_title.setStyleSheet("font-size:22px;font-weight:600"); detection_box.addWidget(self.diagnosis_title)
        self.diagnosis_text=QLabel("采集过程中显示预览质量；采集完成后使用完整记录进行分析。"); self.diagnosis_text.setWordWrap(True); detection_box.addWidget(self.diagnosis_text)
        self.feature_table=QTableWidget(3,5); self.feature_table.setHorizontalHeaderLabels(["通道","RMS","峰峰值","峰值因子","峭度"]); self.feature_table.verticalHeader().hide(); self.feature_table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        self.feature_table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers); detection_box.addWidget(self.feature_table)
        self.limitations=QLabel(); self.limitations.setWordWrap(True); self.limitations.setObjectName("muted"); detection_box.addWidget(self.limitations); detection_box.addStretch()
        self.export_btn=QPushButton("导出分析");self.export_btn.clicked.connect(self.export_result);detection_box.addWidget(self.export_btn,alignment=Qt.AlignmentFlag.AlignRight)
        self.tabs.addTab(detection,"检测")
        footer=QHBoxLayout(); self.source_label=QLabel("来源 —"); self.source_label.setObjectName("muted"); footer.addWidget(self.source_label)
        self.traffic=QLabel(); self.traffic.setObjectName("muted"); footer.addWidget(self.traffic); footer.addStretch()
        box.addLayout(footer)
        self.set_display_live(False); self.refresh_ports(); self.update_controls()

    def show_notice(self, text):
        self._notice_message=text; self.refresh_notice()

    def refresh_notice(self):
        text="\n".join(part for part in (self._notice_message,self._display_warning) if part)
        self.notice.setText(text); self.notice.setVisible(bool(text))

    def set_display_warning(self, text):
        self._display_warning=text; self.refresh_notice()

    def update_waveform_caption(self):
        context=self.waveforms._sampling_context
        legacy_preview=context.get('summary') and context.get('summary_source')=='legacy_preview'
        self.set_display_warning('当前预览未提供连续原始样点，不能显示为连续波形。请核对板端显示服务版本；可导出 BIN 检查原始数据。' if legacy_preview else '')
        if not context:return
        if self.waveforms._speed:
            self.window_label.setText('转速趋势 · rpm');return
        bounds=self.waveforms.viewport.x_range or self.waveforms.viewport.default_range or self.waveforms.viewport.domain
        span=bounds[1]-bounds[0]
        width=f'{span*1000:.2f} ms' if span<.2 else f'{span:.3f} s'
        if context.get('provider') and self.local_waveform:
            raw=span*self.local_waveform.sample_rate_hz<=2*max(64,self.waveforms.plot.width()-100)
            self.window_label.setText(('原始样点' if raw else '完整记录包络')+' · '+width)
        elif context.get('detail'):
            detail=context['detail'];raw=detail[0]<=bounds[0] and bounds[1]<=detail[1]
            self.window_label.setText(('连续原始样点' if raw else '窗口包络 · 右键恢复细节')+' · '+width)
        elif context.get('summary'):
            self.window_label.setText('预览极值摘要 · 缺少连续原始样点' if legacy_preview else '记录极值摘要 · 导出后查看原始样点')

    def update_controls(self):
        connected=self.session.connected
        busy=self.active or self.remote_busy or self.download_pending or self.download is not None or self.analysis_process is not None or self.delete_request_id is not None
        self.start_btn.setEnabled(connected and self.available and not busy and not capture_plan_error(self.plan))
        self.cancel_btn.setEnabled(self.active and bool(self.current_job) and connected and not self.stop_pending)
        self.cancel_btn.setText("正在停止" if self.stop_pending else "停止采集")
        self.params_btn.setEnabled(not busy); self.open_btn.setEnabled(not busy); self.analysis_btn.setEnabled(not busy)
        self.records_btn.setEnabled(connected and not busy and self.records_request_id is None)
        self.download_btn.setEnabled(connected and bool(self.record_id) and not self.active and not self.remote_busy and not self.download_pending and self.analysis_process is None and self.delete_request_id is None)
        self.download_btn.setText("停止传回" if self.download else "导出到电脑")
        self.download_btn.setToolTip("原始数据已保存在 SD 卡；将选中段的 BIN 与元数据复制到电脑")
        self.export_btn.setEnabled(self.result is not None)
        self.ports.setEnabled(self.session.phase=="closed"); self.refresh_btn.setEnabled(self.session.phase=="closed")
        self.baud.setEnabled(self.session.phase=="closed")
        if self.capture_dialog is not None: self.capture_dialog.set_acquisition_active(busy)
        self.connect_btn.setText("断开连接" if self.session.phase!="closed" else "连接设备")
        mode_text = "连续采集" if self.plan.get("mode") == "continuous" else f"{self.plan.get('duration_seconds',0)} s"
        self.plan_label.setText(f"{self.plan.get('sample_rate_hz',0)//1000} kHz · {mode_text} · SD 保存")

    def refresh_ports(self):
        current=self.ports.currentData(); self.ports.clear()
        for name,description in self.session.ports():
            self.ports.addItem(name,name)
            self.ports.setItemData(self.ports.count()-1,description,Qt.ItemDataRole.ToolTipRole)
        if current and self.ports.findData(current)>=0: self.ports.setCurrentIndex(self.ports.findData(current))
        else:
            for i in range(self.ports.count()):
                if "CP210" in (self.ports.itemData(i,Qt.ItemDataRole.ToolTipRole) or ""): self.ports.setCurrentIndex(i); break

    def toggle_connection(self):
        if self.session.phase!="closed": self.session.disconnect()
        elif self.ports.currentData(): self.session.open(self.ports.currentData(),self.baud.currentData())
        else: self.show_notice("未发现串口。请连接开发板 USB 串口，并检查 CP210x 驱动。")

    def connection_state(self, state):
        descriptions={"closed":"设备已断开","probing":"检查开发板 Linux","launching":"启动串口服务","handshake":"校验通信协议","ready":f"设备已连接 · {self.session.baud} bps","closing":"正在关闭串口服务"}
        self.status.setText(descriptions[state])
        if state=="closed":
            self.available=False
            self.set_display_live(False)
            if self.active or self.remote_busy or self.download_pending or self.download:
                self.active=False;self.remote_busy=False;self.preview_failed=True;self.result=None
                self.diagnosis_title.setText("连接已结束");self.diagnosis_text.setText("保留曲线为断开前数据，当前设备状态需重新连接确认。")
                if self.download:self.download.abort();self.download=None
                self.progress.hide()
            self.download_pending=False;self.download_request_id=None
            self.stop_pending=False
            self.records_request_id=None;self.remote_analysis_request_id=None
            self.delete_request_id=None
            if self.record_dialog is not None:self.record_dialog.close()
            if self.closing_window: QTimer.singleShot(0,self.close)
        self.update_controls()

    def connection_failed(self, message):
        self.set_display_live(False)
        self.active=False; self.stop_pending=False; self.remote_busy=False; self.preview_failed=True; self.result=None; self.record_id=""
        self.download_pending=False;self.download_request_id=None
        self.delete_request_id=None
        if self.record_dialog is not None:self.record_dialog.close()
        self.diagnosis_title.setText("连接中断"); self.diagnosis_text.setText("当前预览已停止，原有结果不再作为实时诊断。重新连接后可查看板端已完成记录。")
        if self.download:
            self.download.abort(); self.download=None
        self.progress.hide(); self.show_notice(message); self.update_controls()

    def send(self, command, **fields):
        try: return self.session.send(command,**fields)
        except Exception as exc: self.show_notice(str(exc)); return None

    def request_records(self, checked=False, cursor="", page=0):
        if self.records_request_id or self.active or self.remote_busy or self.delete_request_id:return
        self.record_page=page
        if not cursor and page == 0:self.record_cursors=[""]
        self.records_request_id=self.send("list_records",cursor=cursor,limit=100);self.update_controls()

    def parameters(self):
        if self.active or self.remote_busy or self.download_pending or self.download is not None or self.analysis_process is not None:return
        dialog=CaptureDialog(self.plan,self); self.capture_dialog=dialog
        try:
            if dialog.exec(): self.plan=dialog.plan(); self.update_controls()
        finally:self.capture_dialog=None

    def analysis_parameters(self):
        dialog=AnalysisDialog(self.config,self)
        if dialog.exec():
            self.config=dict(ppr=dialog.ppr.value(),fft=dialog.fft.currentData(),max_hz=dialog.maximum.value())
            if self.local_path and not self.active: self.analyze_local(self.local_path)

    def clear_result(self):
        if self.local_waveform is not None:self.local_waveform.close();self.local_waveform=None
        # Leaving a capture view retires its event ownership. A late automatic
        # segment result must not replace a selected record or a local CLI run.
        self.current_job="";self.result_origin=""
        self.result=None; self.record_id=""; self.last_preview=None; self.generation=""; self.preview_revision=0
        self.preview_failed=False; self.paused=False; self.pause_btn.setText("暂停显示")
        self.set_display_live(False)
        self.waveforms.set_data([[],[],[]],source_id="empty"); self.waveforms.set_authoritative_stats([])
        self.waveforms.set_speed([],source_id="empty")
        self.spectrum_view.set_data([[],[],[]],source_id="empty")
        self.waveforms.set_speed_summary(None,"");self.feature_table.clearContents()
        self.diagnosis_title.setText("等待数据"); self.diagnosis_text.setText(""); self.limitations.setText("")
        self.spectrum_label.setText("等待频谱"); self.window_label.setText(""); self.source_label.setText("来源 —")

    def start_capture(self):
        if not self.session.connected or not self.available or self.active or self.remote_busy or self.download_pending or self.download is not None or self.analysis_process is not None or self.delete_request_id is not None: return
        reason=capture_plan_error(self.plan)
        if reason:self.show_notice(reason);self.update_controls();return
        self.clear_result(); self.current_job=""; self.local_path=None; self.active=True; self.stop_pending=False
        self.set_display_live(True)
        self.progress.setRange(0,0 if self.plan["mode"]=="continuous" else 1000)
        self.progress.setValue(0); self.progress.show(); self.show_notice("")
        self.status.setText("正在提交采集参数")
        if not self.send("start",plan=self.plan): self.active=False; self.set_display_live(False)
        self.update_controls()

    def stop_capture(self):
        if not self.active or not self.current_job or self.stop_pending:return
        if self.send("stop",job_id=self.current_job):
            self.stop_pending=True;self.status.setText("正在停止并保存最后一段到 SD")
        self.update_controls()

    def on_message(self, message):
        try:
            if "event" in message:
                event,data=message["event"],message.get("data",{})
                if event=="service_ready": self.capabilities(data.get("servicecaps",{}))
                elif event=="job_status": self.job_status(data)
                elif event=="session_done": self.session_done(data)
                elif event=="preview": self.preview(data)
                elif event=="preview_rejected":
                    if data.get("job_id")==self.current_job:
                        self.preview_failed=bool(data.get("quarantined")); self.status.setText("预览校验异常 · 完整采集继续")
                        self.show_notice("预览暂停："+data.get("reason","校验失败")); self.diagnosis_title.setText("预览不可用")
                elif event=="analysis": self.complete_analysis(data)
                elif event=="analysis_failed":
                    if self.current_job and data.get("job_id")==self.current_job and self.result_origin!="local" and self.analysis_process is None:
                        self.remote_busy=False
                        self.result=None; self.status.setText("完整记录分析失败"); self.show_notice(data.get("message","分析失败")); self.diagnosis_title.setText("分析失败")
                elif event=="connection_lost": self.connection_failed(data.get("reason","板端采集服务中断"))
                elif event=="records_ready" and not data.get("ok",True): self.show_notice("板端记录恢复失败："+data.get("message",""))
            else:
                command,data=message.get("command"),message.get("data",{})
                if command in ("download_begin","download_chunk") and (not self.download_request_id or message.get("id")!=self.download_request_id):return
                if command=="list_records":
                    if not self.records_request_id or message.get("id")!=self.records_request_id:return
                    self.records_request_id=None
                if command=="analyze_record":
                    if not self.remote_analysis_request_id or message.get("id")!=self.remote_analysis_request_id:return
                    self.remote_analysis_request_id=None
                if command=="delete_records":
                    if not self.delete_request_id or message.get("id")!=self.delete_request_id:return
                    self.delete_request_id=None
                if message.get("ok") is not True:
                    if command=="start": self.active=False; self.stop_pending=False; self.progress.hide(); self.set_display_live(False)
                    if command=="stop":self.stop_pending=False
                    if command=="analyze_record":self.remote_busy=False
                    if command in ("download_begin","download_chunk"):
                        self.send("download_end")
                        if self.download:self.download.abort();self.download=None
                        self.download_pending=False;self.download_request_id=None;self.progress.hide()
                    error=message.get("error",{}); self.show_notice(f"{error.get('code','ERROR')}：{error.get('message','操作失败')}")
                elif command=="hello":
                    self.capabilities(data.get("servicecaps",{}));self.preview_interval_ms=data.get("preview_interval_ms",1000)
                elif command=="start": self.current_job=data["job_id"]; self.status.setText("正在采集 · 原始数据写入 SD")
                elif command=="list_records": self.show_records(data.get("records",[]),data.get("next_cursor",""))
                elif command=="analyze_record": self.complete_analysis(data,selected=True)
                elif command=="download_begin": self.download_started(data)
                elif command=="download_chunk": self.download_chunk(data)
                elif command=="delete_records":self.records_deleted(data)
            self.update_controls()
        except Exception as exc:
            self.freeze_display()
            self.show_notice("数据处理失败："+str(exc))
            self.result=None; self.preview_failed=True; self.remote_busy=False; self.status.setText("数据校验失败 · 显示已冻结"); self.diagnosis_title.setText("数据无效"); self.update_controls()

    def capabilities(self, caps):
        self.available=caps.get("available") is True
        if self.available: self.show_notice("")
        elif caps: self.show_notice("采集设备不可用："+caps.get("reason","等待服务就绪"))
        self.update_controls()

    def job_status(self, data):
        job=data.get("job_id")
        if self.current_job and job!=self.current_job: return
        if not self.current_job and self.active: self.current_job=job
        if job!=self.current_job: return
        state=data.get("state"); text=STATES.get(state,state or "未知状态")
        total=int(data.get("total_records",data.get("requested_records",0)))
        received=int(data.get("records_received",data.get("records_completed",0)))
        saved=int(data.get("durable_records",0))
        rate=data.get("plan",{}).get("sample_rate_hz",self.plan.get("sample_rate_hz",0))
        if rate and state in ("running","stopping","saving"):
            text+=f" · {received/rate:.1f} s · SD 已保存 {saved/rate:.1f} s"
        self.status.setText(text)
        if total:
            self.progress.setRange(0,1000); self.progress.setValue(min(1000,int(1000*received/total)))
        if state in ("completed","stopped","failed","cancelled"):
            self.active=False; self.stop_pending=False; self.progress.hide()
            self.set_display_live(False)
            self.remote_busy=False
            self.preview_failed=True
            self.spectrum_label.setText("采集结束 · 最后预览窗口")
            if saved and rate:self.status.setText(text+f" · SD 已保存 {saved/rate:.1f} s")
            if state in ("failed","cancelled"):
                self.result=None; self.diagnosis_title.setText("采集异常结束")
                self.diagnosis_text.setText("SD 中已封存的分段保留；请查看记录和异常原因。当前曲线为结束前预览。")
                self.show_notice(data.get("error_message") or "采集未完整结束，请核对 SD 已保存记录。")

    def session_done(self, data):
        if data.get("job_id")!=self.current_job:return
        self.job_status(data)
        if data.get("session_complete") is False:
            self.show_notice(data.get("error_message") or "部分数据未完成保存；SD 中已封存记录保留。")

    def preview(self, data):
        if not self.active or self.preview_failed or data.get("job_id")!=self.current_job: return
        generation=str(data.get("generation",""))
        if not generation or (self.generation and self.generation!=generation): raise ProtocolError("预览代次发生变化")
        if type(data.get("synthetic")) is not bool or data.get("source") not in ("adc_unconfirmed","hardware_test","test_fixture"):
            raise ProtocolError("预览来源无效")
        revision=data.get("revision",0)
        if not finite(revision) or revision<self.preview_revision: raise ProtocolError("收到旧预览")
        self.generation=generation; self.preview_revision=revision; self.last_preview=data
        self.source_label.setText("来源：合成自检" if data["synthetic"] else "来源：ADC · 传感器与标定待确认")
        self.diagnosis_title.setText("实时质量监测")
        self.diagnosis_text.setText("当前显示经过校验的采集窗口。故障类别待完整记录与实测规则验证。")
        self.limitations.setText(f"预览缺口：{data.get('preview_dropped_records',0)} 条 · 原始采集单独保存，不以预览点数代替原始记录。")
        if not self.paused: self.render_preview(data)

    def render_preview(self,data):
        detail=data.get('waveform_detail');overview=data.get('waveform_envelope')
        if detail is None and data.get('voltage_representation')=='use_waveform_detail':
            raise ProtocolError('新版预览标记缺少连续原始样点')
        raw_context=None
        if detail is not None:
            voltage,detail_range,rate=decode_detail(detail)
            if overview is None and data.get('overview_omitted_due_to_budget') is True:intervals,domain=[[],[],[]],detail_range
            else:intervals,domain=decode_envelope(overview)
            if rate!=data.get('sample_rate_hz') or not domain[0]<=detail_range[0]<detail_range[1]<=domain[1]:raise ProtocolError('原始预览时间范围不一致')
            raw_context=dict(intervals=intervals,domain=domain,detail=detail_range)
        else:voltage=data.get("voltage",[])
        if len(voltage)!=3 or any(len(points)>10000 for points in voltage): raise ProtocolError("波形通道无效")
        source_id=f"live:{self.current_job}:{data.get('generation','')}"
        self.waveforms.set_data(voltage,"V",source_id=source_id)
        self.waveforms.set_sampling_context(**raw_context) if raw_context else self.waveforms.set_sampling_context(summary=True,summary_source='legacy_preview')
        stats=data.get("amplitude_stats",{})
        channels=stats.get("channels",[])
        valid_stats=(stats.get("scope")=="raw_window" and stats.get("unit")=="V"
            and stats.get("derived_before_display_decimation") is True
            and stats.get("input_revision")==data.get("revision")
            and stats.get("start_s")==data.get("start_s") and stats.get("end_s")==data.get("end_s")
            and isinstance(channels,list) and len(channels)==3
            and all(isinstance(item,dict) and item.get("channel")==i+1
                and type(item.get("sample_count")) is int and item["sample_count"]>=0
                and all(item.get(key) is None or (finite(item.get(key)) and item[key]>=0)
                        for key in ("rms","peak_to_peak")) for i,item in enumerate(channels)))
        count=channels[0]["sample_count"] if valid_stats and len({c["sample_count"] for c in channels})==1 else None
        self.waveforms.set_authoritative_stats(channels if valid_stats else [],"raw_window",unit="V",sample_count=count)
        self.update_waveform_caption()
        self.waveforms.set_speed(data.get("speed",[]),"rpm",source_id=source_id)
        self.waveforms.set_speed_summary(data.get("mean_rpm"),"当前窗口平均转速")
        spectrum=data.get("spectrum",{})
        if spectrum.get("available"):
            packed=bool(spectrum.get('channels') and spectrum['channels'][0].get('encoding'))
            points=decode_spectrum(spectrum) if packed else [list(zip(channel["frequency_hz"],channel["peak_amplitude"])) for channel in spectrum.get("channels",[])]
            self.spectrum_view.set_data(points,"V",source_id=source_id)
            if packed:self.spectrum_view.set_sampling_context(domain=(0,spectrum.get('max_hz',2000)))
            channel=spectrum['channels'][0]
            self.spectrum_label.setText(f"幅度谱 · FFT {channel.get('fft_size','—')} · 分辨率 {channel.get('bin_width_hz',0):.2f} Hz" if packed else "旧版频谱摘要 · 需部署新版显示固件")
        else:
            self.spectrum_view.set_data([[],[],[]],"V",source_id=source_id)
            self.spectrum_label.setText("当前窗口频谱不可用，等待连续采样")

    def pause(self):
        if self.paused:self.resume_display()
        else:self.freeze_display()

    def set_display_live(self, enabled):
        self.paused=False
        for target in self.plot_targets:target.set_live_mode(enabled)
        self.pause_btn.setText("暂停显示"); self.pause_btn.setEnabled(enabled)
        self.pause_btn.setVisible(enabled)

    def freeze_display(self):
        if not self.active:return
        self.paused=True
        for target in self.plot_targets:target.pause_live()
        self.pause_btn.setText("回到最新")

    def resume_display(self):
        if self._resuming_display or not self.active:return
        self._resuming_display=True
        try:
            self.paused=False
            for target in self.plot_targets:target.resume_live()
            if self.last_preview:self.render_preview(self.last_preview)
            self.pause_btn.setText("暂停显示")
        finally:self._resuming_display=False

    def complete_analysis(self,data,selected=False):
        if self.analysis_process is not None or self.result_origin=="local":return
        if not selected and data.get("job_id")!=self.current_job: return
        if data.get("scope")=="completed_segment":
            result=data.get("result",{})
            if result.get("schema")!="zynq_vibration.analysis.v1" or result.get("integrity")!="passed":raise ProtocolError("分段分析校验未通过")
            self.record_id=data["record_id"];self.result=result;self.result_origin="board"
            self.show_analysis_features(result,segment=True)
            self.auto_save_analysis();return
        if data.get("scope")!="complete_record": raise ProtocolError("不能把预览当完整分析")
        self.remote_busy=False
        if selected:self.current_job=""
        self.record_id=data["record_id"]; self.local_path=None
        self.result_origin="board"; self.render_result(data["result"])
        self.status.setText("所选记录分析完成 · 原始文件已保存在 SD")
        self.show_notice("")
        self.auto_save_analysis()

    def render_result(self,result):
        if result.get("schema")!="zynq_vibration.analysis.v1" or result.get("integrity")!="passed": raise ProtocolError("完整文件校验未通过")
        self.result=result; unit,factor=display_conversion(result)
        voltage_scale=1. if unit=='V' else 1./(result['configuration']['sensor_sensitivity_v_per_g']*result['configuration']['analog_gain'])
        self.set_display_live(False)
        source_id=f"record:{self.record_id or self.local_path or id(result)}"
        data=[[(p["time_s"],p["values"][ch]*factor) for p in result["waveform"]] for ch in range(3)]
        remote_context=None
        if result.get('display_detail') is not None:
            data,detail_range,_=decode_detail(result['display_detail'])
            intervals,domain=decode_envelope(result['display_envelope'])
            if not domain[0]<=detail_range[0]<detail_range[1]<=domain[1]:raise ProtocolError('记录细节与包络范围不一致')
            voltage_scale=1. if unit=='V' else 1./(result['configuration']['sensor_sensitivity_v_per_g']*result['configuration']['analog_gain'])
            if voltage_scale!=1.:
                data=[[(t,v*voltage_scale) for t,v in ch] for ch in data]
                intervals=[[(a,b,min(lo*voltage_scale,hi*voltage_scale),max(lo*voltage_scale,hi*voltage_scale)) for a,b,lo,hi in ch] for ch in intervals]
            remote_context=dict(intervals=intervals,domain=domain,detail=detail_range)
        self.waveforms.set_data(data,unit,source_id=source_id)
        if self.local_path is not None and self.result_origin=="local":
            if self.local_waveform is not None:self.local_waveform.close()
            self.local_waveform=LocalWaveform(self.local_path,result,expected_identity=self.analysis_identity)
            reader=self.local_waveform
            def read_view(start,end,pixels,selected):
                window=reader.window(start,end,pixels,channels=selected,unit='V' if unit=='V' else 'analysis')
                series=[(COLORS[c.channel],[(p.time_s,p.value) for p in c.points]) for c in window.series if c.points]
                intervals=[(COLORS[c.channel],[(b.start_time_s,b.end_time_s,b.minimum,b.maximum) for b in c.intervals]) for c in window.series if c.intervals]
                return series,intervals
            self.waveforms.set_sampling_context(domain=reader.domain,provider=read_view)
        elif remote_context:self.waveforms.set_sampling_context(**remote_context)
        else:
            # A legacy summary is not a sequence of adjacent raw samples.
            self.waveforms.set_sampling_context(summary=True,summary_source='record_summary')
        self.waveforms.set_authoritative_stats(display_stats(result["time_stats"],factor),"complete_record",unit=unit)
        self.window_label.setText(f"完整记录 · {result.get('duration_s',0):.3f} s · 缩放查看原始样点" if self.local_waveform is not None else "记录极值摘要 · 导出后可缩放查看原始样点")
        self.update_waveform_caption()
        display_spectrum=result.get('display_spectrum')
        if display_spectrum:
            points=decode_spectrum(display_spectrum)
            if unit!='V':points=[[(hz,a*abs(voltage_scale)) for hz,a in ch] for ch in points]
            self.spectrum_view.set_data(points,unit,source_id=source_id)
            self.spectrum_view.set_sampling_context(domain=(0,display_spectrum.get('max_hz',2000)))
            channel=display_spectrum['channels'][0]
        else:
            self.spectrum_view.set_data([list(zip(channel["frequency_hz"],[a*abs(factor) for a in channel["peak_amplitude"]])) for channel in result["spectra"]],unit,source_id=source_id)
            channel=result['spectra'][0]
        self.spectrum_label.setText("记录平均幅度谱 · FFT "+str(channel["fft_size"])+" · 分辨率 "+text_number(channel["bin_width_hz"]," Hz"))
        speed=result["speed"]; use_rpm=speed.get("pulses_per_rev") is not None
        self.waveforms.set_speed([(p["time_s"],p["rpm" if use_rpm else "pulse_frequency_hz"]) for p in speed["trace"]],"rpm" if use_rpm else "Hz",source_id=source_id)
        self.waveforms.set_speed_summary(speed.get("held_mean_rpm"),"全记录有效区间平均转速")
        synthetic=result.get("source_metadata",{}).get("synthetic") is True
        self.source_label.setText("来源：合成自检" if synthetic else "来源：未确认机械数据" )
        self.show_analysis_features(result)
        self.update_controls()

    def show_analysis_features(self,result,segment=False):
        synthetic=result.get("source_metadata",{}).get("synthetic") is True
        unit,factor=display_conversion(result)
        self.diagnosis_title.setText("合成自检数据" if synthetic else "特征分析完成 · 尚未分类")
        prefix="最近保存分段。" if segment else ""
        self.diagnosis_text.setText(prefix+"当前无经过真实四态样本验证的分类模型。正常、不平衡、不对中和松动不能仅凭显示曲线判定。")
        for row,stats in enumerate(display_stats(result["time_stats"],factor)):
            values=[f"CH{row+1}",text_number(stats["rms"]),text_number(stats["peak_to_peak"]),text_number(stats["crest_factor"]),text_number(stats["pearson_kurtosis"])]
            for col,value in enumerate(values): self.feature_table.setItem(row,col,QTableWidgetItem(value))
        self.limitations.setText("幅值单位："+unit+"。未完成逐轴标定时，名义加速度不能当作已校准测量。\n完整记录经过 CRC 和采样连续性校验；这些校验不代表机械故障分类准确率。")
        self.update_controls()

    def open_file(self):
        path,_=QFileDialog.getOpenFileName(self,"打开采集数据",str(self.save_directory),"采集数据 (*.BIN *.bin)")
        if path: self.analyze_local(Path(path))

    def analyze_local(self,path):
        if self.active or self.analysis_process: return
        if not self.analyzer.is_file(): self.show_notice("缺少分析程序："+str(self.analyzer)); return
        self.clear_result(); self.local_path=Path(path); self.result_origin="local"
        try:self.analysis_identity=capture_file_identity(path)
        except (OSError,ValueError) as exc:self.show_notice(str(exc));return
        self.status.setText("正在校验并分析文件"); self.show_notice("")
        self.analysis_output=bytearray(); self.analysis_errors=bytearray()
        process=QProcess(self); self.analysis_process=process
        process.analysis_path=self.local_path;process.analysis_identity=self.analysis_identity
        process.setProcessChannelMode(QProcess.ProcessChannelMode.SeparateChannels)
        process.readyReadStandardOutput.connect(lambda:self.read_analysis(process))
        process.readyReadStandardError.connect(lambda:self.analysis_errors.extend(bytes(process.readAllStandardError())))
        process.finished.connect(lambda code,status:self.analysis_finished(process,code,status))
        process.errorOccurred.connect(lambda error:self.analysis_error(process,error))
        arguments=[str(path),"--fft-size",str(self.config["fft"]),"--max-hz",str(self.config["max_hz"]),"--waveform-points","1200"]
        # Inspect only to choose the CLI option. The core still verifies the
        # entire header/payload CRC, format and record continuity independently.
        try:
            with Path(path).open("rb") as handle:header=handle.read(64)
            if len(header)>=64 and header[:8]==b"CAPDDR2\0" and struct.unpack_from("<I",header,60)[0]==0:
                arguments += ["--ppr",str(self.config["ppr"])]
        except OSError:pass
        process.start(str(self.analyzer),arguments)
        self.update_controls()

    def analysis_error(self,process,error):
        if process is not self.analysis_process:return
        self.show_notice("分析进程错误："+process.errorString())
        if error==QProcess.ProcessError.FailedToStart:
            self.analysis_process=None;self.result=None;self.status.setText("分析程序无法启动");self.diagnosis_title.setText("分析不可用")
            process.deleteLater();self.update_controls()

    def read_analysis(self,process):
        self.analysis_output.extend(bytes(process.readAllStandardOutput()))
        if len(self.analysis_output)>16*1024*1024: process.kill(); self.show_notice("分析输出超过限制")

    def analysis_finished(self,process,code,status):
        if process is not self.analysis_process: return
        self.read_analysis(process); self.analysis_errors.extend(bytes(process.readAllStandardError()))
        self.analysis_process=None
        try:
            if code!=0 or status!=QProcess.ExitStatus.NormalExit: raise ProtocolError(bytes(self.analysis_errors).decode("utf-8",errors="replace").strip() or "分析程序异常退出")
            # Bind the successful render to this child process's input receipt,
            # independently of any later board-side events.
            self.local_path=process.analysis_path;self.analysis_identity=process.analysis_identity
            self.result_origin="local";self.current_job="";self.record_id=""
            self.render_result(json.loads(self.analysis_output))
            self.status.setText("文件分析完成 · "+self.local_path.name)
            if not self.local_path.with_suffix(".meta.json").is_file(): self.show_notice("未找到同名来源说明。硬件自检标记仍会强制识别，其他来源保持未确认。")
        except Exception as exc:
            self.result=None; self.status.setText("文件分析失败"); self.show_notice(str(exc)); self.diagnosis_title.setText("文件不可用")
        process.deleteLater(); self.update_controls()

    def show_records(self,records,next_cursor=""):
        self.records=records
        if self.record_dialog is not None:
            previous_dialog=self.record_dialog;previous_dialog.close();previous_dialog.deleteLater()
        dialog=QDialog(self);self.record_dialog=dialog
        dialog.setWindowTitle("SD 记录"); dialog.resize(900,500); dialog.setWindowModality(Qt.WindowModality.WindowModal); box=QVBoxLayout(dialog)
        table=QTableWidget(len(records),3); table.setHorizontalHeaderLabels(["记录","大小","来源"]); table.horizontalHeader().setSectionResizeMode(0,QHeaderView.ResizeMode.Stretch); table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows); table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers); box.addWidget(table)
        table.setObjectName("sdRecordsTable");table.setSelectionMode(QTableWidget.SelectionMode.ExtendedSelection)
        for row,record in enumerate(records):
            for col,text in enumerate([record["record_id"].split("/")[-1],f"{record.get('bytes',0)/1048576:.1f} MiB","合成自检" if record.get("synthetic") else "ADC / 未确认"]): table.setItem(row,col,QTableWidgetItem(text))
        buttons=QHBoxLayout()
        previous=QPushButton("上一页");previous.setEnabled(self.record_page>0);buttons.addWidget(previous)
        following=QPushButton("下一页");following.setEnabled(bool(next_cursor));buttons.addWidget(following)
        buttons.addWidget(QLabel(f"第 {self.record_page+1} 页"));buttons.addStretch()
        remove=QPushButton("删除所选数据");remove.setObjectName("deleteSdRecords");remove.setEnabled(False);buttons.addWidget(remove)
        view=QPushButton("显示与分析");view.setEnabled(False); buttons.addWidget(view); box.addLayout(buttons)
        def selection_changed():
            rows=table.selectionModel().selectedRows()
            idle=self.session.connected and not (self.active or self.remote_busy or self.download or self.download_pending or self.analysis_process or self.delete_request_id or self.records_request_id)
            remove.setEnabled(idle and bool(rows));view.setEnabled(idle and len(rows)==1)
        table.itemSelectionChanged.connect(selection_changed)
        def turn_page(offset):
            page=self.record_page+offset
            if offset>0:
                self.record_cursors=self.record_cursors[:page]+[next_cursor]
            previous.setEnabled(False);following.setEnabled(False);view.setEnabled(False);remove.setEnabled(False)
            self.request_records(cursor=self.record_cursors[page],page=page)
        previous.clicked.connect(lambda:turn_page(-1));following.clicked.connect(lambda:turn_page(1))
        def choose():
            row=table.currentRow()
            if row<0:return
            if self.remote_busy or self.active:return
            chosen=records[row]["record_id"]; dialog.accept(); self.clear_result(); self.remote_busy=True; self.status.setText("正在分析板端记录")
            self.remote_analysis_request_id=self.send("analyze_record",record_id=chosen)
            if not self.remote_analysis_request_id:self.remote_busy=False
            self.update_controls()
        view.clicked.connect(choose); table.cellDoubleClicked.connect(lambda *_:choose())
        def delete_selected():
            selected=[records[index.row()] for index in table.selectionModel().selectedRows()]
            if self.delete_records(selected,dialog):dialog.accept()
        remove.clicked.connect(delete_selected)
        # Non-blocking pagination: each response replaces only this record page.
        dialog.finished.connect(lambda _: self.clear_record_dialog(dialog))
        dialog.show()

    def clear_record_dialog(self,dialog):
        if self.record_dialog is dialog:self.record_dialog=None

    def delete_records(self,records,parent=None):
        if not records or not self.session.connected or self.active or self.remote_busy or self.download or self.download_pending or self.analysis_process or self.delete_request_id:return False
        ids=[record["record_id"] for record in records]
        if len(ids)>100 or len(set(ids))!=len(ids):return False
        size=sum(int(record.get("bytes",0)) for record in records)
        confirm=QMessageBox(QMessageBox.Icon.Warning,"删除 SD 数据",f"删除选中的 {len(ids)} 段采集数据（约 {size/1048576:.1f} MiB）？\n原始数据及对应说明文件会一并删除，无法撤销。",QMessageBox.StandardButton.Yes|QMessageBox.StandardButton.No,parent or self)
        confirm.setDetailedText("\n".join(ids));confirm.setDefaultButton(QMessageBox.StandardButton.No)
        confirm.button(QMessageBox.StandardButton.Yes).setText("确认删除");confirm.button(QMessageBox.StandardButton.No).setText("取消")
        if confirm.exec()!=QMessageBox.StandardButton.Yes:return False
        # Recheck after the confirmation's event loop: disconnect or capture
        # state changes must never let an old selection issue a delete.
        if not self.session.connected or self.active or self.remote_busy or self.download or self.download_pending or self.analysis_process or self.delete_request_id:return False
        self.delete_request_id=self.send("delete_records",record_ids=ids)
        if self.delete_request_id:self.status.setText("正在删除所选 SD 数据")
        self.update_controls();return self.delete_request_id is not None

    def records_deleted(self,data):
        results=data.get("results",[]);deleted={item["record_id"] for item in results if item.get("deleted") is True}
        failures=[item for item in results if item.get("deleted") is not True]
        changed=deleted|{item["record_id"] for item in results if item.get("files_removed",0)>0 or item.get("remaining_staging_id")}
        if self.record_id in changed:
            if self.local_path and self.local_path.is_file():self.record_id=""
            else:self.clear_result()
        self.status.setText(f"已删除 {len(deleted)} 段 SD 数据")
        self.show_notice("；".join(f"{item['record_id']}：{item.get('error',{}).get('message','未删除')}" for item in failures))
        self.request_records()

    def begin_download(self):
        if self.download:
            self.send("download_end");self.download.abort(); self.download=None; self.download_request_id=None; self.progress.hide(); self.status.setText("已停止传回 · 板端原文件保留"); self.update_controls(); return
        if self.download_pending:return
        path=QFileDialog.getExistingDirectory(self,"选择原始数据保存目录",str(self.save_directory))
        if path:
            self.save_directory=Path(path);self.download_pending=True
            self.download_request_id=self.send("download_begin",record_id=self.record_id)
            if not self.download_request_id:self.download_pending=False
            self.update_controls()

    def download_started(self,data):
        self.download_pending=False
        try:
            if self.download:raise ProtocolError("Duplicate download manifest")
            self.download=RecordDownload(self.save_directory,data)
        except Exception:
            # The board already holds the selected pair after download_begin.
            # Local destination/manifest rejection must release that reference.
            self.send("download_end")
            if self.download:self.download.abort();self.download=None
            self.download_request_id=None;self.progress.hide()
            raise
        self.download_start=time.monotonic()
        self.progress.setRange(0,1000); self.progress.setValue(0); self.progress.show(); self.status.setText("正在传回原始数据与来源说明")
        self.download_request_id=self.send("download_chunk",**self.download.next_chunk()); self.update_controls()

    def download_chunk(self,data):
        if self.download is None: return
        try:
            self.download.accept(data); self.progress.setValue(int(1000*self.download.received/self.download.total))
            elapsed=max(.1,time.monotonic()-self.download_start); rate=self.download.received/elapsed
            remaining=(self.download.total-self.download.received)/max(1,rate)
            self.status.setText(f"传回 {self.download.received/1048576:.1f}/{self.download.total/1048576:.1f} MiB · 约剩 {remaining/60:.1f} 分钟")
            chunk=self.download.next_chunk()
            if chunk: self.download_request_id=self.send("download_chunk",**chunk)
            else:
                path=self.download.finish(); self.download=None;self.download_request_id=None; self.progress.hide(); self.show_notice(""); self.analyze_local(path)
        except Exception:
            self.send("download_end")
            if self.download: self.download.abort(); self.download=None
            self.download_pending=False;self.download_request_id=None
            self.progress.hide(); raise

    def auto_save_analysis(self):
        try:
            directory=self.save_directory/"分析结果"; directory.mkdir(parents=True,exist_ok=True)
            name=self.record_id.replace("/","_")+".json"
            with (directory/name).open("x",encoding="utf-8") as handle: json.dump(self.result,handle,ensure_ascii=False,allow_nan=False,indent=2)
        except FileExistsError: pass
        except OSError as exc: self.show_notice("分析已完成，结果自动保存失败："+str(exc))

    def export_result(self):
        if self.result is None:return
        path,_=QFileDialog.getSaveFileName(self,"导出分析结果",str(self.save_directory/"分析结果.json"),"JSON (*.json)")
        if path:
            try:
                with Path(path).open("x",encoding="utf-8") as handle:json.dump(self.result,handle,ensure_ascii=False,allow_nan=False,indent=2)
            except FileExistsError:self.show_notice("文件已存在，请选择新的文件名。")
            except OSError as exc:self.show_notice(str(exc))

    def closeEvent(self,event):
        if not self.closing_window and (self.active or self.download or self.download_pending):
            result=QMessageBox.question(self,"结束当前操作","关闭将停止采集并保存最后一段到 SD；未完成的电脑文件传回会中止。继续关闭？")
            if result!=QMessageBox.StandardButton.Yes:event.ignore();return
        if self.download:self.download.abort();self.download=None
        if self.analysis_process:self.analysis_process.kill()
        if self.session.phase!="closed":
            self.closing_window=True; self.session.disconnect(); event.ignore(); return
        if self.local_waveform is not None:self.local_waveform.close();self.local_waveform=None
        event.accept()


def main():
    parser=argparse.ArgumentParser(description="Zynq vibration serial workbench")
    parser.add_argument("--analyzer",type=Path)
    parser.add_argument("--open",type=Path)
    args=parser.parse_args()
    app=QApplication(sys.argv[:1]); app.setApplicationName("VibrationSerial"); configure_application(app)
    window=Window(args.analyzer);window.show()
    if args.open:QTimer.singleShot(100,lambda:window.analyze_local(args.open))
    return app.exec()


if __name__=="__main__":
    raise SystemExit(main())
