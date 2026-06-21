#include "settingstab.h"
#include "appmodel.h"
#include "packagesview.h"
#include "runnerspage.h"
#include "repositoriespage.h"
#include "downloadspage.h"
#include "pathspage.h"

#include <QListWidget>
#include <QStackedWidget>
#include <QHBoxLayout>
#include <QFrame>

SettingsTab::SettingsTab(AppModel &model, QWidget *parent)
    : QWidget(parent), Model(model)
{
    QHBoxLayout * outer = new QHBoxLayout(this);
    outer->setContentsMargins(0,0,0,0); outer->setSpacing(0);

    // Left: category sidebar.
    CategoryList = new QListWidget(this);
    CategoryList->setFixedWidth(170);
    CategoryList->setFrameShape(QFrame::NoFrame);
    CategoryList->addItem("Installed Packages");
    CategoryList->addItem("Runners");
    CategoryList->addItem("Repositories");
    CategoryList->addItem("Downloads");
    CategoryList->addItem("Storage & Paths");
    outer->addWidget(CategoryList);

    // Right: stacked, self-contained page widgets (each talks to the model directly).
    Stack = new QStackedWidget(this);
    outer->addWidget(Stack, 1);
    Stack->addWidget(new PackagesView(Model, Stack));
    Stack->addWidget(new RunnersPage(Model, Stack));
    Stack->addWidget(new RepositoriesPage(Model, Stack));
    Stack->addWidget(new DownloadsPage(Model, Stack));
    Stack->addWidget(new PathsPage(Model, Stack));

    connect(CategoryList, &QListWidget::currentRowChanged, Stack, &QStackedWidget::setCurrentIndex);
    CategoryList->setCurrentRow(0);
}
