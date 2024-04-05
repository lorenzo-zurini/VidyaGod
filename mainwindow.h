#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

#include <QFile>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <QStandardItemModel>

#include <QFileDialog>

QT_BEGIN_NAMESPACE
namespace Ui
{
    class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void on_AddGameButton_clicked();

private:
    Ui::MainWindow *ui;

    QJsonDocument * LibraryJSONDocument;
    QStandardItemModel * LibraryModel;


    void InitClassVariables();
    QJsonDocument * LoadJSON(QFile*);
    void SaveJSON(QJsonDocument*, QFile*);
};
#endif // MAINWINDOW_H
