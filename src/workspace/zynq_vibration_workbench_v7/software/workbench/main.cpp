#include "window.hpp"
#include <QApplication>
#include <QCommandLineParser>
#include <QFont>
#include <QTimer>
#include <QPixmap>
#include <QTextCodec>
#include <QFontDatabase>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextLayout>
#include <QGlyphRun>
#include <QCryptographicHash>
#include <QSettings>
#include <QStorageInfo>
#include <QDir>
#include <QDebug>
namespace {
void configureBoardRangeSettings(){
#if defined(__arm__) && defined(__linux__)
    // The board rootfs is RAM-backed. Keep user-entered plot ranges on the
    // already mounted data card, without changing boot or acquisition files.
    const QString root=QStringLiteral("/mnt/capture-data");
    const QFileInfo rootInfo(root);const QStorageInfo volume(root);
    if(!rootInfo.isDir()||rootInfo.isSymLink()||rootInfo.canonicalFilePath()!=root||
       !volume.isValid()||volume.isReadOnly()||volume.rootPath()!=root||volume.device()!=QByteArray("/dev/mmcblk0p2")){
        qWarning("Plot range persistence unavailable: expected writable data partition");return;
    }
    QString parent=root;
    for(const auto& name:{QStringLiteral(".vibration-workbench"),QStringLiteral("ui"),QStringLiteral("VibrationWorkbench")}){
        const auto path=QDir(parent).filePath(name);const QFileInfo info(path);
        if(info.isSymLink()||(info.exists()&&(!info.isDir()||info.canonicalFilePath()!=path))||
           (!info.exists()&&!QDir(parent).mkdir(name))){qWarning("Plot range persistence unavailable: invalid settings directory");return;}
        parent=path;
    }
    const QFileInfo file(QDir(parent).filePath(QStringLiteral("WorkbenchV7.ini")));
    if(file.isSymLink()||(file.exists()&&!file.isFile())){qWarning("Plot range persistence unavailable: invalid settings file");return;}
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,QDir(root).filePath(QStringLiteral(".vibration-workbench/ui")));
#endif
}
}
int main(int argc,char** argv){
    QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8"));QApplication app(argc,argv);app.setApplicationName("vibration_workbench");
    configureBoardRangeSettings();
    // Use the host's existing Windows UI fonts for reference-matching previews.
    // Board deployments can supply the same families through their normal font
    // directory. Fonts are not extracted from Codex or copied into source bundles.
    for(const auto& fontPath:{"/mnt/c/Windows/Fonts/segoeui.ttf","/mnt/c/Windows/Fonts/seguisb.ttf","/mnt/c/Windows/Fonts/msyh.ttc","/mnt/c/Windows/Fonts/msyhbd.ttc",
        "/usr/share/fonts/truetype/workbench/segoeui.ttf","/usr/share/fonts/truetype/workbench/seguisb.ttf","/usr/share/fonts/truetype/workbench/msyh.ttc","/usr/share/fonts/truetype/workbench/msyhbd.ttc"})
        if(QFileInfo::exists(fontPath))QFontDatabase::addApplicationFont(fontPath);
    QFont font(QStringLiteral("Segoe UI,Microsoft YaHei UI,Microsoft YaHei,Noto Sans CJK SC,sans-serif"));font.setPixelSize(14);font.setWeight(QFont::Normal);font.setStyleStrategy(QFont::PreferAntialias);font.setHintingPreference(QFont::PreferDefaultHinting);app.setFont(font);
    QCommandLineParser parser;parser.addHelpOption();parser.addPositionalArgument("record","Optional CAPDDR2 file","[record]");parser.addOption({"capture","Show new acquisition page"});parser.addOption({"screenshot","Save actual Qt window and exit","png"});parser.addOption({"font-report","Write a new font-family audit file","json"});parser.addOption({"size","Window size","WxH","1024x600"});parser.addOption({"view","Initial analysis view: time, spectrum, speed, detection","view","time"});parser.addOption({"directory","Initial record directory","path"});parser.addOption({"fullscreen","Use full screen"});parser.process(app);
    if(parser.isSet("font-report")){
        QFile report(parser.value("font-report"));if(report.exists()||!report.open(QIODevice::WriteOnly|QIODevice::NewOnly))return 5;
        QTextLayout sample(QStringLiteral("振动分析 CH1 123"),app.font());sample.beginLayout();auto line=sample.createLine();line.setLineWidth(1000);sample.endLayout();
        QJsonArray families,runs;for(const auto& run:sample.glyphRuns()){if(!families.contains(run.rawFont().familyName()))families.append(run.rawFont().familyName());QJsonArray positions;for(const auto& position:run.positions())positions.append(position.x());runs.append(QJsonObject{{"family",run.rawFont().familyName()},{"name_table_sha256",QString::fromLatin1(QCryptographicHash::hash(run.rawFont().fontTable("name"),QCryptographicHash::Sha256).toHex())},{"positions_x",positions}});}
        const auto data=QJsonDocument(QJsonObject{{"sample",sample.text()},{"families",families},{"runs",runs},{"pixel_size",app.font().pixelSize()}}).toJson();if(report.write(data)!=data.size())return 5;
    }
    vibration::workbench::Window window;const auto size=parser.value("size").split('x');if(size.size()!=2||size[0].toInt()<1024||size[1].toInt()<600)return 2;window.resize(size[0].toInt(),size[1].toInt());
    const QStringList views{"time","spectrum","acceleration","detection"};const int view=views.indexOf(parser.value("view"));if(view<0)return 2;
    if(parser.isSet("directory"))window.setDirectory(parser.value("directory"));
    if(parser.isSet("fullscreen"))window.showFullScreen();else window.show();if(!parser.positionalArguments().isEmpty()){window.openCapture(parser.positionalArguments().first());window.setView(view);}if(parser.isSet("capture"))window.showAcquisition();
    if(parser.isSet("screenshot")){auto* timer=new QTimer(&app);timer->setInterval(200);QObject::connect(timer,&QTimer::timeout,&app,[&]{if(!window.isAnalyzing()){timer->stop();app.exit(window.grab().save(parser.value("screenshot"))?0:3);}});timer->start();QTimer::singleShot(60000,&app,[&]{app.exit(4);});}return app.exec();
}
