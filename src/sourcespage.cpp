#include "sourcespage.h"
#include "appmodel.h"          // AppModel — config + sync/add/remove package source

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QScrollArea>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QMessageBox>
#include <QFrame>

#include <nlohmann/json.hpp>

#include <string>

SourcesPage::SourcesPage(AppModel &model, QWidget *parent)
    : QWidget(parent), Model(model)
{
    QVBoxLayout * pl = new QVBoxLayout(this);
    pl->setContentsMargins(8,8,8,8);
    Scroll = new QScrollArea(this);
    Scroll->setWidgetResizable(true);
    Scroll->setFrameShape(QFrame::NoFrame);
    pl->addWidget(Scroll);

    connect(&Model, &AppModel::packageSourcesChanged, this, &SourcesPage::rebuild);
    // A fetch failure (e.g. networking off) — surface it; the source stays configured for a later sync.
    connect(&Model, &AppModel::packageSourceFailed, this, [this](const QString & msg){
        QMessageBox::warning(this, "Add source", msg);
    });
    rebuild();
}

// Each Settings.PackageSources[] entry is {NAME, CID:<ipfs folder CID>}, fetched to ~/.VidyaGod/LIBRARY/<name> and
// indexed. Adding fetches the dehydrated tree (off-thread, via the model, needs the IPFS node online); removing drops
// the source, its fetched dir, and its LIBRARY entries.
void SourcesPage::rebuild()
{
    if (!Scroll) return;
    if (Scroll->widget()) Scroll->widget()->deleteLater();

    QWidget * contents = new QWidget(Scroll);
    QVBoxLayout * v = new QVBoxLayout(contents); contents->setLayout(v);
    Scroll->setWidget(contents);

    QLabel * intro = new QLabel(
        "Sources are IPFS folder CIDs of dehydrated packages (manifests + covers, no bundled content). Their packages "
        "appear in the catalog and hydrate their content over IPFS on download. A CID is immutable — to get an updated "
        "catalog, add the publisher's new CID. Adding/updating needs IPFS networking enabled.", contents);
    intro->setWordWrap(true);
    intro->setStyleSheet("color:#8f98a0;font-size:9pt;");
    v->addWidget(intro);

    // ── Sync now ── re-index sources, fetching any not-yet-present one (e.g. the default runners on a first run).
    {
        QHBoxLayout * syncRow = new QHBoxLayout();
        QPushButton * syncBtn = new QPushButton("Sync now", contents);
        syncRow->addWidget(syncBtn); syncRow->addStretch(1);
        v->addLayout(syncRow);
        connect(syncBtn, &QPushButton::clicked, this, [this, syncBtn]{
            syncBtn->setEnabled(false); syncBtn->setText("Syncing…");
            Model.syncSources();   // off-thread; packageSourcesChanged rebuilds this page (re-enabling the button) when done
        });
    }

    auto & S = (*Model.config())["Settings"];
    if (!S.contains("PackageSources") || !S["PackageSources"].is_array())
        S["PackageSources"] = nlohmann::ordered_json::array();

    if (S["PackageSources"].empty())
    {
        QLabel * none = new QLabel("No sources configured.", contents);
        none->setStyleSheet("color:#8f98a0;");
        v->addWidget(none);
    }
    for (int i = 0; i < int(S["PackageSources"].size()); ++i)
    {
        const auto & Src = S["PackageSources"][i];
        const std::string Cid = Src.is_object() ? Src.value("CID", std::string()) : (Src.is_string() ? std::string(Src) : std::string());
        std::string Name = Src.is_object() ? Src.value("NAME", std::string()) : std::string();
        if (Name.empty()) Name = Cid.size() > 12 ? Cid.substr(0, 12) : Cid;

        QGroupBox * card = new QGroupBox(QString::fromStdString(Name), contents);
        QHBoxLayout * row = new QHBoxLayout(card); card->setLayout(row);
        QLabel * cidLbl = new QLabel("<span style='color:#8f98a0;'>" + QString::fromStdString(Cid).toHtmlEscaped() + "</span>", card);
        cidLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        row->addWidget(cidLbl, 1);
        QPushButton * rm = new QPushButton("Remove", card);
        connect(rm, &QPushButton::clicked, this, [this, i]{ Model.removePackageSource(i); });   // model drops + rebuilds + emits
        row->addWidget(rm);
        v->addWidget(card);
    }

    // ── Add-source row ──
    QGroupBox * add = new QGroupBox("Add source", contents);
    QFormLayout * af = new QFormLayout(add); add->setLayout(af);
    QLineEdit * nameEdit = new QLineEdit(add);
    nameEdit->setPlaceholderText("(optional — defaults to a short CID prefix)");
    QLineEdit * cidEdit = new QLineEdit(add);
    cidEdit->setPlaceholderText("Qm…  (an IPFS folder CID of dehydrated packages)");
    af->addRow("Name:", nameEdit);
    af->addRow("CID:", cidEdit);
    QPushButton * addBtn = new QPushButton("Add + fetch", add);
    af->addRow("", addBtn);
    connect(addBtn, &QPushButton::clicked, this, [this, nameEdit, cidEdit, addBtn]{
        const QString Cid = cidEdit->text().trimmed();
        if (Cid.isEmpty()) { QMessageBox::warning(this, "Add source", "Enter an IPFS folder CID."); return; }
        if (!Model.addPackageSource(Cid, nameEdit->text()))   // false = empty or already configured
        { QMessageBox::warning(this, "Add source", "That source is already configured."); return; }
        addBtn->setEnabled(false); addBtn->setText("Fetching…");   // success → page rebuilds (re-enabling) on packageSourcesChanged
    });
    v->addWidget(add);

    v->addStretch(1);
}
