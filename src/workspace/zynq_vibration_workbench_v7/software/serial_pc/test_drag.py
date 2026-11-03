"""Qt nested-loop deletion regression; this does not simulate OS mouse dragging."""
import os
import unittest
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM","offscreen")

from PySide6.QtCore import QCoreApplication, QEvent, QEventLoop, QPoint, QPointF, Qt, QTimer
from PySide6.QtGui import QMouseEvent
from PySide6.QtWidgets import QApplication
from shiboken6 import isValid

import plots


class DragLifetimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app=QApplication.instance() or QApplication([])

    def test_source_deleted_during_nested_drop_loop(self):
        stack=plots.WaveformStack()
        source=next(button for button in stack.rows[0].findChildren(plots.DragButton)
                    if button.kind=="channel" and button.token==0)
        source.origin=QPoint(0,0)
        observations={}

        class NestedDrop:
            def __init__(self,button):self.source=button
            def setMimeData(self,mime):
                observations["channel_payload"]=bytes(mime.data("application/x-vibration-channel"))
            def exec(self,action):
                loop=QEventLoop()
                def rebuild():
                    stack.merge("ch2",0)
                    QCoreApplication.sendPostedEvents(None,QEvent.Type.DeferredDelete)
                    observations["source_deleted_in_nested_loop"]=not isValid(self.source)
                    loop.quit()
                QTimer.singleShot(0,rebuild)
                loop.exec()
                return Qt.DropAction.MoveAction

        move=QMouseEvent(QEvent.Type.MouseMove,QPointF(12,0),QPointF(12,0),
                         Qt.MouseButton.NoButton,Qt.MouseButton.LeftButton,Qt.KeyboardModifier.NoModifier)
        try:
            with patch.object(plots,"QDrag",NestedDrop):source.mouseMoveEvent(move)
            self.assertEqual(observations["channel_payload"],b"0")
            self.assertTrue(observations["source_deleted_in_nested_loop"])
            self.assertTrue(any(set(channels)=={0,1} for name,channels in stack.groups if name!="overview"))
        finally:
            stack.deleteLater();QCoreApplication.sendPostedEvents(None,QEvent.Type.DeferredDelete)


if __name__=="__main__":unittest.main(verbosity=2)
