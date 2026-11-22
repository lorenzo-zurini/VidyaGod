#include "layouteditor.h"

#include "controllerservice.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

using namespace ControllerLayout;

namespace
{
const QColor Lime(0x3c, 0xe0, 0x5c);

// Where each control is drawn on the Deck, as fractions of the picture (x, y, w, h). A stick's click is the stick's
// inner disc; controls not listed (stick and trackpad touches, a trackpad's click) are in the list only.
struct Spot { const char *Id; double X, Y, W, H; bool Round; const char *Text; };
// Fractions of the Deck's own rectangle (DeckRect: as wide as 2.25 heights, the Deck's proportion, centred), the
// shoulder buttons above it and the back grips below.
const Spot Spots[] = {
    { "lt", 0.05, 0.00, 0.09, 0.07, false, "L2" },     { "lb", 0.15, 0.015, 0.09, 0.06, false, "L1" },
    { "rb", 0.76, 0.015, 0.09, 0.06, false, "R1" },    { "rt", 0.86, 0.00, 0.09, 0.07, false, "R2" },
    { "dpad_up", 0.072, 0.17, 0.03, 0.08, false, "▲" },  { "dpad_down", 0.072, 0.35, 0.03, 0.08, false, "▼" },
    { "dpad_left", 0.040, 0.26, 0.03, 0.08, false, "◀" }, { "dpad_right", 0.104, 0.26, 0.03, 0.08, false, "▶" },
    { "left_stick", 0.17, 0.17, 0.08, 0.18, true, "" },   { "l3", 0.193, 0.22, 0.034, 0.08, true, "L3" },
    { "right_stick", 0.75, 0.17, 0.08, 0.18, true, "" },  { "r3", 0.773, 0.22, 0.034, 0.08, true, "R3" },
    { "y", 0.897, 0.15, 0.036, 0.08, true, "Y" }, { "a", 0.897, 0.37, 0.036, 0.08, true, "A" },
    { "x", 0.862, 0.26, 0.036, 0.08, true, "X" }, { "b", 0.932, 0.26, 0.036, 0.08, true, "B" },
    { "left_pad", 0.11, 0.50, 0.14, 0.32, false, "" },  { "right_pad", 0.75, 0.50, 0.14, 0.32, false, "" },
    { "select", 0.255, 0.12, 0.03, 0.07, true, "⧉" },   { "start", 0.715, 0.12, 0.03, 0.07, true, "☰" },
    { "guide", 0.245, 0.83, 0.05, 0.07, false, "STEAM" }, { "quick", 0.705, 0.83, 0.05, 0.07, false, "…" },
    { "l4", 0.05, 0.92, 0.06, 0.07, false, "L4" }, { "l5", 0.12, 0.93, 0.06, 0.07, false, "L5" },
    { "r4", 0.89, 0.92, 0.06, 0.07, false, "R4" }, { "r5", 0.82, 0.93, 0.06, 0.07, false, "R5" },
    { "gyro", 0.47, 0.83, 0.06, 0.07, false, "gyro" },
};

// The Deck's rectangle in a widget's rectangle: 2.25 times as wide as tall, as large as fits, centred.
QRectF DeckRect(const QRectF &Area)
{
    const double Aspect = 2.25;
    double W = Area.width(), H = W / Aspect;
    if (H > Area.height()) { H = Area.height(); W = H * Aspect; }
    return QRectF(Area.center().x() - W / 2, Area.center().y() - H / 2, W, H);
}

QString Q(const std::string &S) { return QString::fromStdString(S); }
}

// ---------------------------------------------------------------------------
ControllerPicture::ControllerPicture(QWidget *Parent) : QWidget(Parent)
{
    setMinimumSize(320, 210);
    setFocusPolicy(Qt::NoFocus);   // the list beside it is what a controller walks
    setCursor(Qt::PointingHandCursor);
}

void ControllerPicture::setSelected(const QString &Id) { Selected = Id; update(); }

