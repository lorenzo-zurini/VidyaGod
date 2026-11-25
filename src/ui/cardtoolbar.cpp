#include "cardtoolbar.h"
#include "theme.h"   // the named colours
#include "appmodel.h"

#include <QActionGroup>
#include <QButtonGroup>
#include <QMenu>
#include <QToolButton>
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
        T.Status->setStyleSheet("color:" + Theme::Dim + "; font-size:9pt; padding-left:12px;");
        T.Row->addWidget(T.Status);
    }
    T.Row->addStretch();

    QLineEdit *Search = new QLineEdit(T.Bar);
    Search->setPlaceholderText("Search…");
    Search->setClearButtonEnabled(true);
    Search->setMinimumWidth(110);   // it gives way first on a narrow window; the labels never do
    Search->setMaximumWidth(240);
    Search->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QObject::connect(Search, &QLineEdit::textChanged, Owner, [OnSearch](const QString &Text) { OnSearch(Text.trimmed()); });
    T.Row->addWidget(Search, 1);

    //The card size: rarely changed, so one menu (three chips took the width the labels needed at 640 px).
    QToolButton *Size = new QToolButton(T.Bar);
    Size->setObjectName("cardSize");
    Size->setText("Size");
    Size->setToolTip("Card size");
    Size->setPopupMode(QToolButton::InstantPopup);
    Size->setAutoRaise(true);
    QMenu *SizeMenu = new QMenu(Size);
    QActionGroup *Sizes = new QActionGroup(SizeMenu);
    Sizes->setExclusive(true);
    for (const auto &[Label, Width] : std::vector<std::pair<QString, int>>{ {"Large", 250}, {"Medium", 185}, {"Small", 120} })
    {
        QAction *A = SizeMenu->addAction(Label);
        A->setObjectName("size" + Label);   // sizeLarge, sizeMedium, sizeSmall: one name each
        A->setCheckable(true);
        A->setChecked(Model.cardPixelWidth() == Width);
        Sizes->addAction(A);
        QObject::connect(A, &QAction::triggered, Owner, [&Model, W = Width] { Model.setCardPixelWidth(W); });   // → cardSizeChanged
    }
    Size->setMenu(SizeMenu);
    T.Row->addWidget(Size);
    QFrame *Sep = new QFrame(T.Bar);
    Sep->setFrameShape(QFrame::VLine);
    Sep->setStyleSheet("color:#3a4048;");
    T.Row->addWidget(Sep);
    return T;
}

void SyncSizeButtons(QWidget *Owner, int CardWidth)
{
    const QString Want = CardWidth >= 250 ? "Large" : CardWidth <= 120 ? "Small" : "Medium";
    for (auto *A : Owner->findChildren<QAction *>(QRegularExpression("^size(Large|Medium|Small)$"))) A->setChecked(A->text() == Want);
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
