#include "touch_keyboard.hpp"
#include <QApplication>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>
#include <utility>

namespace vibration::workbench {
namespace {
QPushButton* button(const QString& text,const QString& name,QWidget* parent){
    auto* result=new QPushButton(text,parent);result->setObjectName(name);result->setMinimumHeight(44);result->setMinimumWidth(0);
    result->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);result->setAutoDefault(false);result->setFocusPolicy(Qt::NoFocus);return result;
}
}
TouchKeyboardDialog::TouchKeyboardDialog(const QString& title,const QString& initial,Mode mode,QWidget* parent,Validator validator)
    :QDialog(parent),validator_(std::move(validator)){
    setWindowTitle(title);setObjectName(mode==Mode::Text?"workbenchTextKeyboard":"workbenchNumberPad");setModal(true);
    const auto screen=parent&&parent->screen()?parent->screen():QGuiApplication::primaryScreen();
    const auto available=screen?screen->availableGeometry().size():QSize(800,480);
    resize(qMin(mode==Mode::Text?780:420,available.width()),qMin(460,available.height()));
    auto* box=new QVBoxLayout(this);box->setContentsMargins(12,10,12,10);box->setSpacing(6);
    auto* heading=new QLabel(title.isEmpty()?QStringLiteral("输入内容"):title);heading->setProperty("role","title");box->addWidget(heading);
    entry_=new QLineEdit(initial);entry_->setObjectName(mode==Mode::Text?"touchKeyboardEntry":"numberPadEntry");entry_->setMinimumHeight(44);entry_->setMaxLength(mode==Mode::Text?200:28);entry_->selectAll();
    if(mode!=Mode::Text){entry_->setAlignment(Qt::AlignRight);entry_->setInputMethodHints(mode==Mode::Integer?Qt::ImhDigitsOnly:Qt::ImhFormattedNumbersOnly);}box->addWidget(entry_);
    error_=new QLabel;error_->setObjectName("touchKeyboardError");error_->setWordWrap(true);error_->setMinimumHeight(36);error_->setStyleSheet("color:#9a4b32;font-size:16px;");box->addWidget(error_);
    auto* keys=new QGridLayout;keys->setSpacing(5);
    const QStringList rows=mode==Mode::Text?QStringList{"1234567890","qwertyuiop","asdfghjkl","zxcvbnm_-./"}:QStringList{"789","456","123",mode==Mode::Decimal?"-0.":"-0"};
    QList<QPushButton*> letters;
    for(int row=0;row<rows.size();++row)for(int column=0;column<rows[row].size();++column){
        const auto value=QString(rows[row][column]);auto* key=button(value,"touchKey_"+value,this);key->setProperty("keyboardCharacter",value);
        if(rows[row][column].isLetter())letters.append(key);keys->addWidget(key,row,column);
        connect(key,&QPushButton::clicked,this,[this,key]{entry_->insert(key->text());error_->clear();});
    }
    box->addLayout(keys,1);
    auto* edit=new QHBoxLayout;edit->setSpacing(6);
    if(mode==Mode::Text){
        auto* shift=button(QStringLiteral("大写"),"touchKeyboardShift",this);shift->setCheckable(true);edit->addWidget(shift);
        connect(shift,&QPushButton::toggled,this,[letters,shift](bool upper){for(auto* key:letters)key->setText(upper?key->text().toUpper():key->text().toLower());shift->setText(upper?QStringLiteral("小写"):QStringLiteral("大写"));});
        auto* space=button(QStringLiteral("空格"),"touchKeyboardSpace",this);edit->addWidget(space);
        connect(space,&QPushButton::clicked,this,[this]{entry_->insert(" ");error_->clear();});
    }
    auto* erase=button(QStringLiteral("退格"),"touchKeyboardBackspace",this);edit->addWidget(erase);connect(erase,&QPushButton::clicked,this,[this]{entry_->backspace();error_->clear();});
    auto* clear=button(QStringLiteral("清空"),"touchKeyboardClear",this);edit->addWidget(clear);connect(clear,&QPushButton::clicked,this,[this]{entry_->clear();error_->clear();});box->addLayout(edit);
    auto* actions=new QHBoxLayout;actions->setSpacing(8);
    auto* cancel=button(QStringLiteral("取消"),"touchKeyboardCancel",this);auto* apply=button(QStringLiteral("确定"),"touchKeyboardApply",this);apply->setProperty("primary",true);actions->addWidget(cancel);actions->addWidget(apply);box->addLayout(actions);
    connect(cancel,&QPushButton::clicked,this,&QDialog::reject);connect(apply,&QPushButton::clicked,this,&TouchKeyboardDialog::submit);connect(entry_,&QLineEdit::returnPressed,this,&TouchKeyboardDialog::submit);
}
QString TouchKeyboardDialog::textValue()const{return entry_->text();}
void TouchKeyboardDialog::submit(){const auto reason=validator_?validator_(entry_->text()):QString();if(!reason.isEmpty()){error_->setText(reason);return;}accept();}
QString TouchKeyboardDialog::getText(QWidget* parent,const QString& title,const QString& initial,Mode mode,bool* accepted,Validator validator){
    TouchKeyboardDialog dialog(title,initial,mode,parent,std::move(validator));const bool ok=dialog.exec()==QDialog::Accepted;if(accepted)*accepted=ok;return ok?dialog.textValue():initial;
}
TouchLineEdit::TouchLineEdit(QWidget* parent):QLineEdit(parent){}
void TouchLineEdit::mousePressEvent(QMouseEvent* event){
    if(!isEnabled()||isReadOnly()||event->button()!=Qt::LeftButton){QLineEdit::mousePressEvent(event);return;}
    bool accepted=false;const auto value=TouchKeyboardDialog::getText(window(),accessibleName(),text(),mode_,&accepted,validator_);if(accepted)setText(value);event->accept();
}
}