void ControllerPicture::setChanged(const QStringList &Ids) { Changed = Ids; update(); }

QRectF ControllerPicture::hotspot(const QString &Id) const
{
    const QRectF Area = DeckRect(QRectF(rect()).adjusted(8, 8, -8, -8));
    for (const Spot &S : Spots)
        if (Id == QLatin1String(S.Id))
            return QRectF(Area.x() + S.X * Area.width(), Area.y() + S.Y * Area.height(), S.W * Area.width(), S.H * Area.height());
    return {};
}

void ControllerPicture::paintEvent(QPaintEvent *)
{
    QPainter P(this);
    P.setRenderHint(QPainter::Antialiasing);
    const QRectF Area = DeckRect(QRectF(rect()).adjusted(8, 8, -8, -8));
    const QColor Body = palette().color(QPalette::Mid), Ink = palette().color(QPalette::WindowText), Face = palette().color(QPalette::Button);
    //The body (its grips rounded), the screen.
    const QRectF B(Area.x(), Area.y() + 0.06 * Area.height(), Area.width(), 0.86 * Area.height());
    P.setPen(QPen(Ink, 1.5));
    P.setBrush(Body);
    P.drawRoundedRect(B, B.height() * 0.42, B.height() * 0.42);
    const QRectF Screen(Area.x() + 0.31 * Area.width(), Area.y() + 0.13 * Area.height(), 0.38 * Area.width(), 0.64 * Area.height());
    P.setBrush(palette().color(QPalette::Dark));
    P.drawRoundedRect(Screen, 6, 6);
    QFont F = font();
    F.setBold(true);
    for (const Spot &S : Spots)
    {
        const QRectF R = hotspot(QLatin1String(S.Id));
        const bool On = Selected == QLatin1String(S.Id), Bound = Changed.contains(QLatin1String(S.Id));
        if (On)   // the lift under the selected one: the mark's magenta, offset
        {
            P.setPen(Qt::NoPen);
            P.setBrush(QColor(0xc4, 0x12, 0xd2));
            const QRectF L = R.translated(3, 3);
            if (S.Round) P.drawEllipse(L); else P.drawRoundedRect(L, 5, 5);
        }
        P.setPen(QPen(On || Bound ? Lime : Ink, On ? 3 : Bound ? 2 : 1.2));
        P.setBrush(On ? Lime.darker(260) : Face);
        if (S.Round) P.drawEllipse(R);
        else P.drawRoundedRect(R, 5, 5);
        if (*S.Text)
        {
            F.setPixelSize(std::clamp(int(std::min(R.height() * 0.5, R.width() * 0.9 / std::max<qsizetype>(1, qstrlen(S.Text) > 3 ? 4 : 1))), 7, 22));
            P.setFont(F);
            P.setPen(On ? Lime : Ink);
            P.drawText(R, Qt::AlignCenter, QString::fromUtf8(S.Text));
        }
    }
}

void ControllerPicture::mousePressEvent(QMouseEvent *E)
{
    //The innermost hit wins: a stick's click is drawn inside the stick.
    QString Hit;
    double Smallest = 1e18;
    for (const Spot &S : Spots)
    {
        const QRectF R = hotspot(QLatin1String(S.Id));
        if (R.contains(E->position()) && R.width() * R.height() < Smallest) { Smallest = R.width() * R.height(); Hit = QLatin1String(S.Id); }
    }
    if (!Hit.isEmpty()) { setSelected(Hit); emit controlClicked(Hit); }
}

// ---------------------------------------------------------------------------
LayoutEditor::LayoutEditor(const QString &Dir_, const QString &GameKey_, const QString &GameTitle_, QWidget *Parent)
    : QDialog(Parent), Dir(Dir_), GameKey(GameKey_), GameTitle(GameTitle_)
{
    setObjectName(QStringLiteral("LayoutEditor"));
    setWindowTitle(GameTitle.isEmpty() ? tr("Controller layouts") : tr("Controller layout — %1").arg(GameTitle));
    buildUi();
    resize(1180, 720);
    const QString ForThis = GameKey.isEmpty() ? QString() : Q(ForGame(Dir.toStdString(), GameKey.toStdString()));
    refillLayouts(ForThis.isEmpty() ? QStringLiteral("Gamepad") : ForThis);
    openLayout(ForThis.isEmpty() ? QStringLiteral("Gamepad") : ForThis);
    selectControl(QStringLiteral("a"));
}

