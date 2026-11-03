#include "capture_page.hpp"
#include "capture_client.hpp"
#include "controls.hpp"
#include "test_fonts.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <functional>
#include <stdexcept>

namespace {
QJsonArray checks;
void check(bool ok,const QString& name){checks.append(QJsonObject{{"check",name},{"passed",ok}});if(!ok)throw std::runtime_error(name.toStdString());}
void waitFor(const std::function<bool()>& predicate,const QString& name){QElapsedTimer elapsed;elapsed.start();while(!predicate()&&elapsed.elapsed()<10000){QApplication::processEvents();QThread::msleep(2);}check(predicate(),name);}
template<class T>T* find(QObject& owner,const char* name){auto* widget=owner.findChild<T*>(name);check(widget,QString("control: ")+name);return widget;}
void click(QWidget* widget){const QPointF p(widget->rect().center());QMouseEvent down(QEvent::MouseButtonPress,p,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier),up(QEvent::MouseButtonRelease,p,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(widget,&down);QApplication::sendEvent(widget,&up);}
}

int main(int argc,char** argv){
    QApplication app(argc,argv);vibration::workbench::qa::configureFonts(app);app.setStyleSheet(vibration::workbench::stylesheet());
    QJsonObject report{{"schema","capture-parameters-host-v1"},{"scope","Host synthetic fixture and real Qt widgets; no physical capture hardware"}};
    try{
        check(argc==4,"service work evidence arguments");check(!QFileInfo::exists("/dev/ax7z020-capture"),"refuses physical capture hardware");
        QDir work(QString::fromLocal8Bit(argv[2])),out(QString::fromLocal8Bit(argv[3]));check(work.exists()&&out.exists(),"isolated work and evidence directories");
        vibration::workbench::CapturePage page([]{return new vibration::workbench::NumberField;});page.setStreaming(false);page.resize(1024,600);page.setServiceCommand(QString::fromLocal8Bit(argv[1]),{"--test-fixture"});page.show();
        auto* rate=find<QComboBox>(page,"captureRate");auto* samples=find<QLineEdit>(page,"captureSamples");auto* duration=find<QDoubleSpinBox>(page,"captureDuration");auto* ppr=find<QDoubleSpinBox>(page,"capturePpr");auto* start=find<QPushButton>(page,"captureStart");auto* cancel=find<QPushButton>(page,"captureCancel");auto* error=find<QLabel>(page,"captureParameterError");
        find<QLineEdit>(page,"captureDirectory")->setText(work.absolutePath());waitFor([&]{return start->isEnabled();},"fixture available");
        check(duration->minimum()==1&&duration->maximum()==40,"existing duration boundaries unchanged");
        for(int hz:{5000,10000,25000,50000}){
            rate->setCurrentIndex(rate->findData(hz));
            for(int seconds:{1,40}){
                duration->setValue(seconds);check(samples->text().toLongLong()==qint64(hz)*seconds&&start->isEnabled(),QString("rate-duration to samples %1 Hz %2 s").arg(hz).arg(seconds));
                samples->setText(QString::number(qint64(hz)*(seconds==1?40:1)));check(duration->value()==(seconds==1?40:1)&&start->isEnabled(),QString("samples to duration %1 Hz boundary %2").arg(hz).arg(seconds));
            }
        }
        int durationChanges=0,sampleChanges=0;
        QObject::connect(duration,QOverload<double>::of(&QDoubleSpinBox::valueChanged),&page,[&]{++durationChanges;});QObject::connect(samples,&QLineEdit::textChanged,&page,[&]{++sampleChanges;});
        for(int i=0;i<100;++i){duration->setValue(i%2?1:40);samples->setText(QString::number(50000*(i%2?2:3)));}
        check(durationChanges<=100&&sampleChanges<=100,"one hundred bidirectional linkage cycles have bounded signal count");
        for(const auto& raw:QStringList{"8192","0","-1","2000001","2097024","2097025","2147483648","9999999999999999999999999999","50000.0","1e5"," 50000","５００００",""}){
            samples->setText(raw);check(samples->text()==raw&&!start->isEnabled()&&error->isVisible()&&!page.property("captureParametersValid").toBool(),"reject without rounding: "+raw);
            QKeyEvent enter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);QApplication::sendEvent(samples,&enter);QMetaObject::invokeMethod(start,"clicked",Qt::DirectConnection,Q_ARG(bool,false));QApplication::processEvents();check(!page.property("captureActive").toBool(),"return and direct action cannot submit invalid count: "+raw);
        }
        samples->setText("8192");check(page.grab().save(out.filePath("hdmi-invalid-count-1024.png")),"render invalid point count");
        samples->setText("150000");check(duration->value()==3&&start->isEnabled(),"valid count recovers after invalid edits");
        check(!page.findChild<QLabel*>("captureEstimate")&&!page.findChild<QLabel*>("captureDevice"),"compact parameters omit permanent count, size and device summaries");
        check(find<QLabel>(page,"captureSource")->text().contains(QStringLiteral("合成")),"synthetic source remains visible in heading");
        bool touchBlocked=false;int phase=0;QTimer keypad;keypad.setInterval(5);
        QObject::connect(&keypad,&QTimer::timeout,&page,[&]{auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog)return;auto* entry=dialog->findChild<QLineEdit*>("numberPadEntry");if(!entry)return;if(!phase){keypad.stop();phase=1;entry->setText("8192");QMetaObject::invokeMethod(entry,"returnPressed");touchBlocked=dialog->isVisible();QApplication::processEvents();dialog->grab().save(out.filePath("hdmi-point-keypad-invalid.png"));keypad.start();}else{entry->setText("50000");QMetaObject::invokeMethod(entry,"returnPressed");}});keypad.start();click(samples);keypad.stop();check(touchBlocked&&samples->text()=="50000"&&duration->value()==1,"touch keypad rejects unsupported count on Enter then accepts exact count");
        check(page.grab().save(out.filePath("hdmi-parameters-1024.png")),"render valid acquisition grid");
        for(auto* widget:page.findChildren<QWidget*>())if(widget->isVisible()&&!widget->objectName().isEmpty())check(page.rect().contains(QRect(widget->mapTo(&page,QPoint()),widget->size())),"widget fits 1024 by 600: "+widget->objectName());
        QJsonObject submitted;
        QObject::connect(&page,&vibration::workbench::CapturePage::captureState,&page,[&](const QJsonObject& status){if(submitted.isEmpty()&&status.contains("plan"))submitted=status.value("plan").toObject();});
        click(start);waitFor([&]{return cancel->isEnabled();},"valid plan starts actual synthetic service");check(!rate->isEnabled()&&!samples->isEnabled()&&!duration->isEnabled()&&!ppr->isEnabled(),"all four parameters lock during acquisition");
        check(submitted.keys()==QStringList{"duration_seconds","output_directory","pulses_per_rev","sample_rate_hz"},"outbound acquisition plan keys unchanged");check(submitted.value("sample_rate_hz").toInt()==50000&&submitted.value("duration_seconds").toInt()==1,"exact rate/count linkage reaches existing protocol");
        click(cancel);waitFor([&]{return !page.property("captureActive").toBool();},"cancel preserves original acquisition lifecycle");check(samples->isEnabled()&&start->isEnabled(),"parameters unlock after cancellation");check(work.entryList(QDir::Files|QDir::Hidden|QDir::NoDotAndDotDot).isEmpty(),"cancel publishes no completed capture");
        report["result"]="passed";
    }catch(const std::exception& exception){report["result"]="failed";report["error"]=QString::fromUtf8(exception.what());}
    report["checks"]=checks;const auto bytes=QJsonDocument(report).toJson(QJsonDocument::Indented);std::fwrite(bytes.constData(),1,std::size_t(bytes.size()),stdout);return report.value("result")=="passed"?0:1;
}
