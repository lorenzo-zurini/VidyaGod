#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

#include <QFile>

#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>

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

    QFile * GlobalConfigFile;
    nlohmann::json GlobalConfigJSON;

    void InitClassVariables();
    nlohmann::json LoadJSON(QFile * JSONFile);
    void SaveJSON(nlohmann::json * JSONDocument, QFile * JSONFile);
};
#endif // MAINWINDOW_H
