#include "touch_directory_dialog.hpp"
#include "touch_keyboard.hpp"
#include "controls.hpp"
#include "test_fonts.hpp"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <stdexcept>

using namespace vibration::workbench;
namespace {
QJsonArray checks;
void check(bool value,const QString& name){checks.append(QJsonObject{{"check",name},{"passed",value}});if(!value)throw std::runtime_error(name.toStdString());}
template<class T>T* find(QObject& owner,const QString& name){auto* value=owner.findChild<T*>(name);if(!value)throw std::runtime_error(("missing "+name).toStdString());return value;}
void tap(QPushButton* value){const QPointF p=value->rect().center();QMouseEvent down(QEvent::MouseButtonPress,p,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier),up(QEvent::MouseButtonRelease,p,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(value,&down);QApplication::sendEvent(value,&up);}
void tapItem(QListWidget* list,QListWidgetItem* item){const QPointF p=list->visualItemRect(item).center();QMouseEvent down(QEvent::MouseButtonPress,p,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier),up(QEvent::MouseButtonRelease,p,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(list->viewport(),&down);QApplication::sendEvent(list->viewport(),&up);}
void key(QObject& owner,const QString& name){tap(find<QPushButton>(owner,name));}
void fits(QWidget& dialog,const QString& name){QApplication::processEvents();check(dialog.width()<=800&&dialog.height()<=480,name+" dialog fits 800x480");for(auto* button:dialog.findChildren<QPushButton*>())if(button->isVisible())check(button->height()>=44&&dialog.rect().contains(QRect(button->mapTo(&dialog,QPoint()),button->size())),name+" touch target "+button->objectName());}
}
int main(int argc,char** argv){
    QApplication app(argc,argv);qa::configureFonts(app);app.setStyleSheet(stylesheet());QJsonObject report{{"scope","Host Qt touch controls with isolated temporary directories; no physical board or screen"}};
    try{
        QTemporaryDir root;check(root.isValid(),"isolated temporary root");const auto parent=QDir(root.path()).canonicalPath();QDir work(parent);check(work.mkdir("existing"),"existing raw-data folder fixture");QFile raw(work.filePath("existing/original.BIN"));check(raw.open(QIODevice::WriteOnly)&&raw.write("untouched raw fixture")==21,"raw-data sentinel fixture");raw.close();
        if(argc==2&&QString::fromLocal8Bit(argv[1])=="--sync-failure"){
            qputenv("AX7_QA_SYNC_ROOT",QFile::encodeName(parent));qputenv("AX7_QA_SYNC_FAIL","parent");
            TouchDirectoryDialog failed(parent,nullptr,parent);
            QTimer::singleShot(0,[&]{if(auto* modal=QApplication::activeModalWidget()){find<QLineEdit>(*modal,"touchKeyboardEntry")->setText("Sync_failure");key(*modal,"touchKeyboardApply");}});
            key(failed,"touchDirectoryNew");
            check(QDir(work.filePath("Sync_failure")).exists(),"sync failure preserves the created directory for inspection");
            check(failed.currentDirectory()==parent&&failed.selectedDirectory().isEmpty(),"sync failure does not enter or select the unconfirmed directory");
            check(find<QLabel>(failed,"touchDirectoryError")->text().contains(QStringLiteral("同步失败")),"parent fsync failure remains visible to the user");
            check(raw.open(QIODevice::ReadOnly)&&raw.readAll()=="untouched raw fixture","sync failure preserves existing raw data");raw.close();
            report["result"]="passed";report["checks"]=checks;const auto bytes=QJsonDocument(report).toJson(QJsonDocument::Indented);std::fwrite(bytes.constData(),1,std::size_t(bytes.size()),stdout);return 0;
        }
        const auto before=work.entryList(QDir::AllEntries|QDir::Hidden|QDir::NoDotAndDotDot);
        {TouchDirectoryDialog dialog(work.filePath("missing"),nullptr,parent);dialog.show();fits(dialog,"directory");check(dialog.currentDirectory()==parent,"missing initial directory falls back inside allowed root");check(!find<QPushButton>(dialog,"touchDirectoryUp")->isEnabled(),"cannot navigate above allowed root");key(dialog,"touchDirectoryCancel");check(dialog.selectedDirectory().isEmpty(),"cancel returns no selection");}
        check(work.entryList(QDir::AllEntries|QDir::Hidden|QDir::NoDotAndDotDot)==before,"construct browse and cancel create nothing");
        for(const auto& name:QStringList{"",".","..","../escape","/escape","a/b","a\\b","a:b","a?b","name ","name.","CON","existing",QString(201,'a'),QString("bad\nname")})check(!TouchDirectoryDialog::folderNameError(parent,name).isEmpty(),"reject invalid/duplicate name "+name);
        check(TouchDirectoryDialog::folderNameError(parent,QStringLiteral("试验01")).isEmpty(),"Chinese name accepted without platform-specific conversion");
        TouchDirectoryDialog dialog(parent,nullptr,parent);dialog.show();int stages=0;bool duplicateBlocked=false,cancelled=false;QTimer timer;timer.setInterval(5);
        QObject::connect(&timer,&QTimer::timeout,&dialog,[&]{auto* keyboard=dynamic_cast<TouchKeyboardDialog*>(QApplication::activeModalWidget());if(!keyboard)return;timer.stop();auto* entry=find<QLineEdit>(*keyboard,"touchKeyboardEntry");
            if(stages==0){fits(*keyboard,"text keyboard");entry->setText("existing");key(*keyboard,"touchKeyboardApply");duplicateBlocked=keyboard->isVisible()&&!find<QLabel>(*keyboard,"touchKeyboardError")->text().isEmpty();key(*keyboard,"touchKeyboardCancel");cancelled=true;}
            else {key(*keyboard,"touchKeyboardShift");key(*keyboard,"touchKey_r");key(*keyboard,"touchKeyboardShift");key(*keyboard,"touchKey_u");key(*keyboard,"touchKey_n");key(*keyboard,"touchKey__");key(*keyboard,"touchKey_2");key(*keyboard,"touchKey_0");key(*keyboard,"touchKey_2");key(*keyboard,"touchKey_6");key(*keyboard,"touchKeyboardApply");}
        });
        timer.start();key(dialog,"touchDirectoryNew");check(cancelled&&duplicateBlocked&&work.entryList(QDir::AllEntries|QDir::Hidden|QDir::NoDotAndDotDot)==before,"duplicate remains in keyboard and cancel writes nothing");
        ++stages;timer.start();key(dialog,"touchDirectoryNew");check(QDir(work.filePath("Run_2026")).exists()&&dialog.currentDirectory()==QDir(work.filePath("Run_2026")).canonicalPath(),"touch keys create exact mixed-case custom folder and enter it");
        key(dialog,"touchDirectoryUp");check(dialog.currentDirectory()==parent,"touch up returns to allowed root");auto* list=find<QListWidget>(dialog,"touchDirectoryList");QListWidgetItem* existing=nullptr;for(int i=0;i<list->count();++i)if(list->item(i)->text()=="existing")existing=list->item(i);check(existing,"existing directory is listed");tapItem(list,existing);check(dialog.currentDirectory()==work.filePath("existing"),"single touch item navigates existing directory");key(dialog,"touchDirectoryChoose");check(dialog.result()==QDialog::Accepted&&dialog.selectedDirectory()==work.filePath("existing"),"selection commits only confirmed directory");
        {TouchDirectoryDialog absent(work.filePath("unmounted/vibration"),nullptr,work.filePath("unmounted"));check(!find<QPushButton>(absent,"touchDirectoryChoose")->isEnabled()&&!find<QPushButton>(absent,"touchDirectoryNew")->isEnabled()&&!QDir(work.filePath("unmounted")).exists(),"missing data root never creates mount or system directories");}
        {TouchDirectoryDialog outside(QDir::tempPath(),nullptr,parent);check(outside.currentDirectory()==parent,"outside initial directory is constrained to allowed root");}
        {QTemporaryDir outside;check(outside.isValid()&&QFile::link(outside.path(),work.filePath("outside-link")),"isolated symlink fixture");TouchDirectoryDialog linked(work.filePath("outside-link"),nullptr,parent);check(linked.currentDirectory()==parent,"initial symlink cannot escape data root");auto* listing=find<QListWidget>(linked,"touchDirectoryList");bool listed=false;for(int i=0;i<listing->count();++i)listed|=listing->item(i)->text()=="outside-link";check(!listed,"directory list excludes symlink navigation");check(!TouchDirectoryDialog::folderNameError(parent,"outside-link").isEmpty(),"new-folder name cannot replace a symlink");}
        {const auto chinese=QStringLiteral("试验01");QTimer::singleShot(0,[&]{if(auto* modal=QApplication::activeModalWidget()){find<QLineEdit>(*modal,"touchKeyboardEntry")->setText(chinese);key(*modal,"touchKeyboardApply");}});TouchDirectoryDialog unicode(parent,nullptr,parent);key(unicode,"touchDirectoryNew");check(QDir(work.filePath(chinese)).exists(),"Chinese text from platform input creates exact Unicode directory");}
        {TouchKeyboardDialog numeric("Gain","1",TouchKeyboardDialog::Mode::Decimal,nullptr,[](const QString& value){bool ok=false;double gain=value.toDouble(&ok);return !ok||gain==0?QString("gain cannot be zero"):QString();});numeric.show();fits(numeric,"numeric keyboard");key(numeric,"touchKey_0");key(numeric,"touchKeyboardApply");check(numeric.isVisible()&&!find<QLabel>(numeric,"touchKeyboardError")->text().isEmpty(),"numeric validator rejects zero");key(numeric,"touchKeyboardClear");key(numeric,"touchKey_-");key(numeric,"touchKey_2");key(numeric,"touchKey_.");key(numeric,"touchKey_5");key(numeric,"touchKeyboardApply");check(numeric.result()==QDialog::Accepted&&numeric.textValue()=="-2.5","numeric touch keys preserve signed decimal");}
        bool accepted=true;QTimer::singleShot(0,[]{if(auto* modal=QApplication::activeModalWidget())key(*modal,"touchKeyboardCancel");});check(TouchKeyboardDialog::getText(nullptr,"Cancel","original",TouchKeyboardDialog::Mode::Text,&accepted)=="original"&&!accepted,"keyboard cancel preserves original field");
        check(raw.open(QIODevice::ReadOnly)&&raw.readAll()=="untouched raw fixture","existing raw data unchanged");raw.close();
        if(argc>1){QDir evidence(QString::fromLocal8Bit(argv[1]));TouchDirectoryDialog shot(parent,nullptr,parent);shot.show();QApplication::processEvents();check(shot.grab().save(evidence.filePath("touch-directory-800.png")),"directory screenshot");TouchKeyboardDialog keyboard(QStringLiteral("新建文件夹 · 输入名称"),"Run_2026",TouchKeyboardDialog::Mode::Text);keyboard.show();QApplication::processEvents();check(keyboard.grab().save(evidence.filePath("touch-keyboard-800.png")),"keyboard screenshot");}
        report["result"]="passed";
    }catch(const std::exception& error){report["result"]="failed";report["error"]=QString::fromUtf8(error.what());}
    report["checks"]=checks;const auto bytes=QJsonDocument(report).toJson(QJsonDocument::Indented);std::fwrite(bytes.constData(),1,std::size_t(bytes.size()),stdout);return report["result"]=="passed"?0:1;
}
