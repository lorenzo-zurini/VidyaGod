#include "theme.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QPalette>
#include <QStyleFactory>
#include <QWidget>

namespace Theme
{

void Apply(QApplication &App)
{
    App.setStyle(QStyleFactory::create("Fusion"));
    QPalette P;
    const QColor Window(0x1b, 0x20, 0x26), Base(0x23, 0x29, 0x31), Text(0xd8, 0xda, 0xde), Dim(0x8f, 0x98, 0xa0), Off(0x5d, 0x65, 0x6e);
    P.setColor(QPalette::Window, Window);
    P.setColor(QPalette::WindowText, Text);
    P.setColor(QPalette::Base, Base);
    P.setColor(QPalette::AlternateBase, QColor(0x28, 0x2f, 0x38));
    P.setColor(QPalette::ToolTipBase, QColor(0x2a, 0x30, 0x38));
    P.setColor(QPalette::ToolTipText, QColor(0xe6, 0xe8, 0xea));
    P.setColor(QPalette::PlaceholderText, Dim);
    P.setColor(QPalette::Text, Text);
    P.setColor(QPalette::Button, QColor(0x2a, 0x30, 0x38));
    P.setColor(QPalette::ButtonText, Text);
    P.setColor(QPalette::BrightText, Qt::white);
    P.setColor(QPalette::Light, QColor(0x39, 0x41, 0x4a));
    P.setColor(QPalette::Midlight, QColor(0x32, 0x39, 0x41));
    P.setColor(QPalette::Mid, QColor(0x2c, 0x33, 0x3b));
    P.setColor(QPalette::Dark, QColor(0x12, 0x16, 0x1a));
    P.setColor(QPalette::Shadow, QColor(0x0b, 0x0d, 0x10));
    P.setColor(QPalette::Highlight, QColor(0x3d, 0x7f, 0xc4));
    P.setColor(QPalette::HighlightedText, Qt::white);
    P.setColor(QPalette::Link, QColor(0x6f, 0xb0, 0xff));
    P.setColor(QPalette::LinkVisited, QColor(0x9a, 0x8f, 0xe0));
    for (const auto R : { QPalette::WindowText, QPalette::Text, QPalette::ButtonText })
        P.setColor(QPalette::Disabled, R, Off);
    P.setColor(QPalette::Disabled, QPalette::Button, QColor(0x22, 0x27, 0x2d));
    App.setPalette(P);
    //Shared styles, each scoped to the widgets it is for: the rest is Fusion on the palette above.
    App.setStyleSheet(
        "QListWidget#SettingsSidebar { background: #15191e; border: none; padding: 10px 0; outline: none; }"
        "QListWidget#SettingsSidebar::item { padding: 7px 12px; margin: 1px 8px; border-radius: 5px; color: #c7cdd3; }"
        "QListWidget#SettingsSidebar::item:hover { background: #222932; }"
        "QListWidget#SettingsSidebar::item:selected { background: #2b3e55; color: #ffffff; }"
        "QFrame#OfflineBanner { background: #2a2416; border: 1px solid #5a4a22; border-radius: 6px; }"
        "QFrame#OfflineBanner QLabel { color: #e8d6a8; }"
        "QToolTip { color: #e6e8ea; background: #2a3038; border: 1px solid #3a4250; padding: 4px; }");
}

QWidget *Capped(QWidget *Page, int Max)
{
    auto *Box = new QWidget(Page->parentWidget());
    auto *L = new QHBoxLayout(Box);
    L->setContentsMargins(0, 0, 0, 0);
    Page->setParent(Box);
    Page->setMaximumWidth(Max);
    L->addWidget(Page, 1);
    L->addStretch(0);
    return Box;
}

} // namespace Theme
