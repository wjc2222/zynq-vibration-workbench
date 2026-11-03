#pragma once
#include <QDialog>
class QLabel;
class QListWidget;
class QPushButton;
namespace vibration::workbench {
// Browsing is read-only. Only the explicit new-folder confirmation writes a
// single child directory; there are deliberately no rename/delete operations.
class TouchDirectoryDialog final : public QDialog {
public:
    explicit TouchDirectoryDialog(const QString& initialDirectory,QWidget* parent=nullptr,
                                  const QString& allowedRoot=QStringLiteral("/mnt/capture-data"));
    QString selectedDirectory() const{return selected_;}
    QString currentDirectory() const{return current_;}
    static QString getExistingDirectory(QWidget* parent,const QString& initialDirectory,
                                        const QString& allowedRoot=QStringLiteral("/mnt/capture-data"));
    static QString folderNameError(const QString& parentDirectory,const QString& name);
private:
    void navigate(const QString& directory);
    void newFolder();
    void choose();
    bool allowed(const QString& canonicalPath) const;
    QString root_,current_,selected_;
    QLabel *path_,*error_;
    QListWidget* list_;
    QPushButton *up_,*choose_,*create_;
};
}
