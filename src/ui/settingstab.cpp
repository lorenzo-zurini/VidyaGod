#include "settingstab.h"
#include <QScrollArea>
#include "appmodel.h"
#include "generalpage.h"
#include "controllerpage.h"
#include "dependenciespage.h"
#include "packagesview.h"
#include "runnerspage.h"
#include "sourcespage.h"
#include "networkpage.h"
#include "pathspage.h"
#include "theme.h"

#include <QListWidget>
#include <QStackedWidget>
#include <QHBoxLayout>
#include <QFrame>
#include <QStyle>

SettingsTab::SettingsTab(AppModel &model, QWidget *parent)
    : QWidget(parent), Model(model)
{
    QHBoxLayout * outer = new QHBoxLayout(this);
    outer->setContentsMargins(0,0,0,0); outer->setSpacing(0);

    // Left: category sidebar.
    CategoryList = new QListWidget(this);
    CategoryList->setObjectName("SettingsSidebar");   // its look: Theme
    CategoryList->setAccessibleName("Settings pages");
    CategoryList->setProperty("padPane", true);   // a controller's B from a page comes back here (DeckShell)
    CategoryList->setFixedWidth(200);
    CategoryList->setFrameShape(QFrame::NoFrame);
    CategoryList->setIconSize(QSize(16, 16));
    const auto Add = [this](QStyle::StandardPixmap Icon, const char *Name) {
        auto *It = new QListWidgetItem(style()->standardIcon(Icon), Name);
        It->setSizeHint(QSize(0, 32));   // one row height, icon or not
        CategoryList->addItem(It);
    };
    Add(QStyle::SP_FileDialogDetailedView, "General");
    Add(QStyle::SP_MediaPlay, "Controller");
    Add(QStyle::SP_FileDialogInfoView, "Dependencies");   // not a warning sign: one that is always there is ignored
    Add(QStyle::SP_DriveHDIcon, "Installed Packages");
    Add(QStyle::SP_DesktopIcon, "Runners");
    Add(QStyle::SP_DirLinkIcon, "Sources");
    Add(QStyle::SP_DriveNetIcon, "Network");
    Add(QStyle::SP_DirIcon, "Storage & Paths");
    outer->addWidget(CategoryList);

    // Right: stacked, self-contained page widgets (each talks to the model directly). Built LAZILY on first visit —
    // constructing all of them up front stat-ed the disk (Installed Packages hydration checks) and enumerated
    // runners/sources on the main thread at launch, before the window was interactive, for a tab the user may never
    // open. A placeholder holds each slot so the stack index stays aligned with the sidebar row.
    Stack = new QStackedWidget(this);
    outer->addWidget(Stack, 1);
    PageFactories = {
        [this]{ return static_cast<QWidget *>(new GeneralPage(Model, Stack)); },
        [this]{ return static_cast<QWidget *>(new ControllerPage(Model, Stack)); },
        [this]{ return static_cast<QWidget *>(new DependenciesPage(Model, Stack)); },
        [this]{ return static_cast<QWidget *>(new PackagesView(Model, Stack)); },
        [this]{ return static_cast<QWidget *>(new RunnersPage(Model, Stack)); },
        [this]{ return static_cast<QWidget *>(new SourcesPage(Model, Stack)); },
        [this]{ return static_cast<QWidget *>(new NetworkPage(Model, Stack)); },
        [this]{ return static_cast<QWidget *>(new PathsPage(Model, Stack)); },
    };
    Pages.assign(PageFactories.size(), nullptr);
    for (std::size_t i = 0; i < PageFactories.size(); ++i) Stack->addWidget(new QWidget(Stack));   // placeholders

    connect(CategoryList, &QListWidget::currentRowChanged, this, [this](int row){ showPage(row); });
    // Qt treats SELECTION and CURRENT ITEM as different things, and assistive tech (AT-SPI — screen readers, and
    // the tools/a11ydrive.py harness) drives SELECTION. Listening only to currentRowChanged meant such a client
    // could highlight "Sources" while the panel kept showing General: visibly selected, functionally inert.
    // Mirror selection onto the current row so both paths land on the same page.
    connect(CategoryList, &QListWidget::itemSelectionChanged, this, [this]{
        const QList<QListWidgetItem *> Sel = CategoryList->selectedItems();
        if (Sel.isEmpty()) return;
        const int Row = CategoryList->row(Sel.first());
        if (Row >= 0 && Row != CategoryList->currentRow()) CategoryList->setCurrentRow(Row);
    });
    CategoryList->setCurrentRow(0);   // builds + shows the first page only
}

void SettingsTab::showPage(int row)
{
    if (row < 0 || row >= (int)PageFactories.size()) return;
    if (!Pages[row])
    {
        //Content no wider than a page reads well, in a scroll area: a small window scrolls a long page instead of
        //crushing its rows into each other (and never sideways: the page's text wraps).
        auto * Scroll = new QScrollArea(Stack);
        Scroll->setFrameShape(QFrame::NoFrame);
        Scroll->setWidgetResizable(true);
        Scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        Scroll->setWidget(Theme::Capped(PageFactories[row]()));
        Scroll->viewport()->setAutoFillBackground(false);
        Scroll->widget()->setAutoFillBackground(false);
        Pages[row] = Scroll;
        QWidget * placeholder = Stack->widget(row);       // pointer to the slot's placeholder
        Stack->insertWidget(row, Pages[row]);             // real page takes index `row`; placeholder shifts to row+1
        Stack->removeWidget(placeholder);                 // remove by pointer — leaves index `row` intact
        placeholder->deleteLater();
    }
    Stack->setCurrentIndex(row);
}

bool SettingsTab::openPage(const QString &Name)
{
    for (int Row = 0; Row < CategoryList->count(); ++Row)
        if (CategoryList->item(Row)->text() == Name)
        {
            CategoryList->setCurrentRow(Row);   // the sidebar shows it too; showPage follows
            return true;
        }
    return false;
}