void LayoutEditor::buildUi()
{
    auto *Root = new QVBoxLayout(this);

    //Which layout.
    auto *Top = new QHBoxLayout;
    Top->addWidget(new QLabel(tr("Layout"), this));
    LayoutCombo = new QComboBox(this);
    LayoutCombo->setObjectName(QStringLiteral("layoutCombo"));
    LayoutCombo->setProperty("padAdjust", true);
    LayoutCombo->setMinimumContentsLength(28);
    Top->addWidget(LayoutCombo, 1);
    DeleteButton = new QPushButton(tr("Delete"), this);
    DeleteButton->setObjectName(QStringLiteral("deleteLayout"));
    Top->addWidget(DeleteButton);
    Root->addLayout(Top);
    Description = new QLabel(this);
    Description->setWordWrap(true);
    Description->setObjectName(QStringLiteral("layoutDescription"));
    Root->addWidget(Description);

    //The controller, the controls, the selected control's binding.
    auto *Mid = new QSplitter(Qt::Horizontal, this);
    Mid->setObjectName(QStringLiteral("layoutSplitter"));
    Mid->setChildrenCollapsible(false);
    //Left: the controller, and under it what the selected control does. Right: every control, the whole height.
    auto *LeftW = new QWidget(Mid);
    auto *Left = new QVBoxLayout(LeftW);
    Left->setContentsMargins(0, 0, 0, 0);
    Picture = new ControllerPicture(LeftW);
    Left->addWidget(Picture, 1);
    Tree = new QTreeWidget(Mid);
    Tree->setObjectName(QStringLiteral("controlsTree"));
    Tree->setColumnCount(2);
    Tree->setHeaderLabels({ tr("Control"), tr("Does") });
    Tree->setRootIsDecorated(false);
    Tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    Tree->setMinimumWidth(340);
    ControlTitle = new QLabel(LeftW);
    QFont TF = ControlTitle->font();
    TF.setBold(true);
    ControlTitle->setFont(TF);
    Left->addWidget(ControlTitle);
    Editors = new QStackedWidget(LeftW);

    //Page 0: a button or a trigger.
    auto *ButtonPage = new QWidget(Editors);
    auto *BF = ButtonForm = new QFormLayout(ButtonPage);
    TypeCombo = new QComboBox(ButtonPage);
    TypeCombo->setObjectName(QStringLiteral("actionType"));
    ValueCombo = new QComboBox(ButtonPage);
    ValueCombo->setObjectName(QStringLiteral("actionValue"));
    Chord = new QCheckBox(tr("Also press another"), ButtonPage);
    Chord->setObjectName(QStringLiteral("chord"));
    Type2Combo = new QComboBox(ButtonPage);
    Type2Combo->setObjectName(QStringLiteral("actionType2"));
    Value2Combo = new QComboBox(ButtonPage);
    Value2Combo->setObjectName(QStringLiteral("actionValue2"));
    for (QComboBox *C : { TypeCombo, ValueCombo, Type2Combo, Value2Combo }) C->setProperty("padAdjust", true);
    BF->addRow(tr("Does"), TypeCombo);
    BF->addRow(tr("Which"), ValueCombo);
    BF->addRow(QString(), Chord);
    BF->addRow(tr("And"), Type2Combo);
    BF->addRow(tr("Which"), Value2Combo);
    Editors->addWidget(ButtonPage);

    //Page 1: a stick.
    auto *StickPage = new QWidget(Editors);
    auto *SF = new QFormLayout(StickPage);
    StickMode = new QComboBox(StickPage);
    StickMode->setObjectName(QStringLiteral("stickMode"));
    StickMode->setProperty("padAdjust", true);
    StickMode->addItem(tr("Joystick (as it is)"), QStringLiteral("joystick"));
    StickMode->addItem(tr("Mouse"), QStringLiteral("mouse"));
    StickMode->addItem(tr("Four keys"), QStringLiteral("keys"));
    StickMode->addItem(tr("D-pad"), QStringLiteral("dpad"));
    SF->addRow(tr("Works as"), StickMode);
    auto *SpeedRow = new QHBoxLayout;
    Speed = new QSlider(Qt::Horizontal, StickPage);
    Speed->setObjectName(QStringLiteral("mouseSpeed"));
    Speed->setRange(2, 40);   // hundreds of pixels a second
    SpeedLabel = new QLabel(StickPage);
    SpeedRow->addWidget(Speed, 1);
    SpeedRow->addWidget(SpeedLabel);
    SF->addRow(tr("Speed"), SpeedRow);
    static const char *Dirs[4] = { QT_TR_NOOP("Up"), QT_TR_NOOP("Down"), QT_TR_NOOP("Left"), QT_TR_NOOP("Right") };
    for (int I = 0; I < 4; ++I)
    {
        StickKeys[I] = new QComboBox(StickPage);
        StickKeys[I]->setObjectName(QStringLiteral("stickKey%1").arg(I));
        StickKeys[I]->setProperty("padAdjust", true);
        for (const std::string &K : KeyboardKeys()) StickKeys[I]->addItem(Q(KeyLabel(K)), Q(K));
        SF->addRow(tr(Dirs[I]), StickKeys[I]);
    }
    Editors->addWidget(StickPage);

    //Page 2: a note (a trackpad, the gyro).
    Note = new QLabel(Editors);
    Note->setWordWrap(true);
    Note->setObjectName(QStringLiteral("bindingNote"));
    Editors->addWidget(Note);
    Left->addWidget(Editors);
    Mid->setStretchFactor(0, 3);
    Mid->setStretchFactor(1, 2);
    Root->addWidget(Mid, 1);

    //Save, try, use, close.
    auto *Bottom = new QHBoxLayout;
    Bottom->addWidget(new QLabel(tr("Name"), this));
    NameEdit = new QLineEdit(this);
    NameEdit->setObjectName(QStringLiteral("layoutName"));
    Bottom->addWidget(NameEdit, 1);
    SaveButton = new QPushButton(tr("Save"), this);
    SaveButton->setObjectName(QStringLiteral("saveLayout"));
    TryButton = new QPushButton(tr("Try now"), this);
    TryButton->setObjectName(QStringLiteral("tryLayout"));
    UseButton = new QPushButton(tr("Use for this game"), this);
    UseButton->setObjectName(QStringLiteral("useLayout"));
    UseButton->setVisible(!GameKey.isEmpty());
    auto *Close = new QPushButton(tr("Close"), this);
    Close->setObjectName(QStringLiteral("closeLayouts"));
    for (QPushButton *Bt : { SaveButton, TryButton, UseButton, Close }) Bottom->addWidget(Bt);
    Root->addLayout(Bottom);
    auto *StatusRow = new QHBoxLayout;
    UsedBy = new QLabel(this);
    UsedBy->setObjectName(QStringLiteral("usedBy"));
    Status = new QLabel(this);
    Status->setObjectName(QStringLiteral("layoutStatus"));
    StatusRow->addWidget(UsedBy, 1);
    StatusRow->addWidget(Status);
    Root->addLayout(StatusRow);

    //Wiring.
    connect(LayoutCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        if (Filling) return;
        const QString Was = Q(Current.Name);
        const bool Dropped = Dirty;
        openLayout(LayoutCombo->currentData().toString());
        if (Dropped) setStatus(tr("Unsaved changes to %1 were dropped").arg(Was));
    });
    connect(Picture, &ControllerPicture::controlClicked, this, [this](const QString &Id) { selectControl(Id); });
    connect(Tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *I, QTreeWidgetItem *) {
        if (Filling || !I) return;
        const QString Id = I->data(0, Qt::UserRole).toString();
        if (Id.isEmpty()) return;
        Picture->setSelected(Id);
        showBinding();
    });
    connect(TypeCombo, &QComboBox::currentIndexChanged, this, [this](int) { if (!Filling) applyFromWidgets(); });
    connect(ValueCombo, &QComboBox::currentIndexChanged, this, [this](int) { if (!Filling) applyFromWidgets(); });
    connect(Chord, &QCheckBox::toggled, this, [this](bool) { if (!Filling) applyFromWidgets(); });
    connect(Type2Combo, &QComboBox::currentIndexChanged, this, [this](int) { if (!Filling) applyFromWidgets(); });
    connect(Value2Combo, &QComboBox::currentIndexChanged, this, [this](int) { if (!Filling) applyFromWidgets(); });
    connect(StickMode, &QComboBox::currentIndexChanged, this, [this](int) { if (!Filling) applyFromWidgets(); });
    connect(Speed, &QSlider::valueChanged, this, [this](int V) {
        SpeedLabel->setText(tr("%1 px/s").arg(V * 100));
        if (!Filling) applyFromWidgets();
    });
    for (QComboBox *K : StickKeys) connect(K, &QComboBox::currentIndexChanged, this, [this](int) { if (!Filling) applyFromWidgets(); });
    connect(SaveButton, &QPushButton::clicked, this, [this] { QString E; if (!saveAs(NameEdit->text().trimmed(), &E)) setStatus(E, true); });
    connect(TryButton, &QPushButton::clicked, this, [this] { QString E; if (!tryNow(&E)) setStatus(E, true); });
    connect(UseButton, &QPushButton::clicked, this, [this] { QString E; if (!useForGame(&E)) setStatus(E, true); });
    connect(Close, &QPushButton::clicked, this, &QDialog::reject);
    //Deleting asks once more, on the same button (a controller has no confirmation box to aim at).
    connect(DeleteButton, &QPushButton::clicked, this, [this] {
        if (DeleteButton->property("armed").toBool())
        {
            const QString Gone = Q(Current.Name);
            Remove(Dir.toStdString(), Current.Name);
            DeleteButton->setProperty("armed", false);
            DeleteButton->setText(tr("Delete"));
            refillLayouts(QStringLiteral("Gamepad"));
            openLayout(QStringLiteral("Gamepad"));
            setStatus(tr("Deleted %1").arg(Gone));
            return;
        }
        DeleteButton->setProperty("armed", true);
        DeleteButton->setText(tr("Press again to delete"));
    });
}

