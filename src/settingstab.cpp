#include "settingstab.h"
#include "appmodel.h"
#include "generalpage.h"
#include "dependenciespage.h"
#include "packagesview.h"
#include "runnerspage.h"
#include "sourcespage.h"
#include "ipfssettingspage.h"
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
    CategoryList->addItem("General");
    CategoryList->addItem("Dependencies");
    CategoryList->addItem("Installed Packages");
    CategoryList->addItem("Runners");
    CategoryList->addItem("Sources");
    CategoryList->addItem("IPFS");
    CategoryList->addItem("Storage & Paths");
    outer->addWidget(CategoryList);

    // Right: stacked, self-contained page widgets (each talks to the model directly). Built LAZILY on first visit —
    // constructing all seven up front stat-ed the disk (Installed Packages hydration checks) and enumerated
    // runners/sources on the main thread at launch, before the window was interactive, for a tab the user may never
    // open. A placeholder holds each slot so the stack index stays aligned with the sidebar row.
    Stack = new QStackedWidget(this);
    outer->addWidget(Stack, 1);
    PageFactories = {
        [this]{ return static_cast<QWidget *>(new GeneralPage(Model, Stack)); },
        [this]{ return static_cast<QWidget *>(new DependenciesPage(Model, Stack)); },
        [this]{ return static_cast<QWidget *>(new PackagesView(Model, Stack)); },
        [this]{ return static_cast<QWidget *>(new RunnersPage(Model, Stack)); },
        [this]{ return static_cast<QWidget *>(new SourcesPage(Model, Stack)); },
        [this]{ return static_cast<QWidget *>(new IpfsSettingsPage(Model, Stack)); },
        [this]{ return static_cast<QWidget *>(new PathsPage(Model, Stack)); },
    };
    Pages.assign(PageFactories.size(), nullptr);
    for (std::size_t i = 0; i < PageFactories.size(); ++i) Stack->addWidget(new QWidget(Stack));   // placeholders

    connect(CategoryList, &QListWidget::currentRowChanged, this, [this](int row){ showPage(row); });
    CategoryList->setCurrentRow(0);   // builds + shows the first page only
}

void SettingsTab::showPage(int row)
{
    if (row < 0 || row >= (int)PageFactories.size()) return;
    if (!Pages[row])
    {
        Pages[row] = PageFactories[row]();
        QWidget * placeholder = Stack->widget(row);       // pointer to the slot's placeholder
        Stack->insertWidget(row, Pages[row]);             // real page takes index `row`; placeholder shifts to row+1
        Stack->removeWidget(placeholder);                 // remove by pointer — leaves index `row` intact
        placeholder->deleteLater();
    }
    Stack->setCurrentIndex(row);
}
