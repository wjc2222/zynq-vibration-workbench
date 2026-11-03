#include "touch_directory_dialog.hpp"
#include "touch_keyboard.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QVBoxLayout>
#ifdef __linux__
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace vibration::workbench {
namespace {
const QString captureRoot=QStringLiteral("/mnt/capture-data/vibration");
QPushButton* action(const QString& text,const char* name){auto* result=new QPushButton(text);result->setObjectName(name);result->setMinimumHeight(44);result->setAutoDefault(false);return result;}
QString createFolderDurably(const QString& parent,const QString& name){
#ifdef __linux__
    struct DirectoryFd {int fd;~DirectoryFd(){if(fd>=0)::close(fd);}};
    DirectoryFd directory{::open(QFile::encodeName(parent).constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW)};
    if(directory.fd<0||::mkdirat(directory.fd,QFile::encodeName(name).constData(),0700))return QStringLiteral("无法新建文件夹，请检查权限、存储空间或重名情况。");
    DirectoryFd created{::openat(directory.fd,QFile::encodeName(name).constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW)};
    const bool childSynced=created.fd>=0&&::fsync(created.fd)==0;
    const bool parentSynced=::fsync(directory.fd)==0;
    if(!childSynced||!parentSynced)return QStringLiteral("文件夹已建立，但存储同步失败。请检查 SD 卡后重试。");
#else
    if(!QDir(parent).mkdir(name))return QStringLiteral("无法新建文件夹，请检查权限、存储空间或重名情况。");
#endif
    return {};
}
}
TouchDirectoryDialog::TouchDirectoryDialog(const QString& initialDirectory,QWidget* parent,const QString& allowedRoot):QDialog(parent){
    setObjectName("touchDirectoryDialog");setWindowTitle(QStringLiteral("选择保存目录"));setModal(true);
    const auto screen=parent&&parent->screen()?parent->screen():QGuiApplication::primaryScreen();const auto size=screen?screen->availableGeometry().size():QSize(800,480);resize(qMin(780,size.width()),qMin(460,size.height()));
    auto* box=new QVBoxLayout(this);box->setContentsMargins(12,10,12,10);box->setSpacing(8);
    auto* title=new QLabel(QStringLiteral("选择保存目录"));title->setProperty("role","title");box->addWidget(title);
    path_=new QLabel;path_->setObjectName("touchDirectoryPath");path_->setTextFormat(Qt::PlainText);path_->setMinimumWidth(0);path_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);path_->setMinimumHeight(44);box->addWidget(path_);
    auto* navigation=new QHBoxLayout;up_=action(QStringLiteral("上一级"),"touchDirectoryUp");auto* home=action(QStringLiteral("采集根目录"),"touchDirectoryRoot");create_=action(QStringLiteral("新建文件夹"),"touchDirectoryNew");navigation->addWidget(up_);navigation->addWidget(home);navigation->addWidget(create_);box->addLayout(navigation);
    list_=new QListWidget;list_->setObjectName("touchDirectoryList");list_->setEditTriggers(QAbstractItemView::NoEditTriggers);list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);list_->setTextElideMode(Qt::ElideMiddle);box->addWidget(list_,1);
    error_=new QLabel;error_->setObjectName("touchDirectoryError");error_->setWordWrap(true);error_->setMinimumHeight(36);error_->setStyleSheet("color:#9a4b32;font-size:16px;");box->addWidget(error_);
    auto* actions=new QHBoxLayout;auto* cancel=action(QStringLiteral("取消"),"touchDirectoryCancel");choose_=action(QStringLiteral("选定此目录"),"touchDirectoryChoose");choose_->setProperty("primary",true);actions->addWidget(cancel);actions->addWidget(choose_);box->addLayout(actions);
    connect(cancel,&QPushButton::clicked,this,&QDialog::reject);connect(choose_,&QPushButton::clicked,this,&TouchDirectoryDialog::choose);connect(create_,&QPushButton::clicked,this,&TouchDirectoryDialog::newFolder);
    connect(up_,&QPushButton::clicked,this,[this]{QDir directory(current_);if(directory.cdUp())navigate(directory.absolutePath());});
    connect(home,&QPushButton::clicked,this,[this]{navigate(allowed(QFileInfo(captureRoot).canonicalFilePath())?captureRoot:root_);});
    connect(list_,&QListWidget::itemClicked,this,[this](QListWidgetItem* item){navigate(item->data(Qt::UserRole).toString());});
    QFileInfo rootInfo(allowedRoot);if(rootInfo.isAbsolute()&&rootInfo.isDir()&&!rootInfo.isSymLink())root_=rootInfo.canonicalFilePath();
    if(root_.isEmpty()){path_->setText(allowedRoot);up_->setEnabled(false);home->setEnabled(false);choose_->setEnabled(false);create_->setEnabled(false);error_->setText(QStringLiteral("数据目录尚未就绪。请先确认数据分区已挂载，然后重新选择。"));return;}
    const auto initial=QFileInfo(initialDirectory).canonicalFilePath();navigate(allowed(initial)?initial:root_);
    if(!allowed(initial))error_->setText(QStringLiteral("已显示数据目录。请选择已有文件夹，或点击“新建文件夹”输入名称。"));
}
bool TouchDirectoryDialog::allowed(const QString& path)const{return !root_.isEmpty()&&!path.isEmpty()&&(path==root_||path.startsWith(root_+"/"));}
void TouchDirectoryDialog::navigate(const QString& directory){
    QFileInfo info(directory);const auto canonical=info.canonicalFilePath();if(!info.isDir()||!allowed(canonical)){error_->setText(QStringLiteral("只能选择数据目录内的现有文件夹。"));return;}
    current_=canonical;path_->setText(path_->fontMetrics().elidedText(current_,Qt::ElideMiddle,qMax(100,width()-28)));path_->setToolTip(current_);error_->clear();list_->clear();
    const auto entries=QDir(current_).entryInfoList(QDir::Dirs|QDir::NoDotAndDotDot|QDir::NoSymLinks,QDir::Name|QDir::IgnoreCase);
    for(const auto& entry:entries){auto* item=new QListWidgetItem(entry.fileName(),list_);item->setData(Qt::UserRole,entry.absoluteFilePath());item->setToolTip(entry.absoluteFilePath());item->setSizeHint(QSize(0,48));}
    up_->setEnabled(current_!=root_);choose_->setEnabled(info.isReadable());create_->setEnabled(info.isWritable());
    if(entries.isEmpty())error_->setText(QStringLiteral("此目录下没有子文件夹，可直接选定或新建文件夹。"));
}
QString TouchDirectoryDialog::folderNameError(const QString& parentDirectory,const QString& name){
    if(name.isEmpty()||name.trimmed().isEmpty())return QStringLiteral("请输入文件夹名称。");
    if(name!=name.trimmed()||name.endsWith('.')||name=="."||name=="..")return QStringLiteral("名称不能为点号，或以空格、点号结尾。");
    if(name.toUtf8().size()>200)return QStringLiteral("名称过长，请缩短至 200 个字节以内。");
    const QString invalid=QStringLiteral("/\\:*?\"<>|");for(const auto c:name)if(c.unicode()<32||c.unicode()==127||invalid.contains(c))return QStringLiteral("名称不能包含路径分隔符或 / \\ : * ? \" < > | 等字符。");
    static const QRegularExpression reserved("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$",QRegularExpression::CaseInsensitiveOption);if(reserved.match(name).hasMatch())return QStringLiteral("请使用其他文件夹名称。");
    QFileInfo parent(parentDirectory);if(!parent.isAbsolute()||!parent.isDir()||parent.canonicalFilePath().isEmpty())return QStringLiteral("上级目录已不存在，请重新选择。");
    const QFileInfo target(QDir(parentDirectory).filePath(name));if(target.exists()||target.isSymLink())return QStringLiteral("同名文件或文件夹已存在，请换一个名称。");return {};
}
void TouchDirectoryDialog::newFolder(){
    if(!allowed(current_))return;
    const auto parent=current_;bool accepted=false;
    const auto name=TouchKeyboardDialog::getText(this,QStringLiteral("新建文件夹 · 输入名称"),{},TouchKeyboardDialog::Mode::Text,&accepted,[parent](const QString& value){return folderNameError(parent,value);});
    if(!accepted)return;
    const auto reason=folderNameError(parent,name);if(!reason.isEmpty()){error_->setText(reason);return;}
    if(!allowed(QFileInfo(parent).canonicalFilePath())||QFileInfo(parent).canonicalFilePath()!=parent){error_->setText(QStringLiteral("保存目录已变化，请重新选择。"));return;}
    const auto creationError=createFolderDurably(parent,name);if(!creationError.isEmpty()){error_->setText(creationError);return;}
    navigate(QDir(parent).filePath(name));
}
void TouchDirectoryDialog::choose(){
    QFileInfo info(current_);if(!allowed(current_)||!info.isDir()||!info.isReadable()||info.canonicalFilePath()!=current_){error_->setText(QStringLiteral("目录已变化或无法访问，请重新选择。"));return;}selected_=current_;accept();
}
QString TouchDirectoryDialog::getExistingDirectory(QWidget* parent,const QString& initialDirectory,const QString& allowedRoot){TouchDirectoryDialog dialog(initialDirectory,parent,allowedRoot);return dialog.exec()==QDialog::Accepted?dialog.selectedDirectory():QString();}
}