void LayoutEditor::refillLayouts(const QString &Select)
{
    Filling = true;
    LayoutCombo->clear();
    for (const Layout &T : Templates()) LayoutCombo->addItem(tr("%1 (VidyaGod's)").arg(Q(T.Name)), Q(T.Name));
    const auto Mine = Saved(Dir.toStdString());
    if (!Mine.empty()) LayoutCombo->insertSeparator(LayoutCombo->count());
    for (const Layout &L : Mine) LayoutCombo->addItem(Q(L.Name), Q(L.Name));
    LayoutCombo->setCurrentIndex(std::max(0, LayoutCombo->findData(Select)));
    Filling = false;
}

bool LayoutEditor::openLayout(const QString &Name)
{
    const auto L = Named(Dir.toStdString(), Name.toStdString());
    if (!L) return false;
    Current = *L;
    Dirty = false;
    bool Builtin = false;
    for (const Layout &T : Templates()) Builtin |= T.Name == L->Name;
    //A built-in is never overwritten: its changes are saved under a name of the user's.
    NameEdit->setText(Builtin ? (GameTitle.isEmpty() ? tr("%1 (mine)").arg(Name) : tr("%1 — %2").arg(GameTitle, Name)) : Name);
    DeleteButton->setEnabled(!Builtin);
    DeleteButton->setProperty("armed", false);
    DeleteButton->setText(tr("Delete"));
    Description->setText(Q(L->Description));
    Filling = true;
    LayoutCombo->setCurrentIndex(std::max(0, LayoutCombo->findData(Name)));
    Filling = false;
    refreshTree();
    showBinding();
    setStatus({});
    return true;
}

