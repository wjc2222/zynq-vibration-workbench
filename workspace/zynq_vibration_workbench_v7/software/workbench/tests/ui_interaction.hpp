#pragma once
#include <QApplication>
#include <QAbstractItemView>
#include <QComboBox>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QPushButton>
#include <QThread>
#include <QLineEdit>
#include <QKeyEvent>
#include <QWheelEvent>
#include <stdexcept>

namespace vibration::workbench::qa::interaction {
inline void events(int ms=35){QElapsedTimer timer;timer.start();do{QApplication::processEvents();QThread::msleep(1);}while(timer.elapsed()<ms);}
inline void tapAt(QWidget* widget,const QPoint& local){
    if(!widget||!widget->isVisible()||!widget->isEnabled())throw std::runtime_error("touch target is hidden/disabled");
    const QPoint global=widget->mapToGlobal(local);auto* hit=QApplication::widgetAt(global);
    if(!hit||(hit!=widget&&!widget->isAncestorOf(hit)))throw std::runtime_error(("touch target occluded: "+widget->objectName()).toStdString());
    const auto point=hit->mapFromGlobal(global);
    QMouseEvent move(QEvent::MouseMove,point,global,Qt::NoButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(hit,&move);
    QMouseEvent press(QEvent::MouseButtonPress,point,global,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(hit,&press);
    QMouseEvent release(QEvent::MouseButtonRelease,point,global,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(hit,&release);events();
}
inline void tap(QWidget* widget){tapAt(widget,widget->rect().center());}
inline void choose(QComboBox* combo,const QString& data){
    const int row=combo->findData(data);if(row<0)throw std::runtime_error("missing touch choice");tap(combo);auto* view=combo->view();const auto index=combo->model()->index(row,0);view->scrollTo(index);events(250);const auto area=view->visualRect(index);if(area.height()<44)throw std::runtime_error("choice row below 44px");tapAt(view->viewport(),area.center());
}
inline QPushButton* button(QObject& root,const char* id){auto* result=root.findChild<QPushButton*>(id);if(!result)throw std::runtime_error(std::string("missing button ")+id);return result;}
inline void dataAction(QWidget& window,const char* id){tap(button(window,"workbenchDataMenu"));tap(button(window,id));}
inline void settingAction(QWidget& window,const char* id){tap(button(window,"workbenchOptions"));tap(button(window,id));}
inline void condition(QWidget& window,const QString& key){auto* combo=window.findChild<QComboBox*>("nextCaptureCondition");if(!combo)throw std::runtime_error("missing condition dropdown");choose(combo,key);}
inline void type(QLineEdit* field,const QString& text){tap(field);QKeyEvent select(QEvent::KeyPress,Qt::Key_A,Qt::ControlModifier);QApplication::sendEvent(field,&select);for(const auto ch:text){QKeyEvent key(QEvent::KeyPress,ch.unicode(),Qt::NoModifier,QString(ch));QApplication::sendEvent(field,&key);}events();}
inline void zoomAndPan(QWidget* graph,const QRectF& area){
 const QPoint point=area.center().toPoint(),global=graph->mapToGlobal(point);QWheelEvent wheel(point,global,QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(graph,&wheel);events();
 QMouseEvent press(QEvent::MouseButtonPress,point,global,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(graph,&press);
 const QPoint moved=point+QPoint(-45,0);QMouseEvent move(QEvent::MouseMove,moved,graph->mapToGlobal(moved),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(graph,&move);QMouseEvent release(QEvent::MouseButtonRelease,moved,graph->mapToGlobal(moved),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(graph,&release);events();
}
}
