import copy
import math
import os
import sys
import unittest
from pathlib import Path
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from PySide6.QtCore import Qt, QPointF, QPoint, QEvent
from PySide6.QtGui import QMouseEvent, QWheelEvent
from PySide6.QtWidgets import QApplication
from plots import Viewport, Plot, WaveformStack, envelope, AmplitudeDialog
from waveform_controls import WaveformControls

APP=QApplication.instance() or QApplication([])


class NavigationTests(unittest.TestCase):
    def data(self):
        return [[(i/100,math.sin(i/10)+ch) for i in range(1001)] for ch in range(3)]

    def test_numeric_anchor_clamp_invalid(self):
        view=Viewport();view.domain=(0,10)
        self.assertTrue(view.zoom(.5,2))
        self.assertEqual(view.x_range,(1,6))
        self.assertTrue(view.pan(-100));self.assertEqual(view.x_range,(0,5))
        self.assertTrue(view.pan(100));self.assertEqual(view.x_range,(5,10))
        before=view.x_range
        for invalid in (float('nan'),float('inf'),-1,0):self.assertFalse(view.zoom(invalid))
        self.assertEqual(view.x_range,before)
        for low,high in ((1,1),(2,1),(float('nan'),2),(0,float('inf'))):self.assertFalse(view.set_y(low,high))
        self.assertIsNone(view.y_range)

    def test_linked_manual_persist_merge_reorder_and_units(self):
        stack=WaveformStack();data=self.data();original=copy.deepcopy(data)
        stack.set_data(data,"V",source_id="one")
        self.assertTrue(stack.set_y_range(-1.25,3.75))
        stack.rows[1].plot.zoom(.5,2)
        bounds=stack.viewport.x_range
        self.assertEqual(bounds,(1,6))
        for row in stack.rows:self.assertEqual(row.plot.viewport.x_range,bounds)
        stack.merge("ch1",1);stack.reorder("overview","ch1")
        for row in stack.rows:
            self.assertEqual(row.plot.viewport.y_range,None if row.is_speed else (-1.25,3.75));self.assertEqual(row.plot.viewport.x_range,bounds)
        self.assertEqual(stack.data,original)
        stack.set_data(data,"V",source_id="one")
        self.assertEqual(stack.viewport.y_range,(-1.25,3.75))
        stack.set_data(data,"g（名义）",source_id="two")
        self.assertIsNone(stack.viewport.y_range);self.assertIsNone(stack.viewport.x_range)
        stack.set_y_range(-2,2);stack.reset_view()
        self.assertIsNone(stack.viewport.y_range);self.assertIsNone(stack.viewport.x_range)

    def test_live_frozen_rebuild_and_resume(self):
        stack=WaveformStack();stack.set_live_mode(True);stack.set_data(self.data())
        stack.zoom(.5,2);frozen=copy.deepcopy(stack.data)
        next_data=[[(x+10,y) for x,y in values] for values in self.data()]
        stack.set_data(next_data)
        self.assertTrue(stack.paused);self.assertEqual(stack.data,frozen)
        stack.reorder("overview","ch1");stack.hide();stack.show()
        self.assertTrue(all(row.plot.series for row in stack.rows))
        self.assertEqual(stack.data,frozen)
        stack.resume_live();self.assertFalse(stack.paused);self.assertEqual(stack.data,next_data)
        self.assertIsNone(stack.viewport.x_range)

    def test_geometry_wheel_drag_box_and_gaps(self):
        plot=Plot();plot.resize(900,260);plot.set_series([("#3569c8",self.data()[0])]);plot.show();APP.processEvents()
        area=plot._axis_layout()["area"];point=QPointF(area.left()+area.width()*.2,area.center().y())
        wheel=QWheelEvent(point,point,QPoint(0,0),QPoint(0,120),Qt.MouseButton.NoButton,Qt.KeyboardModifier.NoModifier,Qt.ScrollPhase.NoScrollPhase,False)
        APP.sendEvent(plot,wheel)
        self.assertAlmostEqual(plot.viewport.x_range[0],.4);self.assertAlmostEqual(plot.viewport.x_range[1],8.4)
        plot.set_navigation_mode("box")
        start=QPointF(area.left()+area.width()*.25,area.center().y());end=QPointF(area.left()+area.width()*.75,area.center().y())
        for kind,pos,button,buttons in ((QEvent.Type.MouseButtonPress,start,Qt.MouseButton.LeftButton,Qt.MouseButton.LeftButton),(QEvent.Type.MouseMove,end,Qt.MouseButton.NoButton,Qt.MouseButton.LeftButton),(QEvent.Type.MouseButtonRelease,end,Qt.MouseButton.LeftButton,Qt.MouseButton.NoButton)):
            APP.sendEvent(plot,QMouseEvent(kind,pos,pos,pos,button,buttons,Qt.KeyboardModifier.NoModifier))
        self.assertAlmostEqual(plot.viewport.x_range[0],2.4);self.assertAlmostEqual(plot.viewport.x_range[1],6.4)
        points=[(i/1000,math.sin(i)) for i in range(1000)];points.insert(500,(.5,float('nan')))
        reduced=envelope(points,20)
        self.assertTrue(any(not math.isfinite(y) for _,y in reduced));self.assertTrue(math.isnan(points[500][1]))
        plot.set_y_range(-1.25,3.75);self.assertEqual(plot._axis_layout()['ranges'][2:],(-1.25,3.75))

    def test_stats_require_authoritative_provenance_and_controls_reject_nan(self):
        stack=WaveformStack();stack.set_data(self.data());controls=WaveformControls(stack)
        self.assertIn('—',stack.stats_text())
        stats=[dict(rms=2,peak_to_peak=5)]*3
        stack.set_authoritative_stats(stats,"envelope",unit="V");self.assertIn('—',stack.stats_text())
        stack.set_authoritative_stats(stats,"raw_window",unit="V",sample_count=1001);self.assertIn('原始预览窗口',stack.stats_text())
        stack.set_authoritative_stats(stats,"complete_record",unit="V");self.assertIn('全记录',stack.stats_text())
        dialog=AmplitudeDialog(stack.rows[0].plot)
        dialog.lower.setText('nan');dialog.upper.setText('2');dialog.apply_range()
        self.assertIsNone(stack.viewport.y_range);self.assertIn('有限数',dialog.note.text())
        stack.set_authoritative_stats(stats,"raw_window",unit="g");self.assertIn('—',stack.stats_text())

    def test_direct_mouse_pan_amplitude_shift_box_double_click(self):
        stack=WaveformStack();stack.resize(900,900);stack.set_data(self.data());stack.show();APP.processEvents()
        plot=stack.rows[0].plot;area=plot._axis_layout()['area']
        def send(kind,pos,button,buttons,mod=Qt.KeyboardModifier.NoModifier):
            APP.sendEvent(plot,QMouseEvent(kind,pos,pos,pos,button,buttons,mod))
        def drag(start,end,mod=Qt.KeyboardModifier.NoModifier):
            send(QEvent.Type.MouseButtonPress,start,Qt.MouseButton.LeftButton,Qt.MouseButton.LeftButton,mod)
            send(QEvent.Type.MouseMove,end,Qt.MouseButton.NoButton,Qt.MouseButton.LeftButton,mod)
            send(QEvent.Type.MouseButtonRelease,end,Qt.MouseButton.LeftButton,Qt.MouseButton.NoButton,mod)
        start=QPointF(area.left()+area.width()*.2,area.center().y());end=QPointF(area.left()+area.width()*.6,area.center().y())
        drag(start,end,Qt.KeyboardModifier.ShiftModifier)
        self.assertAlmostEqual(stack.viewport.x_range[0],2);self.assertAlmostEqual(stack.viewport.x_range[1],6)
        drag(end,start)
        self.assertAlmostEqual(stack.viewport.x_range[0],3.6);self.assertAlmostEqual(stack.viewport.x_range[1],7.6)
        point=QPointF(20,area.center().y());before=plot._axis_layout()['ranges'][2:]
        APP.sendEvent(plot,QWheelEvent(point,point,QPoint(0,0),QPoint(0,120),Qt.MouseButton.NoButton,Qt.KeyboardModifier.NoModifier,Qt.ScrollPhase.NoScrollPhase,False))
        after=stack.viewport.y_range
        self.assertAlmostEqual(after[1]-after[0],.8*(before[1]-before[0]))
        drag(point,point+QPointF(0,10));self.assertGreater(stack.viewport.y_range[0],after[0])
        for row in stack.rows:self.assertEqual(row.plot.viewport.y_range,None if row.is_speed else stack.viewport.y_range)
        send(QEvent.Type.MouseButtonDblClick,start,Qt.MouseButton.LeftButton,Qt.MouseButton.LeftButton)
        self.assertIsNone(stack.viewport.x_range);self.assertIsNone(stack.viewport.y_range)
        from PySide6.QtWidgets import QPushButton,QComboBox,QLineEdit
        readout=WaveformControls(stack)
        self.assertFalse(readout.findChildren(QPushButton));self.assertFalse(readout.findChildren(QComboBox));self.assertFalse(readout.findChildren(QLineEdit))
        stack.close()

    def test_five_rows_independent_rpm_and_protected_merge(self):
        stack=WaveformStack();stack.set_data(self.data(),source_id='one')
        speed=[(i/100,1200+i/10) for i in range(1001)];stack.set_speed(speed,source_id='one')
        self.assertEqual(len(stack.rows),5)
        row=next(row for row in stack.rows if row.is_speed)
        self.assertEqual(row.plot.y_label,'rpm');self.assertEqual(row.plot.series[0][1],speed)
        row.plot.zoom(.5,2)
        for item in stack.rows:self.assertEqual(item.plot.viewport.x_range,(1,6))
        row.plot.set_y_range(1000,1600);self.assertIsNone(stack.viewport.y_range)
        stack.set_y_range(-2,2);self.assertEqual(row.plot.viewport.y_range,(1000.,1600.))
        before=copy.deepcopy(stack.groups);stack.merge('speed',0);self.assertEqual(stack.groups,before)
        stack.reorder('speed','ch1');self.assertEqual(stack.rows[0].row_id,'speed')
        self.assertEqual(stack.rows[0].plot.viewport.y_range,(1000.,1600.))
        self.assertEqual(stack.rows[0].plot.series[0][1],speed)
        stack.merge('ch2',0);self.assertEqual(len(stack.rows),4);self.assertTrue(any(r.is_speed for r in stack.rows))


if __name__=='__main__':unittest.main()