void LayoutEditor::refreshTree()
{
    const QString Was = selectedControl();
    Filling = true;
    Tree->clear();
    QTreeWidgetItem *Group = nullptr;
    QString GroupName;
    for (const Control &C : Controls())
    {
        if (!Group || GroupName != Q(C.Group))
        {
            GroupName = Q(C.Group);
            Group = new QTreeWidgetItem(Tree, { GroupName });
            QFont F = Group->font(0);
            F.setBold(true);
            Group->setFont(0, F);
            Group->setFlags(Qt::ItemIsEnabled);   // a heading, not a stop
            Group->setFirstColumnSpanned(true);
        }
        auto *I = new QTreeWidgetItem(Group, { Q(C.Label), Q(Current.summary(C.Id)) });
        I->setData(0, Qt::UserRole, Q(C.Id));
        if (C.K == Kind::Gyro) I->setFlags(I->flags() & ~Qt::ItemIsEnabled);
    }
    Tree->expandAll();
    Filling = false;
    QStringList Bound;
    for (const Binding &B : Current.Bindings) Bound << Q(B.Control);
    Picture->setChanged(Bound);
    if (!Was.isEmpty()) selectControl(Was);
    if (GameKey.isEmpty()) UsedBy->clear();
    else
    {
        const QString Chosen = Q(ForGame(Dir.toStdString(), GameKey.toStdString()));
        UsedBy->setText(Chosen.isEmpty() ? tr("%1 uses: the controller as it is").arg(GameTitle) : tr("%1 uses: %2").arg(GameTitle, Chosen));
    }
}

