#pragma once
#include <QDialog>
#include <QLineEdit>
#include <functional>
#include <utility>

class QLabel;
class QMouseEvent;
namespace vibration::workbench {
// Self-contained Qt Widgets keyboard. Text mode works without an IME for
// English/numbers; an installed platform IME can additionally enter Chinese.
class TouchKeyboardDialog final : public QDialog {
public:
    enum class Mode { Text, Decimal, Integer };
    using Validator=std::function<QString(const QString&)>;
    TouchKeyboardDialog(const QString& title,const QString& initial,Mode mode,
                        QWidget* parent=nullptr,Validator validator={});
    QString textValue() const;
    static QString getText(QWidget* parent,const QString& title,const QString& initial,
                           Mode mode=Mode::Text,bool* accepted=nullptr,Validator validator={});
private:
    void submit();
    QLineEdit* entry_;
    QLabel* error_;
    Validator validator_;
};

// A tap opens the reusable keyboard; physical keyboard / platform IME input
// remains possible. Validators run before committing a changed field value.
class TouchLineEdit : public QLineEdit {
public:
    explicit TouchLineEdit(QWidget* parent=nullptr);
    void setKeyboardMode(TouchKeyboardDialog::Mode mode){mode_=mode;}
    void setValidator(TouchKeyboardDialog::Validator validator){validator_=std::move(validator);}
protected:
    void mousePressEvent(QMouseEvent* event) override;
private:
    TouchKeyboardDialog::Mode mode_=TouchKeyboardDialog::Mode::Text;
    TouchKeyboardDialog::Validator validator_;
};
}
