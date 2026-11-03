"""Compact readout; plot navigation is performed directly with the mouse."""
from PySide6.QtWidgets import QWidget, QVBoxLayout, QLabel


class WaveformControls(QWidget):
    def __init__(self,target,parent=None):
        super().__init__(parent)
        self.target=target
        box=QVBoxLayout(self);box.setContentsMargins(0,0,0,0);box.setSpacing(2)
        self.note=QLabel();self.note.setObjectName('muted');box.addWidget(self.note)
        self.stats=QLabel();self.stats.setObjectName('muted');self.stats.setWordWrap(True)
        target.view_changed.connect(self.refresh);target.stats_changed.connect(self.refresh)
        self.refresh()

    def refresh(self):
        target=self.target
        bounds=target.viewport.x_range or target.viewport.domain
        unit='Hz' if getattr(target,'x_label','').startswith('频率') else 's'
        state='已暂停 · 右键回到最新' if target.paused else '实时' if target.live_mode else ''
        self.note.setText(f'{state}  {bounds[0]:.5g}–{bounds[1]:.5g} {unit}'.strip())
        self.note.setToolTip('左键拖动平移 · 滚轮缩放 · Shift＋拖动框选 · 双击复位；纵轴上调整幅值')
        if hasattr(target,'stats_text'):self.stats.setText(target.stats_text())