QTreeWidgetItem *LayoutEditor::itemFor(const QString &Id) const
{
    for (int G = 0; G < Tree->topLevelItemCount(); ++G)
        for (int I = 0; I < Tree->topLevelItem(G)->childCount(); ++I)
            if (Tree->topLevelItem(G)->child(I)->data(0, Qt::UserRole).toString() == Id) return Tree->topLevelItem(G)->child(I);
    return nullptr;
}

bool LayoutEditor::selectControl(const QString &Id)
{
    QTreeWidgetItem *I = itemFor(Id);
    if (!I) return false;
    Filling = true;
    Tree->setCurrentItem(I);
    Filling = false;
    Picture->setSelected(Id);
    showBinding();
    return true;
}

QString LayoutEditor::selectedControl() const
{
    const QTreeWidgetItem *I = Tree ? Tree->currentItem() : nullptr;
    return I ? I->data(0, Qt::UserRole).toString() : QString();
}

namespace
{
void FillValues(QComboBox *C, const QString &Type, const QString &Select)
{
    C->clear();
    if (Type == QLatin1String("gamepad")) for (const std::string &B : GamepadButtons()) C->addItem(Q(GamepadLabel(B)), Q(B));
    else if (Type == QLatin1String("keyboard")) for (const std::string &K : KeyboardKeys()) C->addItem(Q(KeyLabel(K)), Q(K));
    else if (Type == QLatin1String("mouse")) for (const std::string &M : MouseButtons()) C->addItem(Q(MouseLabel(M)), Q(M));
    C->setEnabled(C->count() > 0);
    if (C->count()) C->setCurrentIndex(std::max(0, C->findData(Select)));
}
}

