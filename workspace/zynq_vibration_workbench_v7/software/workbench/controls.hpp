#pragma once
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
namespace vibration::workbench {
enum class Icon {Home, Folder, Add, Settings, Panel, Export, Wave, Spectrum, Speed, Detection, Close, Refresh};
QIcon icon(Icon type,const QColor& color=QColor("#767779"));
class ChoiceField final : public QComboBox {
public:
    explicit ChoiceField(QWidget* parent=nullptr);
protected:
    void paintEvent(QPaintEvent* event) override;
};
class FileTitle final : public QLabel {
public:
    explicit FileTitle(QWidget* parent=nullptr);
    void setFullText(const QString& text);
protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
private:
    void fit();
    QString full_;
};
class NumberField final : public QDoubleSpinBox {
public:
    explicit NumberField(QWidget* parent=nullptr);
protected:
    bool eventFilter(QObject* object,QEvent* event) override;
private:
    void enterNumber();
};
QString stylesheet();
class TouchButton final : public QPushButton {
public:
    explicit TouchButton(const QString& text,QWidget* parent=nullptr):QPushButton(text,parent){}
protected:
    void paintEvent(QPaintEvent*) override;
};
}
