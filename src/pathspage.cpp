#include "pathspage.h"
#include "apppaths.h"          // AppPaths::DataRoot — default TEMP/LIBRARY display roots
#include "appmodel.h"          // AppModel — config + appDataDir + save

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QFileDialog>
#include <QDir>

#include <nlohmann/json.hpp>

#include <string>

PathsPage::PathsPage(AppModel &model, QWidget *parent)
    : QWidget(parent), Model(model)
{
    QVBoxLayout * pl = new QVBoxLayout(this);
    pl->setContentsMargins(12,12,12,12);
    QFormLayout * form = new QFormLayout();
    pl->addLayout(form);

    const QString defaultTempRoot = QDir::cleanPath(QString::fromStdString((AppPaths::DataRoot() / "TEMP").string()));

    // Temporary / runtime root — Settings.Paths.TempRoot (empty = default).
    QWidget * rootRow = new QWidget(this);
    QHBoxLayout * rl = new QHBoxLayout(rootRow); rl->setContentsMargins(0,0,0,0);
    rootRow->setLayout(rl);
    QLineEdit * rootEdit = new QLineEdit(rootRow);
    rootEdit->setPlaceholderText(defaultTempRoot + "  (default)");
    {
        auto & S = (*Model.config())["Settings"];
        if (S.contains("Paths") && S["Paths"].is_object()
            && S["Paths"].contains("TempRoot") && S["Paths"]["TempRoot"].is_string())
            rootEdit->setText(QString::fromStdString(std::string(S["Paths"]["TempRoot"])));
    }
    auto writeTempRoot = [this,rootEdit]{
        auto & S = (*Model.config())["Settings"];
        QString t = rootEdit->text().trimmed();
        if (t.isEmpty()) {
            if (S.contains("Paths") && S["Paths"].is_object()) S["Paths"].erase("TempRoot");
        } else {
            if (!S.contains("Paths") || !S["Paths"].is_object()) S["Paths"] = nlohmann::ordered_json::object();
            S["Paths"]["TempRoot"] = t.toStdString();
        }
        Model.save();
    };
    connect(rootEdit, &QLineEdit::editingFinished, this, writeTempRoot);
    rl->addWidget(rootEdit, 1);
    QPushButton * browse = new QPushButton("Browse…", rootRow);
    connect(browse, &QPushButton::clicked, this, [this,rootEdit,writeTempRoot]{
        QString d = QFileDialog::getExistingDirectory(this, "Select temporary / runtime root");
        if (!d.isEmpty()) { rootEdit->setText(d); writeTempRoot(); }
    });
    rl->addWidget(browse);
    form->addRow("Temporary / runtime root:", rootRow);

    // Library folder — Settings.Paths.LibraryRoot (empty = default). Imported packages are hydrated here,
    // one subfolder per repo.
    const QString defaultLibRoot = QDir::cleanPath(QString::fromStdString((AppPaths::DataRoot() / "LIBRARY").string()));
    QWidget * libRow = new QWidget(this);
    QHBoxLayout * ll = new QHBoxLayout(libRow); ll->setContentsMargins(0,0,0,0);
    libRow->setLayout(ll);
    QLineEdit * libEdit = new QLineEdit(libRow);
    libEdit->setPlaceholderText(defaultLibRoot + "  (default)");
    {
        auto & S = (*Model.config())["Settings"];
        if (S.contains("Paths") && S["Paths"].is_object()
            && S["Paths"].contains("LibraryRoot") && S["Paths"]["LibraryRoot"].is_string())
            libEdit->setText(QString::fromStdString(std::string(S["Paths"]["LibraryRoot"])));
    }
    auto writeLibRoot = [this,libEdit]{
        auto & S = (*Model.config())["Settings"];
        QString t = libEdit->text().trimmed();
        if (t.isEmpty()) {
            if (S.contains("Paths") && S["Paths"].is_object()) S["Paths"].erase("LibraryRoot");
        } else {
            if (!S.contains("Paths") || !S["Paths"].is_object()) S["Paths"] = nlohmann::ordered_json::object();
            S["Paths"]["LibraryRoot"] = t.toStdString();
        }
        Model.save();
    };
    connect(libEdit, &QLineEdit::editingFinished, this, writeLibRoot);
    ll->addWidget(libEdit, 1);
    QPushButton * libBrowse = new QPushButton("Browse…", libRow);
    connect(libBrowse, &QPushButton::clicked, this, [this,libEdit,writeLibRoot]{
        QString d = QFileDialog::getExistingDirectory(this, "Select library folder");
        if (!d.isEmpty()) { libEdit->setText(d); writeLibRoot(); }
    });
    ll->addWidget(libBrowse);
    form->addRow("Library folder:", libRow);

    // AppData dir (read-only, informational — determined at launch, not settable here). Disabled so it's visibly
    // non-editable, but kept text-selectable so the path can still be copied.
    QLineEdit * appDataLbl = new QLineEdit(Model.appDataDir()->path(), this);
    appDataLbl->setReadOnly(true);
    appDataLbl->setStyleSheet("QLineEdit{color:palette(disabled-text);background:palette(window);}");
    form->addRow("App data directory:", appDataLbl);

    QLabel * note = new QLabel("Path changes apply to future launches.", this);
    note->setStyleSheet("color:#8f98a0;font-size:9pt;");
    pl->addWidget(note);
    pl->addStretch(1);
}
