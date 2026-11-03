#include "waveform_stack.hpp"
#include "controls.hpp"
#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFrame>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QTextCodec>
#include <QWheelEvent>
#include <QFileInfo>
#include <QDir>
#include <QVariant>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
const char* mime="application/x-vibration-wave-layout";
QJsonArray checks,events;
void check(bool ok,const QString& name){checks.append(QJsonObject{{"check",name},{"passed",ok}});if(!ok)throw std::runtime_error(name.toStdString());}
QFrame* frame(vibration::workbench::WaveformStack& stack,int id){for(auto* f:stack.findChildren<QFrame*>())if(f->objectName()=="viewPanel"&&f->property("stablePanelId").toInt()==id)return f;throw std::runtime_error("Stable frame missing");}
QPushButton* button(vibration::workbench::WaveformStack& stack,const QString& id){for(auto* b:stack.findChildren<QPushButton*>())if(b->objectName()==id&&!b->isHidden())return b;throw std::runtime_error(("Visible button missing: "+id).toStdString());}
QByteArray payload(vibration::workbench::WaveformStack& stack,const QString& id){auto* b=button(stack,id);auto data=b->property("waveDragPayload").toByteArray();check(!data.isEmpty(),"payload read from actual DragButton property: "+id);return data;}
void expose(vibration::workbench::WaveformStack& stack,QWidget* widget){stack.ensureWidgetVisible(widget,0,12);QApplication::processEvents();check(widget->isVisible(),"actual destination frame visible");}
struct Outcome{bool enter=false,drop=false;};
Outcome drop(vibration::workbench::WaveformStack& stack,int target,const QByteArray& bytes,bool after,const QString& label,const char* format=mime){
    auto* destination=frame(stack,target);expose(stack,destination);
    const QPoint position(destination->width()/2,after?destination->height()*3/4:destination->height()/4);
    QMimeData data;data.setData(format,bytes);
    QDragEnterEvent enter(position,Qt::MoveAction,&data,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(destination,&enter);
    QDropEvent finish(QPointF(position),Qt::MoveAction,&data,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(destination,&finish);
    QApplication::processEvents();events.append(QJsonObject{{"case",label},{"enter_accepted",enter.isAccepted()},{"drop_accepted",finish.isAccepted()},{"action",int(finish.dropAction())}});
    return {enter.isAccepted(),finish.isAccepted()};
}
void accepted(const Outcome& o,const QString& name){check(o.enter&&o.drop,"native MoveAction enter/drop accepted: "+name);}
void rejected(const Outcome& o,const QString& name){check(!o.enter&&!o.drop,"native invalid enter/drop rejected: "+name);}
void click(QPushButton* b){
    check(b->isVisible()&&b->isEnabled()&&b->width()>=44&&b->height()>=44,"visible enabled 44px split button");
    const QPointF p(b->rect().center());QMouseEvent down(QEvent::MouseButtonPress,p,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier),up(QEvent::MouseButtonRelease,p,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(b,&down);QApplication::sendEvent(b,&up);QApplication::processEvents();
}
void shot(vibration::workbench::WaveformStack& stack,const QDir& out,const QString& name){QApplication::processEvents();check(stack.grab().save(out.filePath(name+".png")),"actual Qt stack screenshot: "+name);}
}
int main(int argc,char** argv){
    QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8"));QApplication app(argc,argv);
    for(const auto& path:{"/mnt/c/Windows/Fonts/segoeui.ttf","/mnt/c/Windows/Fonts/seguisb.ttf","/mnt/c/Windows/Fonts/msyh.ttc","/mnt/c/Windows/Fonts/msyhbd.ttc"})if(QFileInfo::exists(path))QFontDatabase::addApplicationFont(path);
    QFont font;font.setFamilies({"Segoe UI","Microsoft YaHei UI","Microsoft YaHei","Noto Sans CJK SC","sans-serif"});font.setPixelSize(14);font.setWeight(QFont::Normal);app.setFont(font);app.setStyleSheet(vibration::workbench::stylesheet());
    QJsonObject report{{"schema","native-wave-drop-qa-v1"},{"boundary","Actual Qt DragEnter/Drop/Mouse/Wheel event dispatch with real DragButton payloads; generated curves only. Physical mouse/touch and hardware are not validated."}};
    try{
        check(argc==3,"evidence directory and size arguments");QDir out(QString::fromLocal8Bit(argv[1]));const auto size=QString::fromLatin1(argv[2]).split('x');check(out.exists()&&size.size()==2,"test path and extent");
        vibration::workbench::WaveformStack stack;stack.resize(size[0].toInt(),size[1].toInt());
        QVector<vibration::workbench::PlotSeries> series;
        for(int channel=0;channel<3;++channel){vibration::workbench::PlotSeries s;s.name=QString("CH%1").arg(channel+1);s.color=QColor(channel==0?"#3769c7":channel==1?"#268878":"#b78032");for(int i=0;i<1000;++i)s.points.push_back({i/5000.,(.15+.05*channel)*std::sin(2*3.141592653589793*(80+20*channel)*i/5000.)});series.push_back(s);}
        const vibration::workbench::PlotSeries speed{QStringLiteral("RPM"),QColor("#3769c7"),{{0,1500},{.2,1500}}};
        stack.setSourceSeries(series,QStringLiteral("g（合成示意）"));stack.setSpeedSeries(speed,"RPM");stack.setXRange(0,.2);stack.show();QApplication::processEvents();
        check(stack.panelOrder()==QVector<int>({0,1,2,3,4}),"initial four vibration panels and independent speed panel");
        check(stack.panelChannels(4).isEmpty()&&stack.plot(4)->series().size()==1&&stack.plot(4)->series()[0].points==speed.points,"fifth panel keeps independent RPM source");shot(stack,out,"00-initial");
        accepted(drop(stack,2,payload(stack,"waveMovePanel0"),true,"panel0 after panel2"),"panel reorder after");check(stack.panelOrder()==QVector<int>({1,2,0,3,4}),"native lower-half drop places panel after target");
        accepted(drop(stack,1,payload(stack,"waveMovePanel3"),false,"panel3 before panel1"),"panel reorder before");check(stack.panelOrder()==QVector<int>({3,1,2,0,4}),"native upper-half drop places panel before target");stack.verticalScrollBar()->setValue(0);shot(stack,out,"01-reordered");
        stack.resetLayout();QApplication::processEvents();
        accepted(drop(stack,1,payload(stack,"waveDragChannel0_0"),false,"CH1 to CH2"),"CH1 merge");check(!stack.panelOrder().contains(0)&&!frame(stack,0)->isVisible()&&stack.panelChannels(1)==QVector<int>({0,1}),"CH1 source disappears and CH2 retains both channels");
        accepted(drop(stack,1,payload(stack,"waveDragChannel2_2"),false,"CH3 to CH2"),"CH3 merge");check(!stack.panelOrder().contains(2)&&!frame(stack,2)->isVisible()&&stack.panelChannels(1)==QVector<int>({0,1,2}),"CH3 source disappears and destination has three channels");
        accepted(drop(stack,1,payload(stack,"waveDragChannel3_0"),false,"duplicate destination CH1"),"duplicate destination");check(stack.panelChannels(1)==QVector<int>({0,1,2})&&stack.panelChannels(3)==QVector<int>({1,2}),"destination deduplicates while source channel moves");stack.verticalScrollBar()->setValue(0);shot(stack,out,"02-merged");
        expose(stack,frame(stack,1));click(button(stack,"waveRemoveChannel1_2"));check(stack.panelOrder().size()==4&&stack.panelChannels(1)==QVector<int>({0,1})&&stack.panelChannels(0)==QVector<int>({2})&&frame(stack,0)->isVisible(),"real split arrow restores CH3 as a visible independent panel");shot(stack,out,"03-split");
        const auto order=stack.panelOrder(),one=stack.panelChannels(1);stack.setSourceSeries(series,QStringLiteral("g（合成示意）"));check(stack.panelOrder()==order&&stack.panelChannels(1)==one,"new source rendering preserves native user arrangement");
        vibration::workbench::WaveformStack foreign;
        rejected(drop(stack,1,payload(foreign,"waveMovePanel0"),false,"foreign nonce"),"foreign nonce");
        auto malformed=payload(stack,"waveMovePanel1");malformed.append(":unexpected");rejected(drop(stack,1,malformed,false,"invalid fields"),"invalid payload shape");
        auto fields=payload(stack,"waveDragChannel1_0").split(':');fields[3]="99";const auto invalidChannel=fields[0]+":"+fields[1]+":"+fields[2]+":"+fields[3];rejected(drop(stack,1,invalidChannel,false,"invalid channel"),"invalid channel");
        rejected(drop(stack,1,payload(stack,"waveMovePanel1"),false,"wrong MIME","text/plain"),"wrong MIME");
        const auto beforeSelf=stack.panelOrder();
        rejected(drop(stack,1,payload(stack,"waveMovePanel1"),false,"self panel"),"self panel");
        rejected(drop(stack,1,payload(stack,"waveDragChannel1_0"),false,"self channel"),"self channel");
        check(stack.panelOrder()==beforeSelf&&stack.panelChannels(1)==one,"invalid and self drops do not mutate layout");
        rejected(drop(stack,4,payload(stack,"waveDragChannel1_0"),false,"vibration channel to speed panel"),"speed panel rejects vibration merge");
        check(stack.panelChannels(4).isEmpty()&&stack.plot(4)->series().size()==1&&stack.plot(4)->series()[0].points==speed.points,"drag and merge preserve independent RPM values");
        stack.resetLayout();QApplication::processEvents();check(stack.verticalScrollBar()->maximum()>0,"five vertical panels exceed viewport and scroll");
        for(int i=0;i<20&&stack.verticalScrollBar()->value()<stack.verticalScrollBar()->maximum();++i){const QPointF local(stack.viewport()->rect().center());QWheelEvent wheel(local,QPointF(stack.viewport()->mapToGlobal(local.toPoint())),QPoint(),QPoint(0,-360),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(stack.viewport(),&wheel);QApplication::processEvents();}
        check(stack.verticalScrollBar()->value()==stack.verticalScrollBar()->maximum(),"native wheel reaches final panel");
        const QRect last(frame(stack,4)->mapTo(stack.viewport(),QPoint()),frame(stack,4)->size());check(last.intersects(stack.viewport()->rect())&&last.bottom()<=stack.viewport()->height()+2,"independent speed tail panel bottom is reachable");shot(stack,out,"04-scroll-tail");report["result"]="passed";
    }catch(const std::exception& e){report["result"]="failed";report["error"]=QString::fromUtf8(e.what());}
    report["checks"]=checks;report["events"]=events;const auto data=QJsonDocument(report).toJson(QJsonDocument::Indented);std::fwrite(data.constData(),1,std::size_t(data.size()),stdout);return report.value("result")=="passed"?0:1;
}