void LayoutEditor::showBinding()
{
    const QString Id = selectedControl();
    const Control *C = FindControl(Id.toStdString());
    if (!C) return;
    Filling = true;
    ControlTitle->setText(Q(C->Label));
    const Binding *B = Current.find(C->Id);
    if (C->K == Kind::Trackpad || C->K == Kind::Gyro)
    {
        Note->setText(C->K == Kind::Gyro ? tr("InputPlumber cannot map the gyro yet.")
                                         : tr("A trackpad is always the pointer. Its click and its touch are buttons: they are in the list."));
        Editors->setCurrentIndex(2);
    }
    else if (C->K == Kind::Stick)
    {
        Editors->setCurrentIndex(1);
        StickMode->setCurrentIndex(std::max(0, StickMode->findData(B ? Q(B->Mode) : QStringLiteral("joystick"))));
        Speed->setValue(int((B && B->Mode == "mouse" ? B->Speed : 1000) / 100));
        SpeedLabel->setText(tr("%1 px/s").arg(Speed->value() * 100));
        static const char *Default[4] = { "KeyW", "KeyS", "KeyA", "KeyD" };
        for (int I = 0; I < 4; ++I)
        {
            const QString K = B && B->Mode == "keys" && B->Keys.size() == 4 ? Q(B->Keys[size_t(I)]) : QString::fromLatin1(Default[I]);
            StickKeys[I]->setCurrentIndex(std::max(0, StickKeys[I]->findData(K)));
        }
        const QString M = StickMode->currentData().toString();
        Speed->setEnabled(M == QLatin1String("mouse"));
        for (QComboBox *K : StickKeys) K->setEnabled(M == QLatin1String("keys"));
    }
    else
    {
        Editors->setCurrentIndex(0);
        const bool Trigger = C->K == Kind::Trigger;
        TypeCombo->clear();
        TypeCombo->addItem(tr("As it is"), QStringLiteral("asis"));
        TypeCombo->addItem(tr("A pad button"), QStringLiteral("gamepad"));
        TypeCombo->addItem(tr("A keyboard key"), QStringLiteral("keyboard"));
        if (!Trigger) TypeCombo->addItem(tr("A mouse button"), QStringLiteral("mouse"));
        if (!Trigger) TypeCombo->addItem(tr("Open the overlay"), QStringLiteral("overlay"));
        TypeCombo->addItem(tr("Nothing"), QStringLiteral("nothing"));
        Type2Combo->clear();
        Type2Combo->addItem(tr("A pad button"), QStringLiteral("gamepad"));
        Type2Combo->addItem(tr("A keyboard key"), QStringLiteral("keyboard"));
        if (!Trigger) Type2Combo->addItem(tr("A mouse button"), QStringLiteral("mouse"));
        QString T = QStringLiteral("asis"), V, T2 = QStringLiteral("keyboard"), V2;
        if (B && B->Mode == "nothing") T = QStringLiteral("nothing");
        else if (B && !B->Actions.empty()) { T = Q(TypeName(B->Actions[0].T)); V = Q(B->Actions[0].Value); }
        const bool Two = B && B->Actions.size() > 1;
        if (Two) { T2 = Q(TypeName(B->Actions[1].T)); V2 = Q(B->Actions[1].Value); }
        TypeCombo->setCurrentIndex(std::max(0, TypeCombo->findData(T)));
        FillValues(ValueCombo, T, V);
        Chord->setChecked(Two);
        const bool CanChord = T == QLatin1String("gamepad") || T == QLatin1String("keyboard") || T == QLatin1String("mouse");
        Chord->setEnabled(CanChord);
        Type2Combo->setCurrentIndex(std::max(0, Type2Combo->findData(T2)));
        FillValues(Value2Combo, T2, V2);
        //The second action's rows only once it is asked for.
        ButtonForm->setRowVisible(Type2Combo, CanChord && Two);
        ButtonForm->setRowVisible(Value2Combo, CanChord && Two);
    }
    Filling = false;
}

