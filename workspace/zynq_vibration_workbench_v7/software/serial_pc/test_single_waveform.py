import copy
import math
import os
import unittest
os.environ.setdefault('QT_QPA_PLATFORM','offscreen')
from PySide6.QtCore import QCoreApplication, QEvent
from PySide6.QtWidgets import QApplication
from shiboken6 import isValid
from single_waveform import SingleWaveformView

APP=QApplication.instance() or QApplication([])

class SingleCanvasTests(unittest.TestCase):
    def setUp(self):
        self.view=SingleWaveformView()
        self.addCleanup(self.dispose_view,self.view)
        self.data=[[(i*.01,math.sin(i*.1)+ch) for i in range(1001)] for ch in range(3)]
        self.view.set_data(self.data,'V','one')
        self.view.set_speed([(i*.01,1200+i*.1) for i in range(1001)],'rpm','one')
    def dispose_view(self,view):
        # close() only hides a QWidget. Dispose its native child tree while
        # QApplication and Python signal handlers are still alive, rather
        # than leaving cyclic view/plot connections to interpreter shutdown.
        if isValid(view):
            view.close()
            view.deleteLater()
        QCoreApplication.sendPostedEvents(None,QEvent.Type.DeferredDelete)
        APP.processEvents()
        self.assertFalse(isValid(view),'test-owned Qt view was not destroyed')
    def test_default_selection_overlay_and_hidden_data(self):
        view=self.view;before=copy.deepcopy(view.data)
        self.assertEqual(len(view.plot.series),1)
        view.channel_boxes[1].click();view.channel_boxes[2].click()
        self.assertEqual(len(view.plot.series),3)
        for check in view.channel_boxes:check.click()
        self.assertEqual(view.plot.series,[]);self.assertIn('选择通道',view.plot.empty_message)
        self.assertEqual(view.data,before);self.assertEqual(view.viewport.domain,(0,10))
        view.channel_boxes[2].click();self.assertEqual(view.plot.series[0][1],self.data[2])

    def test_zero_center_for_biased_voltage_and_acceleration(self):
        view=self.view
        for unit in ('V','m/s²','g'):
            for offset in (-20.,0.,20.):
                data=[[(i/100,offset+math.sin(i/5)) for i in range(101)] for _ in range(3)]
                before=copy.deepcopy(data)
                view.set_data(data,unit,f'{unit}-{offset}')
                lower,upper=view.plot._axis_layout()['ranges'][2:]
                self.assertEqual(lower,-upper)
                self.assertLessEqual(lower,min(y for _,y in data[0]))
                self.assertGreaterEqual(upper,max(y for _,y in data[0]))
                view.set_y_range(1.,3.)
                self.assertEqual(view.viewport.y_range,(-3.,3.))
                view.zoom(.5,.2)
                self.assertAlmostEqual(view.viewport.x_range[0],.1)
                self.assertAlmostEqual(view.viewport.x_range[1],.6)
                self.assertEqual(view.data,before)
        view.select_speed(True);view.set_y_range(1000.,1500.)
        self.assertEqual(view.viewport.y_range,(1000.,1500.))
        view.select_speed(False);self.assertEqual(view.viewport.y_range,(-3.,3.))

    def test_frequency_keeps_nonnegative_axis(self):
        spectrum=SingleWaveformView(allow_speed=False,x_label='频率 / Hz')
        self.addCleanup(self.dispose_view,spectrum)
        spectrum.set_data([[(0.,0.),(100.,2.),(200.,0.)]]*3,'V','fft')
        self.assertFalse(spectrum.plot.zero_centered_y)
        self.assertEqual(spectrum.plot._axis_layout()['ranges'][2],0.)
    def test_mode_switch_preserves_x_separates_y(self):
        view=self.view;view.zoom(.5,2);view.set_y_range(-2,2)
        view.speed_box.click()
        self.assertEqual(view.plot.y_label,'rpm');self.assertIsNone(view.viewport.y_range)
        self.assertEqual(view.viewport.x_range,(1,6));view.set_y_range(1000,1500)
        view.speed_box.click();self.assertEqual(view.viewport.y_range,(-2,2))
        view.speed_box.click();self.assertEqual(view.viewport.y_range,(1000,1500))
        view.channel_boxes[1].click()
        self.assertFalse(view.speed_box.isChecked());self.assertEqual(len(view.plot.series),1)
        self.assertEqual(view.plot.y_label,'V')
    def test_live_pause_selection_switch_and_resume(self):
        view=self.view;view.set_live_mode(True);view.zoom(.5)
        before=copy.deepcopy(view.data)
        later=[[(x+10,y) for x,y in ch] for ch in self.data]
        view.set_data(later,'V','one');self.assertEqual(view.data,before)
        view.channel_boxes[1].click();view.speed_box.click()
        self.assertTrue(view.paused);self.assertEqual(view.viewport.x_range,(2.5,7.5))
        view.resume_live();self.assertFalse(view.paused);self.assertEqual(view.data,later)
        self.assertIsNone(view.viewport.x_range)
    def test_source_change_clears_axes_and_old_stats(self):
        view=self.view;view.set_y_range(-2,2);view.zoom(.5)
        view.set_authoritative_stats([dict(rms=1,peak_to_peak=2)]*3,'complete_record',unit='V')
        view.set_data(self.data,'名义 g','two')
        self.assertIsNone(view.viewport.y_range);self.assertIsNone(view.viewport.x_range)
        self.assertFalse(view._stats);self.assertEqual(view.plot.y_label,'名义 g')
    def test_numeric_speed_is_authoritative_not_curve_mean(self):
        view=self.view;view.set_speed_summary(1444.5,'当前窗口平均转速');view.show_speed_value()
        self.assertEqual(view.speed_value.text(),'1444.5 rpm')
        view.set_speed_summary(None,'');self.assertEqual(view.speed_value.text(),'— rpm')
        view.set_speed([],unit='Hz');view.set_speed_summary(1800,'')
        self.assertEqual(view.speed_value.text(),'— rpm');self.assertIn('PPR',view.speed_description.text())
    def test_scope_units_and_spectrum_mode(self):
        view=self.view
        view.set_authoritative_stats([dict(rms=1,peak_to_peak=2)]*3,'envelope',unit='V');self.assertFalse(view._stats)
        view.set_authoritative_stats([dict(rms=1,peak_to_peak=2)]*3,'raw_window',unit='g');self.assertFalse(view._stats)
        spectrum=SingleWaveformView(allow_speed=False,x_label='频率 / Hz')
        self.addCleanup(self.dispose_view,spectrum)
        self.assertTrue(spectrum.speed_box.isHidden());self.assertTrue(spectrum.speed_button.isHidden());spectrum.close()

if __name__=='__main__':unittest.main(verbosity=2)
