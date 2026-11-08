#include "cardtoolbar.h"
#include "appmodel.h"

#include <QButtonGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>

CardToolbar MakeCardToolbar(QWidget *Owner, AppModel &Model, const std::vector<std::pair<QString, SortMode>> &Sorts,
                            SortMode Current, std::function<void(SortMode)> OnSort,
                            std::function<void(const QString &)> OnSearch, const QString &StatusName)
{
    CardToolbar T;
    T.Bar = new QWidget(Owner);
    T.Row = new QHBoxLayout(T.Bar);
    T.Row->setContentsMargins(8, 4, 8, 4);

    const QString SortStyle =
        "QPushButton{background:transparent;border:none;font-size:9pt;padding:2px 8px;}"
        "QPushButton:checked{border-bottom:2px solid palette(highlight);font-weight:bold;}"
        "QPushButton:hover{color:palette(highlighted-text);}";
    QButtonGroup *Group = new QButtonGroup(T.Bar);
    Group->setExclusive(true);
    for (const auto &[Label, Mode] : Sorts)
    {
        QPushButton *B = new QPushButton(Label, T.Bar);
        B->setCheckable(true);
        B->setChecked(Current == Mode);
        B->setStyleSheet(SortStyle);
        Group->addButton(B);
        QObject::connect(B, &QPushButton::toggled, Owner, [OnSort, Mode = Mode](bool Checked) { if (Checked) OnSort(Mode); });
        T.Row->addWidget(B);
    }
    if (!StatusName.isEmpty())
    {
        T.Status = new QLabel(T.Bar);
        T.Status->setAccessibleName(StatusName);
        T.Status->setStyleSheet("color:#8f98a0; font-size:9pt; padding-left:12px;");
        T.Row->addWidget(T.Status);
    }
    T.Row->addStretch();

    QLineEdit *Search = new QLineEdit(T.Bar);
    Search->setPlaceholderText("Search…");
    Search->setClearButtonEnabled(true);
    Search->setFixedWidth(180);
    QObject::connect(Search, &QLineEdit::textChanged, Owner, [OnSearch](const QString &Text) { OnSearch(Text.trimmed()); });
    T.Row->addWidget(Search);

    for (const auto &[Label, Width] : std::vector<std::pair<QString, int>>{ {"Large", 250}, {"Medium", 185}, {"Small", 120} })
    {
        QPushButton *B = new QPushButton(Label, T.Bar);
        B->setObjectName("size" + Label);   // sizeLarge, sizeMedium, sizeSmall: one name each
        B->setCheckable(true);
        B->setChecked(Model.cardPixelWidth() == Width);
        B->setStyleSheet(
            "QPushButton{color:#8f98a0;background:transparent;border:none;font-size:9pt;padding:2px 8px;}"
            "QPushButton:checked{color:#c6d4df;border-bottom:2px solid #4a90d9;}"
            "QPushButton:hover{color:#c6d4df;}");
        QObject::connect(B, &QPushButton::clicked, Owner, [&Model, W = Width] { Model.setCardPixelWidth(W); });   // → cardSizeChanged
        T.Row->addWidget(B);
    }
    QFrame *Sep = new QFrame(T.Bar);
    Sep->setFrameShape(QFrame::VLine);
    Sep->setStyleSheet("color:#3a4048;");
    T.Row->addWidget(Sep);
    return T;
}

void SyncSizeButtons(QWidget *Owner, int CardWidth)
{
    const QString Want = CardWidth >= 250 ? "Large" : CardWidth <= 120 ? "Small" : "Medium";
    for (auto *B : Owner->findChildren<QPushButton *>(QRegularExpression("^size(Large|Medium|Small)$"))) B->setChecked(B->text() == Want);
}

QSet<QString> LoadCollapsed(AppModel &Model, const char *Key)
{
    QSet<QString> Out;
    const auto &S = (*Model.config())["Settings"];
    if (S.contains(Key) && S[Key].is_array())
        for (const auto &N : S[Key]) if (N.is_string()) Out.insert(QString::fromStdString(N.get<std::string>()));
    return Out;
}

void SaveCollapsed(AppModel &Model, const char *Key, const QSet<QString> &Collapsed)
{
    auto &Arr = (*Model.config())["Settings"][Key] = nlohmann::ordered_json::array();
    for (const QString &N : Collapsed) Arr.push_back(N.toStdString());
    Model.save();
}