void LayoutEditor::applyFromWidgets()
{
    const Control *C = FindControl(selectedControl().toStdString());
    if (!C || C->K == Kind::Trackpad || C->K == Kind::Gyro) return;
    Binding B;
    B.Control = C->Id;
    if (C->K == Kind::Stick)
    {
        B.Mode = StickMode->currentData().toString().toStdString();
        B.Speed = Speed->value() * 100.0;
        if (B.Mode == "keys") for (QComboBox *K : StickKeys) B.Keys.push_back(K->currentData().toString().toStdString());
    }
    else
    {
        const QString T = TypeCombo->currentData().toString();
        if (T == QLatin1String("nothing")) B.Mode = "nothing";
        else if (T != QLatin1String("asis"))
        {
            //A type changed: its value list follows (the first of it, until one is chosen).
            if (sender() == TypeCombo) { Filling = true; FillValues(ValueCombo, T, {}); Filling = false; }
            const auto Ty = TypeFrom(T.toStdString());
            if (Ty) B.Actions.push_back({ *Ty, T == QLatin1String("overlay") ? std::string() : ValueCombo->currentData().toString().toStdString() });
            if (Chord->isChecked() && Chord->isEnabled())
            {
                if (sender() == Type2Combo || Value2Combo->count() == 0) { Filling = true; FillValues(Value2Combo, Type2Combo->currentData().toString(), {}); Filling = false; }
                if (const auto T2 = TypeFrom(Type2Combo->currentData().toString().toStdString()))
                    B.Actions.push_back({ *T2, Value2Combo->currentData().toString().toStdString() });
            }
        }
    }
    QString E;
    if (!bindSelected(B, &E)) setStatus(E, true);
}

bool LayoutEditor::bindSelected(const Binding &B, QString *Error)
{
    if (B.Control != selectedControl().toStdString())
    {
        if (Error) *Error = tr("%1 is not the selected control").arg(Q(B.Control));
        return false;
    }
    if (const std::string E = Check(B); !E.empty())
    {
        if (Error) *Error = Q(E);
        showBinding();   // back to what it does
        return false;
    }
    Current.set(B);
    Dirty = true;
    if (QTreeWidgetItem *I = itemFor(Q(B.Control))) I->setText(1, Q(Current.summary(B.Control)));
    QStringList Bound;
    for (const Binding &X : Current.Bindings) Bound << Q(X.Control);
    Picture->setChanged(Bound);
    showBinding();
    setStatus(tr("Changed — not saved yet"));
    return true;
}

bool LayoutEditor::saveAs(const QString &Name, QString *Error)
{
    Layout L = Current;
    L.Name = Name.toStdString();
    std::string E;
    if (!Save(Dir.toStdString(), L, &E))
    {
        if (Error) *Error = Q(E);
        return false;
    }
    Current = L;
    Dirty = false;
    refillLayouts(Name);
    DeleteButton->setEnabled(true);
    setStatus(tr("Saved as %1").arg(Name));
    return true;
}

bool LayoutEditor::useForGame(QString *Error)
{
    if (GameKey.isEmpty()) { if (Error) *Error = tr("The editor was not opened for a game"); return false; }
    if (Dirty) { if (Error) *Error = tr("Save the changes first: a game uses a saved layout"); return false; }
    //"Gamepad" is the controller as it is: the game then has no layout of its own.
    const std::string Name = Current.Name == "Gamepad" ? std::string() : Current.Name;
    if (!SetForGame(Dir.toStdString(), GameKey.toStdString(), Name)) { if (Error) *Error = tr("Cannot write in %1").arg(Dir); return false; }
    refreshTree();
    setStatus(tr("%1 now uses %2").arg(GameTitle, Q(Current.Name)));
    return true;
}

bool LayoutEditor::tryNow(QString *Error)
{
    QString E;
    if (!ControllerService::instance().loadLayout(Q(Current.toProfile().dump()), &E)) { if (Error) *Error = E; return false; }
    setStatus(ControllerService::instance().running() ? tr("On the controller now") : tr("The controller service is not running: it takes this when it starts"));
    return true;
}

void LayoutEditor::setStatus(const QString &S, bool Bad)
{
    Status->setText(S);
    Status->setStyleSheet(Bad ? QStringLiteral("color:#e06c75;") : QString());
}

QString LayoutEditor::status() const { return Status->text(); }
