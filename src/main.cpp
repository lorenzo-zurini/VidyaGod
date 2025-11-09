#include "mainwindow.h"

#include <QApplication>
#include <QStyleFactory>
#include "mainwindow.h"

int main(int argc, char *argv[])
{
    // Enable high-DPI scaling before QApplication is created
    QApplication a(argc, argv);

    // Force a consistent Qt style
    a.setStyle(QStyleFactory::create("Breeze"));

    // Optional: set a default font to ensure consistent spacing
    a.setFont(QFont("DejaVu Sans", 10));

    MainWindow w;
    w.show();
    return a.exec();
}
