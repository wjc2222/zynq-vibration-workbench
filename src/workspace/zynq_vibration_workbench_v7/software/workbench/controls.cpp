#include "controls.hpp"
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QLineEdit>
#include <QDialog>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QPushButton>
#include <QStyleOptionComboBox>
#include <QStyledItemDelegate>
#include <QAbstractItemView>
#include <cmath>

namespace vibration::workbench {
namespace {
class TouchChoiceDelegate final:public QStyledItemDelegate {
public:using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem& option,const QModelIndex& index)const override{auto size=QStyledItemDelegate::sizeHint(option,index);size.setHeight(std::max(44,size.height()));return size;}
};
}
ChoiceField::ChoiceField(QWidget* parent):QComboBox(parent){setProperty("paintedChoiceArrow",true);auto f=font();f.setPixelSize(16);setFont(f);view()->setFont(f);setItemDelegate(new TouchChoiceDelegate(this));}
void TouchButton::paintEvent(QPaintEvent* event){
    QPushButton::paintEvent(event);
    QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
    const QColor border=!isEnabled()?QColor("#b8c1cd"):(property("primary").toBool()||isChecked())?QColor("#285aaf"):QColor("#8390a1");
    painter.setPen(QPen(border,1.3));painter.setBrush(Qt::NoBrush);painter.drawRoundedRect(QRectF(rect()).adjusted(1,1,-1,-1),5,5);
}
void ChoiceField::paintEvent(QPaintEvent* event){
    QComboBox::paintEvent(event);
    QStyleOptionComboBox option;initStyleOption(&option);
    const QRect arrow=style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxArrow,this);
    const QPointF center=arrow.center();
    QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(isEnabled()?QColor("#687586"):QColor("#aab4c1"),1.6,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
    QPainterPath chevron;chevron.moveTo(center.x()-4,center.y()-2);chevron.lineTo(center.x(),center.y()+2);chevron.lineTo(center.x()+4,center.y()-2);painter.drawPath(chevron);
}
QIcon icon(Icon type,const QColor& color){
    QPixmap map(48,48);map.fill(Qt::transparent);QPainter p(&map);p.setRenderHint(QPainter::Antialiasing);p.scale(2,2);p.setPen(QPen(color,1.6,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.setBrush(Qt::NoBrush);
    const auto line=[&](double x,double y,double xx,double yy){p.drawLine(QPointF(x,y),QPointF(xx,yy));};
    switch(type){
    case Icon::Home: {QPainterPath t;t.moveTo(4,10);t.lineTo(12,4);t.lineTo(20,10);t.lineTo(20,20);t.lineTo(5,20);t.lineTo(5,10);p.drawPath(t);line(10,20,10,14);line(10,14,14,14);line(14,14,14,20);break;}
    case Icon::Folder: {QPainterPath t;t.moveTo(3,6);t.lineTo(9,6);t.lineTo(11,9);t.lineTo(21,9);t.lineTo(21,20);t.lineTo(3,20);t.closeSubpath();p.drawPath(t);break;}
    case Icon::Add:line(12,5,12,19);line(5,12,19,12);break;
    case Icon::Settings:for(int y:{6,12,18}){line(4,y,20,y);p.setBrush(Qt::white);p.drawEllipse(QPointF(y==12?9:15,y),2.5,2.5);}break;
    case Icon::Panel:p.drawRoundedRect(QRectF(3,4,18,16),2,2);line(9,4,9,20);break;
    case Icon::Export:line(12,4,12,16);line(8,8,12,4);line(12,4,16,8);line(4,14,4,20);line(4,20,20,20);line(20,20,20,14);break;
    case Icon::Wave:{QPainterPath t;t.moveTo(2,12);t.lineTo(6,12);t.lineTo(9,4);t.lineTo(13,20);t.lineTo(17,8);t.lineTo(20,12);t.lineTo(22,12);p.drawPath(t);break;}
    case Icon::Spectrum:for(int i=0;i<7;++i)line(3+3*i,20,3+3*i,20-(i==2?15:i==4?10:3));break;
    case Icon::Speed:p.drawArc(QRectF(3,3,18,18),0,180*16);line(12,12,17,7);p.drawEllipse(QPointF(12,12),1,1);line(3,12,5,12);line(19,12,21,12);break;
    case Icon::Detection:p.drawEllipse(QRectF(4,4,16,16));line(8,12,11,15);line(11,15,17,9);break;
    case Icon::Close:line(6,6,18,18);line(6,18,18,6);break;
    case Icon::Refresh:p.drawArc(QRectF(5,5,14,14),40*16,280*16);line(19,5,19,11);line(19,11,14,11);break;
    }
    p.end();map.setDevicePixelRatio(2);return QIcon(map);
}
FileTitle::FileTitle(QWidget* parent):QLabel(parent){setMinimumWidth(0);setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);setTextFormat(Qt::PlainText);}
void FileTitle::setFullText(const QString& text){full_=text;setToolTip(text);fit();}
void FileTitle::fit(){QLabel::setText(fontMetrics().elidedText(full_,Qt::ElideMiddle,qMax(0,contentsRect().width()-2*margin())));}
void FileTitle::resizeEvent(QResizeEvent* e){QLabel::resizeEvent(e);fit();}
void FileTitle::changeEvent(QEvent* e){QLabel::changeEvent(e);if(e->type()==QEvent::FontChange||e->type()==QEvent::StyleChange)fit();}
NumberField::NumberField(QWidget* parent):QDoubleSpinBox(parent){setButtonSymbols(QAbstractSpinBox::NoButtons);setMinimumHeight(44);lineEdit()->installEventFilter(this);}
bool NumberField::eventFilter(QObject* object,QEvent* e){if(object==lineEdit()&&isEnabled()&&e->type()==QEvent::MouseButtonPress&&static_cast<QMouseEvent*>(e)->button()==Qt::LeftButton){enterNumber();return true;}return QDoubleSpinBox::eventFilter(object,e);}
void NumberField::enterNumber(){
    QDialog dialog(window());dialog.setWindowTitle(accessibleName());dialog.setObjectName("workbenchNumberPad");dialog.setFixedSize(384,480);
    auto* box=new QVBoxLayout(&dialog);box->setContentsMargins(20,18,20,18);box->setSpacing(10);
    auto* title=new QLabel(accessibleName().isEmpty()?QStringLiteral("输入数值"):accessibleName());title->setProperty("role","title");box->addWidget(title);
    auto* entry=new QLineEdit(QString::number(value(),'f',decimals()));entry->setObjectName("numberPadEntry");entry->setAlignment(Qt::AlignRight);entry->selectAll();entry->setMinimumHeight(50);box->addWidget(entry);
    auto* error=new QLabel;error->setWordWrap(true);error->setStyleSheet("color:#a15d45;font-size:16px;");box->addWidget(error);
    auto* grid=new QGridLayout;grid->setSpacing(8);bool replace=true;const QStringList keys{"7","8","9","4","5","6","1","2","3","-","0","."};
    connect(entry,&QLineEdit::textEdited,&dialog,[&replace]{replace=false;});
    for(int i=0;i<keys.size();++i){auto* key=new QPushButton(keys[i]);key->setMinimumHeight(48);grid->addWidget(key,i/3,i%3);connect(key,&QPushButton::clicked,&dialog,[&,text=keys[i]]{if(replace){entry->clear();replace=false;}if(entry->text().size()<28)entry->setText(entry->text()+text);error->clear();});}box->addLayout(grid);
    auto* actions=new QHBoxLayout;auto* erase=new QPushButton(QStringLiteral("退格"));auto* cancel=new QPushButton(QStringLiteral("取消"));auto* apply=new QPushButton(QStringLiteral("确定"));apply->setProperty("primary",true);actions->addWidget(erase);actions->addWidget(cancel);actions->addWidget(apply);box->addLayout(actions);
    connect(erase,&QPushButton::clicked,&dialog,[&]{entry->backspace();replace=false;});connect(cancel,&QPushButton::clicked,&dialog,&QDialog::reject);
    connect(apply,&QPushButton::clicked,&dialog,[&]{bool ok=false;const auto n=entry->text().toDouble(&ok);const double factor=std::pow(10.,decimals());if(!ok||!std::isfinite(n)||n<minimum()||n>maximum()||std::abs(n*factor-std::round(n*factor))>1e-7){error->setText(QStringLiteral("请输入 %1–%2 范围内的数值").arg(minimum()).arg(maximum()));return;}setValue(n);dialog.accept();});dialog.exec();
}
QString stylesheet(){return QStringLiteral(
    "QWidget{font-size:16px;}QMainWindow,QDialog,QWidget#workbenchCentral,QStackedWidget#newPages{background:white;color:#20252b;}QLabel{color:#344255;background:transparent;}"
    "QDialog{border:1px solid #8d9bad;border-radius:6px;}"
    "QFrame#workbenchTabs{background:white;border:0;}QStackedWidget#newPages{border:1px solid #c5cbd3;}"
    "QLabel[role=title]{font-size:22px;font-weight:600;color:#20252b;}QLabel[role=muted]{font-size:16px;color:#56616e;}"
    "QPushButton{min-height:44px;border:1px solid #8d9bad;border-radius:5px;background:white;padding:0 14px;color:#344255;font-size:16px;}"
    "QPushButton:hover{background:#f0f5fb;border-color:#9fb3cd;}QPushButton:pressed{background:#e2ebf6;}QPushButton:focus{border-color:#3974bd;}"
    "QPushButton:disabled{color:#9ca6b3;background:#f7f9fb;border-color:#e2e7ee;}"
    "QPushButton[primary=true]{background:#3569c8;color:white;border-color:#3569c8;}QPushButton[primary=true]:hover{background:#285ab2;}QPushButton[primary=true]:disabled{background:#bbc9e2;color:white;border-color:#bbc9e2;}"
    "QPushButton[brand=true]{border:1px solid #8d9bad;padding:0 10px;background:transparent;font-size:19px;font-weight:600;color:#263345;}"
    "QPushButton[tab=true]{border:1px solid #c5cbd3;border-radius:6px;background:white;padding:0 14px;color:#303842;font-size:16px;}"
    "QPushButton[tab=true]:checked{color:#1f55a9;border-color:#3569c8;background:#edf3ff;font-weight:600;}QPushButton:checked{background:#e7eff9;border-color:#96b2d6;}"
    "QLineEdit,QComboBox,QDoubleSpinBox{min-height:44px;padding:0 12px;border:1px solid #8d9bad;border-radius:5px;background:#ffffff;color:#344255;font-size:16px;}QComboBox::drop-down{border:0;width:28px;}"
    "QComboBox[paintedChoiceArrow=true]::down-arrow{image:none;}"
    "QComboBox[labelRequired=true]{border:2px solid #b97b24;background:#fff8eb;color:#83551c;}"
    "QComboBox QAbstractItemView{font-size:16px;background:white;color:#263345;selection-background-color:#e7effb;selection-color:#263345;}QComboBox QAbstractItemView::item{min-height:44px;}"
    "QMenu{font-size:16px;background:white;border:1px solid #8390a1;}QMenu::item{min-height:44px;padding:0 18px;}"
    "QListWidget{background:transparent;border:0;outline:0;}QListWidget::item{min-height:48px;padding:0 10px;border:0;border-radius:4px;color:#536174;}QListWidget::item:selected{background:#e3edf9;color:#245eaa;}"
    "QCheckBox{min-height:44px;spacing:8px;}QCheckBox::indicator{width:20px;height:20px;}"
    "QTextBrowser{border:0;background:white;color:#5d5d5d;}QScrollArea{border:0;background:white;}QScrollArea>QWidget>QWidget{background:white;}"
    "QTabWidget::pane{border:0;}QTabBar::tab{min-height:44px;padding:0 20px;background:#fff;color:#344255;}QTabBar::tab:selected{color:#245eaa;background:#edf3fb;}"
    "QScrollBar:vertical{width:16px;background:#f5f7fa;}QScrollBar::handle:vertical{background:#b7c2d0;min-height:44px;border-radius:5px;margin:3px;}QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}"
);}
}
