#ifndef MAIN_H
#define MAIN_H

#endif // MAIN_H
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QMessageBox>
#include <QApplication>
#include <QStyleFactory>
#include <QFile>


#include "nlohmann/json.hpp"
#include "mainwindow.h"


int main(int argc, char *argv[]);
bool CheckExecutableDependencies();
bool InitializeGlobalConfigJSON(nlohmann::ordered_json * GlobalConfigJSON, QDir * AppDataDir);
