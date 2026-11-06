// `VidyaGod ui …` — the app's windows, seen and driven by name: the exact state of every widget (texts, check states,
// table cells and the checkboxes inside them, combo items, the card grids as painted, open dialogs) and every action
// a person takes on them (click, type, tick, pick, switch tab, open a card, answer a dialog). No accessibility bridge,
// no coordinates, no synthetic input: the widgets themselves.
#include "command.h"
#include "libraryview.h"
#include "uifind.h"

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QSlider>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QTreeWidget>
#include <sstream>

namespace UiFind
{

using json = nlohmann::ordered_json;

// A button's or tab's text as it reads on screen: Qt's shortcut marker gone ("&Save" → "Save", "A && B" → "A & B").
QString Shown(QString T)
{
    T.replace("&&", QChar(0x1));
    T.remove('&');
    return T.replace(QChar(0x1), '&');
}

// A tab's page answers to its tab's title ("Library", "Settings").
QString TabTitle(const QWidget *W)
{
    const QWidget *Stack = W->parentWidget();
    const auto *Tw = Stack ? qobject_cast<const QTabWidget *>(Stack->parentWidget()) : nullptr;
    const int I = Tw ? Tw->indexOf(const_cast<QWidget *>(W)) : -1;
    return I >= 0 ? Shown(Tw->tabText(I)) : QString();
}

std::string NameOf(const QWidget *W)
{
    if (!W->objectName().isEmpty()) return W->objectName().toStdString();
    if (!W->accessibleName().isEmpty()) return W->accessibleName().toStdString();
    if (W->isWindow() && !qobject_cast<const QMenu *>(W)) return W->metaObject()->className();   // a window: its kind
    return TabTitle(W).toStdString();
}

// W as Outer/Inner: its name under the nearest named control around it — what tells two controls of one name apart.
std::string QualifiedName(const QWidget *W)
{
    for (const QWidget *P = W->parentWidget(); P; P = P->parentWidget())
        if (const std::string N = NameOf(P); !N.empty() && N.rfind("qt_", 0) != 0) return N + "/" + NameOf(W);
    return NameOf(W);
}

json ItemData(const QAbstractItemModel *M, const QModelIndex &I)
{
    json Cell = M->data(I, Qt::DisplayRole).toString().toStdString();
    const QVariant Check = M->data(I, Qt::CheckStateRole);
    if (Check.isValid())
        Cell = {{"text", Cell}, {"checked", Check.toInt() == Qt::Checked}};
    return Cell;
}

json StateOf(const QWidget *W, int Depth, bool VisibleOnly);

// An item view's contents: headers and every row, a cell widget (the checkbox in a table cell) as its own state.
json ViewState(const QAbstractItemView *V)
{
    json Out = json::object();
    const QAbstractItemModel *M = V->model();
    if (!M) return Out;
    const QHeaderView *H = nullptr;   // a list has no header: its model's "1" is no column title
    if (const auto *T = qobject_cast<const QTableView *>(V)) H = T->horizontalHeader();
    else if (const auto *Tr = qobject_cast<const QTreeView *>(V)) H = Tr->header();
    if (H && !H->isHidden())
    {
        json Headers = json::array();
        for (int C = 0; C < M->columnCount(); ++C) Headers.push_back(M->headerData(C, Qt::Horizontal).toString().toStdString());
        Out["columns"] = Headers;
    }
    json Rows = json::array();
    std::function<void(const QModelIndex &, int)> Walk = [&](const QModelIndex &Parent, int Level) {
        for (int R = 0; R < M->rowCount(Parent); ++R)
        {
            json Row = json::array();
            for (int C = 0; C < M->columnCount(Parent); ++C)
            {
                const QModelIndex I = M->index(R, C, Parent);
                if (QWidget *Cw = V->indexWidget(I)) Row.push_back(StateOf(Cw, 2, false));
                else Row.push_back(ItemData(M, I));
            }
            Rows.push_back(Level ? json{{"level", Level}, {"cells", Row}} : Row);
            const QModelIndex First = M->index(R, 0, Parent);
            if (M->hasChildren(First)) Walk(First, Level + 1);
        }
    };
    Walk(QModelIndex(), 0);
    Out["rows"] = Rows;
    if (V->currentIndex().isValid()) Out["current"] = {V->currentIndex().row(), V->currentIndex().column()};
    return Out;
}

json StateOf(const QWidget *W, int Depth, bool VisibleOnly)
{
    json J = {{"class", W->metaObject()->className()}};
    if (const std::string N = NameOf(W); !N.empty()) J["name"] = N;
    if (!W->isVisible()) J["visible"] = false;
    if (!W->isEnabled()) J["enabled"] = false;
    if (!W->toolTip().isEmpty()) J["tooltip"] = W->toolTip().toStdString();
    if (const auto *B = qobject_cast<const QAbstractButton *>(W))
    {
        J["text"] = Shown(B->text()).toStdString();
        if (B->isCheckable()) J["checked"] = B->isChecked();
    }
    else if (const auto *L = qobject_cast<const QLabel *>(W)) J["text"] = L->text().toStdString();
    else if (const auto *E = qobject_cast<const QLineEdit *>(W))
    {
        J["text"] = E->text().toStdString();
        if (!E->placeholderText().isEmpty()) J["placeholder"] = E->placeholderText().toStdString();
        if (E->isReadOnly()) J["readonly"] = true;
    }
    else if (const auto *T = qobject_cast<const QTextEdit *>(W)) J["text"] = T->toPlainText().toStdString();
    else if (const auto *P = qobject_cast<const QPlainTextEdit *>(W)) J["text"] = P->toPlainText().toStdString();
    else if (const auto *C = qobject_cast<const QComboBox *>(W))
    {
        json Items = json::array();
        for (int I = 0; I < C->count(); ++I) Items.push_back(C->itemText(I).toStdString());
        J["items"] = Items;
        J["current"] = C->currentText().toStdString();
    }
    else if (const auto *S = qobject_cast<const QSpinBox *>(W)) { J["value"] = S->value(); J["range"] = {S->minimum(), S->maximum()}; }
    else if (const auto *D = qobject_cast<const QDoubleSpinBox *>(W)) { J["value"] = D->value(); J["range"] = {D->minimum(), D->maximum()}; }
    else if (const auto *Sl = qobject_cast<const QSlider *>(W)) { J["value"] = Sl->value(); J["range"] = {Sl->minimum(), Sl->maximum()}; }
    else if (const auto *Pr = qobject_cast<const QProgressBar *>(W)) { J["value"] = Pr->value(); J["range"] = {Pr->minimum(), Pr->maximum()}; }
    else if (const auto *G = qobject_cast<const QGroupBox *>(W))
    {
        J["title"] = G->title().toStdString();
        if (G->isCheckable()) J["checked"] = G->isChecked();
    }
    else if (const auto *Tw = qobject_cast<const QTabWidget *>(W))
    {
        json Tabs = json::array(), Hidden = json::array(), Off = json::array();
        for (int I = 0; I < Tw->count(); ++I)
        {
            const std::string Title = Shown(Tw->tabText(I)).toStdString();
            Tabs.push_back(Title);
            if (!Tw->isTabVisible(I)) Hidden.push_back(Title);
            else if (!Tw->isTabEnabled(I)) Off.push_back(Title);
        }
        J["tabs"] = Tabs;
        if (!Hidden.empty()) J["hidden_tabs"] = Hidden;
        if (!Off.empty()) J["disabled_tabs"] = Off;
        J["current"] = Shown(Tw->tabText(Tw->currentIndex())).toStdString();
    }
    else if (const auto *Lv = qobject_cast<const LibraryView *>(W)) J["grid"] = Lv->stateJson();
    if (const auto *V = qobject_cast<const QAbstractItemView *>(W)) { J["view"] = ViewState(V); return J; }   // its viewport is its contents
    if (qobject_cast<const LibraryView *>(W) || Depth == 0) return J;
    json Kids = json::array();
    for (const QObject *O : W->children())
    {
        const auto *C = qobject_cast<const QWidget *>(O);
        if (!C || C->isWindow() || (VisibleOnly && !C->isVisible())) continue;
        if (qobject_cast<const QTabBar *>(C) && qobject_cast<const QTabWidget *>(W)) continue;   // shown as "tabs"
        Kids.push_back(StateOf(C, Depth - 1, VisibleOnly));
    }
    if (!Kids.empty()) J["children"] = Kids;
    return J;
}

std::vector<QWidget *> Windows()
{
    std::vector<QWidget *> Out;
    for (QWidget *W : QApplication::topLevelWidgets())
        if (W->isVisible() || qobject_cast<QMainWindow *>(W) || W->isModal()) Out.push_back(W);
    return Out;
}

// A control by its name: exact objectName, accessible name or tab title first, then a unique case-insensitive part of
// a name. Two controls of one name resolve to the visible one; an ambiguity left is refused with the candidates, never
// a guess. "Outer/Inner" is a control inside another (a tab, a group, a dialog), for a name two places share.
QWidget *Named(const std::string &Name, const QWidget *Under)
{
    const QString Q = QString::fromStdString(Name);
    std::vector<QWidget *> Exact, Part;
    std::function<void(QWidget *)> Walk = [&](QWidget *W) {
        const QString O = W->objectName(), A = W->accessibleName(), T = TabTitle(W);
        //A window also answers to its kind (PackageEditor, PreLaunchWindow) and its title ("Age of Empires II").
        const QString K = W->isWindow() ? QString(W->metaObject()->className()) : QString();
        const QString Title = W->isWindow() ? W->windowTitle() : QString();
        if (O == Q || A == Q || (!T.isEmpty() && T == Q) || (!K.isEmpty() && K == Q) || (!Title.isEmpty() && Title == Q)) Exact.push_back(W);
        else if ((!O.isEmpty() && O.contains(Q, Qt::CaseInsensitive)) || (!A.isEmpty() && A.contains(Q, Qt::CaseInsensitive))
                 || (!Title.isEmpty() && Title.contains(Q, Qt::CaseInsensitive)))
            Part.push_back(W);
        for (QObject *C : W->children()) if (auto *Cw = qobject_cast<QWidget *>(C)) Walk(Cw);
    };
    if (Under) Walk(const_cast<QWidget *>(Under));
    else for (QWidget *W : QApplication::topLevelWidgets()) Walk(W);
    const auto Pick = [&](std::vector<QWidget *> &V) -> QWidget * {
        //A window with a parent is both a top-level widget and its parent's child: seen twice, still one control.
        std::sort(V.begin(), V.end());
        V.erase(std::unique(V.begin(), V.end()), V.end());
        std::vector<QWidget *> Visible;
        for (QWidget *W : V) if (W->isVisible()) Visible.push_back(W);
        std::vector<QWidget *> &Use = Visible.empty() ? V : Visible;
        if (Use.size() == 1) return Use.front();
        std::string Names;
        for (size_t I = 0; I < Use.size() && I < 8; ++I) Names += (I ? ", " : "") + QualifiedName(Use[I]);
        throw Command::BadArgs("'" + Name + "' names " + std::to_string(Use.size()) + " controls: " + Names);
    };
    if (!Exact.empty()) return Pick(Exact);
    if (const size_t Slash = Name.find('/'); Slash != std::string::npos && Slash > 0 && Slash + 1 < Name.size())
        return Named(Name.substr(Slash + 1), Named(Name.substr(0, Slash), Under));
    if (!Part.empty()) return Pick(Part);
    std::vector<std::string> All;
    std::function<void(const QWidget *)> Names = [&](const QWidget *W) {
        if (const std::string N = NameOf(W); !N.empty() && N.rfind("qt_", 0) != 0) All.push_back(N);
        if (!W->accessibleName().isEmpty()) All.push_back(W->accessibleName().toStdString());   // what the screen reads
        for (const QObject *C : W->children()) if (const auto *Cw = qobject_cast<const QWidget *>(C)) Names(Cw);
    };
    if (Under) Names(Under);
    else for (const QWidget *W : QApplication::topLevelWidgets()) Names(W);
    const std::string Near = Command::Nearest(Name, All);
    throw Command::BadArgs("no control named '" + Name + "'" + (Near.empty() ? "" : " — did you mean '" + Near + "'?")
                           + " (VidyaGod ui tree lists them)");
}

// A card grid by its name — or by the name of what holds exactly one (the Library tab: its "Library games").
LibraryView *Grid(const std::string &Name)
{
    QWidget *W = Named(Name);
    if (auto *V = qobject_cast<LibraryView *>(W)) return V;
    std::vector<LibraryView *> In;
    for (LibraryView *V : W->findChildren<LibraryView *>()) if (V->isVisible() || W->isHidden()) In.push_back(V);
    if (In.size() == 1) return In.front();
    throw Command::BadArgs("'" + Name + "' is not a card grid (they are 'Library games' and 'Catalog games')");
}

// ---- how a widget state reads for a person: an outline, one control a line, tables and grids drawn in place ----

std::string Paint(const std::string &S, const char *Code, bool Colour) { return Colour ? std::string("\033[") + Code + "m" + S + "\033[0m" : S; }

std::string CellText(const json &C)
{
    if (C.is_string()) return C.get<std::string>();
    if (C.is_object() && C.contains("checked") && !C.contains("class"))
        return std::string(C["checked"].get<bool>() ? "[x] " : "[ ] ") + C.value("text", std::string());
    if (C.is_object() && C.contains("checked")) return C["checked"].get<bool>() ? "[x]" : "[ ]";
    if (C.is_object() && C.contains("current") && C["current"].is_string()) return "▾ " + C["current"].get<std::string>();   // a combo
    if (C.is_object() && C.contains("value")) return C["value"].dump();
    if (C.is_object() && C.contains("children"))   // a cell widget holding one control (a centred checkbox)
        for (const auto &K : C["children"]) { const std::string T = CellText(K); if (!T.empty()) return T; }
    if (C.is_object() && C.contains("text")) return C["text"].get<std::string>();
    return C.is_object() ? std::string() : C.dump();
}

void Outline(std::ostringstream &O, const json &N, int Indent, bool Colour)
{
    const std::string Pad(static_cast<size_t>(Indent) * 2, ' ');
    std::string Line = Pad + Paint(N.value("class", std::string("?")), "2", Colour);
    if (N.contains("name")) Line += "  " + Paint("\"" + N["name"].get<std::string>() + "\"", "36", Colour);
    if (N.contains("checked")) Line += N["checked"].get<bool>() ? "  [x]" : "  [ ]";
    if (N.contains("text") && !N["text"].get<std::string>().empty() && N.value("name", std::string()) != N["text"].get<std::string>())
    {
        std::string T = N["text"].get<std::string>();
        for (char &Ch : T) if (Ch == '\n') Ch = ' ';
        if (T.size() > 90) T = T.substr(0, 89) + "…";
        Line += "  " + T;
    }
    if (N.contains("value")) Line += "  = " + N["value"].dump();
    if (N.contains("current") && N["current"].is_string()) Line += "  ▸ " + N["current"].get<std::string>();
    if (N.contains("tabs"))
    {
        const auto In = [&N](const char *K, const json &X) { const json L = N.value(K, json::array()); return std::find(L.begin(), L.end(), X) != L.end(); };
        std::string T;
        for (const auto &X : N["tabs"])
            T += (T.empty() ? "" : " · ") + X.get<std::string>()
                 + (In("hidden_tabs", X) ? Paint(" (hidden)", "2", Colour) : In("disabled_tabs", X) ? Paint(" (disabled)", "2", Colour) : "");
        Line += "  tabs: " + T;
    }
    if (N.contains("items")) Line += "  (" + std::to_string(N["items"].size()) + " items)";
    if (N.contains("title")) Line += "  " + N["title"].get<std::string>();
    if (N.contains("placeholder") && N.value("text", std::string()).empty()) Line += "  " + Paint("(" + N["placeholder"].get<std::string>() + ")", "2", Colour);
    if (N.value("visible", true) == false) Line += Paint("  hidden", "33", Colour);
    if (N.value("enabled", true) == false) Line += Paint("  disabled", "33", Colour);
    O << Line << "\n";
    if (N.contains("view"))
    {
        const json &V = N["view"];
        const json Cols = V.value("columns", json::array());
        std::vector<std::vector<std::string>> Grid;
        std::vector<std::string> Head;
        for (const auto &C : Cols) Head.push_back(C.get<std::string>());
        for (const auto &R : V.value("rows", json::array()))
        {
            const json &Cells = R.is_object() ? R["cells"] : R;
            std::vector<std::string> Row;
            for (const auto &C : Cells) Row.push_back(CellText(C));
            if (R.is_object() && !Row.empty())   // a tree's nested row: indented under its parent, as drawn
                Row.front() = std::string(static_cast<size_t>(R.value("level", 0)) * 2, ' ') + Row.front();
            Grid.push_back(std::move(Row));
        }
        size_t NCols = Head.size();
        for (const auto &R : Grid) NCols = std::max(NCols, R.size());
        std::vector<size_t> W(NCols, 0);
        for (size_t C = 0; C < Head.size(); ++C) W[C] = Head[C].size();
        for (const auto &R : Grid) for (size_t C = 0; C < R.size(); ++C) W[C] = std::max(W[C], std::min<size_t>(40, R[C].size()));
        bool Flat = true;   // the current row is marked where rows are not nested (its index is then its line)
        for (const auto &R : V.value("rows", json::array())) Flat = Flat && R.is_array();
        const long long Current = Flat && V.contains("current") ? V["current"][0].get<long long>() : -1;
        const std::string P2 = Pad + "  ";
        if (!Head.empty())
        {
            O << P2 << "  ";
            for (size_t C = 0; C < Head.size(); ++C) O << Paint(Head[C], "1", Colour) << std::string(W[C] - Head[C].size() + 2, ' ');
            O << "\n";
        }
        for (size_t I = 0; I < Grid.size(); ++I)
        {
            const auto &R = Grid[I];
            O << P2 << (static_cast<long long>(I) == Current ? Paint("▸ ", "36", Colour) : "  ");
            for (size_t C = 0; C < R.size() && C < W.size(); ++C)
            {
                const std::string T = R[C].size() > 40 ? R[C].substr(0, 39) + "…" : R[C];
                O << T << std::string(W[C] > T.size() ? W[C] - T.size() + 2 : 2, ' ');
            }
            O << "\n";
        }
        O << P2 << Paint(std::to_string(Grid.size()) + " rows", "2", Colour) << "\n";
    }
    if (N.contains("grid"))
    {
        const json &G = N["grid"];
        const json &Cards = G["cards"];
        const std::string P2 = Pad + "  ";
        for (const auto &C : Cards)
        {
            std::string L = P2 + C.value("title", std::string()) + Paint("  [" + C.value("tile", std::string()) + "]", "2", Colour);
            for (const char *Badge : { "local", "golden", "downloading" }) if (C.value(Badge, false)) L += Paint(std::string("  ") + Badge, "33", Colour);
            if (C.contains("percent")) L += " " + std::to_string(static_cast<int>(C["percent"].get<double>())) + "%";
            if (C.contains("section")) L += Paint("  · " + C["section"].get<std::string>(), "2", Colour);
            if (!C.value("shown", true)) L += Paint("  (collapsed)", "2", Colour);
            O << L << "\n";
            for (const auto &S : C.value("secondaries", json::array()))
                O << P2 << "  └ " << S.value("title", std::string()) << Paint("  [" + S.value("tile", std::string()) + "]", "2", Colour) << "\n";
        }
        O << P2 << Paint(std::to_string(Cards.size()) + " cards", "2", Colour)
          << (G.contains("empty_message") ? "  " + G["empty_message"].get<std::string>() : std::string()) << "\n";
    }
    for (const auto &K : N.value("children", json::array())) Outline(O, K, Indent + 1, Colour);
}

// " (its items: a, b, c)" — what a combo box offers, for the error naming an item it does not have.
std::string Items(const QComboBox *C)
{
    std::string L;
    for (int I = 0; I < C->count() && I < 30; ++I) L += (I ? ", " : "") + C->itemText(I).toStdString();
    if (C->count() > 30) L += ", …";
    return C->count() ? " (its items: " + L + ")" : " (it has none)";
}

// A dialog as ui dialogs and an action that opened it report it: title, text, buttons, and its controls' state.
json DialogJson(QDialog *D)
{
    json J = {{"title", D->windowTitle().toStdString()}, {"class", D->metaObject()->className()}, {"modal", D->isModal()}};
    if (auto *M = qobject_cast<QMessageBox *>(D))
    {
        J["text"] = M->text().toStdString();
        if (!M->informativeText().isEmpty()) J["informative"] = M->informativeText().toStdString();
        if (!M->detailedText().isEmpty()) J["details"] = M->detailedText().toStdString();
    }
    json Buttons = json::array();
    for (QAbstractButton *B : D->findChildren<QAbstractButton *>())
        if (B->isVisible() && !B->text().isEmpty()) Buttons.push_back(Shown(B->text()).toStdString());
    J["buttons"] = Buttons;
    J["state"] = StateOf(D, 1000, true);
    return J;
}

// A popup menu on screen (a card's or a row's context menu): its title and enabled items.
json MenuJson(QMenu *M)
{
    json Items = json::array();
    for (QAction *A : M->actions())
        if (!A->isSeparator() && A->isVisible()) Items.push_back(json{{"text", Shown(A->text()).toStdString()}, {"enabled", A->isEnabled()}});
    return {{"menu", Shown(M->title()).toStdString()}, {"items", Items}};
}

std::vector<QWidget *> OpenDialogs()
{
    std::vector<QWidget *> Out;
    for (QWidget *W : QApplication::topLevelWidgets())
        if (W->isVisible() && (qobject_cast<QDialog *>(W) || qobject_cast<QMenu *>(W))) Out.push_back(W);
    return Out;
}

// Runs a person's action (a click, a value typed, a card opened) and returns After()'s state — unless the action opens
// a dialog or a menu that waits for an answer (a confirmation's exec()): then the call is answered at once with what
// opened (Context::Early), and ui press answers it; the action finishes when the dialog closes. A dialog the action
// opened without waiting (shown, not exec'd) is listed in the result.
json Act(Command::Context &Ctx, const std::function<void()> &F, const std::function<json()> &After)
{
    const std::vector<QWidget *> Before = OpenDialogs();
    const auto IsNew = [&Before](QWidget *W) { return std::find(Before.begin(), Before.end(), W) == Before.end(); };
    const auto Opened = [&] {
        json Out = json::array();
        for (QWidget *W : OpenDialogs())
            if (IsNew(W)) Out.push_back(qobject_cast<QMenu *>(W) ? MenuJson(qobject_cast<QMenu *>(W)) : DialogJson(qobject_cast<QDialog *>(W)));
        return Out;
    };
    QTimer Watch;   // fires inside the dialog's own event loop
    Watch.setInterval(25);
    QObject::connect(&Watch, &QTimer::timeout, [&] {
        const json Now = Opened();
        if (Now.empty() || !Ctx.Early) return;
        Watch.stop();
        Ctx.Early(json{{"opened", Now}, {"waiting", true},
                       {"note", "the action waits for an answer: VidyaGod ui press <button> gives it"}});
    });
    Watch.start();
    F();
    Watch.stop();
    json Out = After();
    if (const json Left = Opened(); !Left.empty())
    {
        if (!Out.is_object()) Out = json{{"state", Out}};
        Out["opened"] = Left;
    }
    return Out;
}

std::string RenderTree(const json &R, bool Colour)
{
    std::ostringstream O;
    if (R.is_array()) for (const auto &W : R) Outline(O, W, 0, Colour);
    else Outline(O, R, 0, Colour);
    return O.str();
}

} // namespace UiFind

using namespace UiFind;

VG_COMMANDS
{
    using Command::Type;
    Command::RegisterGroup("ui", "the app's windows: see their exact state, act on any control");

    Command::Register({
        "ui tree", "the exact state of every window and control",
        "Every open window as a tree of its controls: each one's class, name (what ui commands address it by), text, "
        "check state, value, items, every table cell (with the checkbox inside a cell), and the card grids exactly as "
        "painted. Hidden controls are left out unless --all.",
        { Command::Opt("under", "only this control and what is inside it"),
          Command::Opt("depth", "levels to descend (default: all)", Type::Int),
          Command::Flag("all", "include hidden controls") },
        { "ui tree", "ui tree --under Network", "ui tree --under 'Library' --depth 2 --json" },
        { "QWidget::children" }, Command::Needs::Ui, false,
        [](Command::Context &, const json &A) {
            const int Depth = static_cast<int>(Command::Int(A, "depth", 1000));
            const bool All = Command::Bool(A, "all", false);
            if (A.contains("under")) return StateOf(Named(Command::Str(A, "under")), Depth, !All);
            json Out = json::array();
            for (QWidget *W : Windows()) Out.push_back(StateOf(W, Depth, !All));
            return Out;
        }, &RenderTree });

    Command::Register({
        "ui show", "one control's state (a table's rows, a grid's cards)",
        "The exact state of one control, named as ui tree names it: a button's text and check state, a table's columns "
        "and rows (and each cell's checkbox), a card grid's cards with their badges, secondaries and sections.",
        { Command::Pos("control", "the control's name (or a unique part of it)") },
        { "ui show 'Library'", "ui show Network --json" },
        { "UiFind::Named", "UiFind::NameOf", "UiFind::StateOf" , "LibraryView::stateJson" }, Command::Needs::Ui, false,
        [](Command::Context &, const json &A) { return StateOf(Named(Command::Str(A, "control")), 1, true); }, &RenderTree });

    Command::Register({
        "ui tab", "switch to a tab", "Make a tab current, exactly as clicking its title does.",
        { Command::Pos("tab", "the tab's title (Library, Catalog, Settings, IPFS, Network)") },
        { "ui tab Network" }, { "QTabWidget::setCurrentIndex" , "IpfsTab::setActive", "NetworkTab::setActive", "SettingsTab::showPage" }, Command::Needs::Ui, false,
        [](Command::Context &Ctx, const json &A) {
            const QString T = QString::fromStdString(Command::Str(A, "tab"));
            std::function<std::pair<QTabWidget *, int>(QWidget *)> Find = [&](QWidget *W) -> std::pair<QTabWidget *, int> {
                if (auto *Tw = qobject_cast<QTabWidget *>(W))
                    for (int I = 0; I < Tw->count(); ++I)
                        if (Shown(Tw->tabText(I)).compare(T, Qt::CaseInsensitive) == 0) return { Tw, I };
                for (QObject *C : W->children()) if (auto *Cw = qobject_cast<QWidget *>(C)) if (auto R = Find(Cw); R.first) return R;
                return { nullptr, -1 };
            };
            for (QWidget *W : Windows())
                if (const auto [Tw, I] = Find(W); Tw)
                    return Act(Ctx, [Tw = Tw, I = I] { Tw->setCurrentIndex(I); }, [&] { return json{{"tab", T.toStdString()}}; });
            throw Command::BadArgs("no tab '" + T.toStdString() + "'");
        }, {} });

    Command::Register({
        "ui click", "press a button (or any clickable control)",
        "Click a button, a checkbox, a radio button or a tool button by name — the control's own click(), so "
        "everything connected to it runs exactly as when a person clicks.",
        { Command::Pos("control", "the control's name") },
        { "ui click 'Verify  Publish'", "ui click 'You Send request'" }, { "QAbstractButton::click" }, Command::Needs::Ui, false,
        [](Command::Context &Ctx, const json &A) {
            QWidget *W = Named(Command::Str(A, "control"));
            auto *B = qobject_cast<QAbstractButton *>(W);
            if (!B) throw Command::BadArgs("'" + NameOf(W) + "' is a " + W->metaObject()->className() + ", not something to click");
            if (!B->isEnabled()) throw std::runtime_error("'" + NameOf(W) + "' is disabled");
            QPointer<QAbstractButton> P(B);
            return Act(Ctx, [P] { if (P) P->click(); }, [P] { return P ? StateOf(P, 0, false) : json{{"clicked", true}}; });
        }, {} });

    Command::Register({
        "ui check", "tick or untick a checkbox (or any checkable control)",
        "Set a checkable control on (default) or off — only toggling it when it differs, as a person would.",
        { Command::Pos("control", "the control's name"),
          Command::Flag("on", "on (default) or --no-on for off") },
        { "ui check 'Auto-accept peer requests'", "ui check 'Close to tray' --no-on" },
        { "QAbstractButton::setChecked" }, Command::Needs::Ui, false,
        [](Command::Context &Ctx, const json &A) {
            QWidget *W = Named(Command::Str(A, "control"));
            const bool On = Command::Bool(A, "on", true);
            if (auto *B = qobject_cast<QAbstractButton *>(W); B && B->isCheckable())
            {
                if (!B->isEnabled()) throw std::runtime_error("'" + NameOf(W) + "' is disabled");
                QPointer<QAbstractButton> P(B);
                return Act(Ctx, [P, On] { if (P && P->isChecked() != On) P->click(); },
                           [P] { return P ? StateOf(P, 0, false) : json{{"clicked", true}}; });
            }
            if (auto *G = qobject_cast<QGroupBox *>(W); G && G->isCheckable()) { G->setChecked(On); return StateOf(G, 0, false); }
            throw Command::BadArgs("'" + NameOf(W) + "' cannot be ticked");
        }, {} });

    Command::Register({
        "ui set", "type into a field, pick an item, set a value",
        "Set a control's value as a person would: the text of a field (then Enter), the item of a combo box or the row "
        "of a list (by its text), the value of a spin box or slider.",
        { Command::Pos("control", "the control's name"), Command::Pos("value", "the new value"),
          Command::Flag("enter", "press Enter in a text field afterwards (default on)") },
        { "ui set 'Add a peer' 12D3KooW…", "ui set 'Max simultaneous downloads' 4", "ui set 'Settings pages' Network" },
        { "QLineEdit::setText", "QComboBox::setCurrentIndex", "QSpinBox::setValue", "QAbstractItemView::setCurrentIndex" }, Command::Needs::Ui, false,
        [](Command::Context &Ctx, const json &A) {
            QWidget *W = Named(Command::Str(A, "control"));
            const QString V = QString::fromStdString(Command::Str(A, "value"));
            if (!W->isEnabled()) throw std::runtime_error("'" + NameOf(W) + "' is disabled");
            //What it sets is checked first (an unknown item is an error); then the setting — which may open a dialog —
            //runs as an action.
            std::function<void()> Do;
            QPointer<QWidget> P(W);
            const bool Enter = Command::Bool(A, "enter", true);
            if (auto *E = qobject_cast<QLineEdit *>(W))
                Do = [E, V, Enter] {
                    E->setText(V);
                    emit E->textEdited(V);
                    if (Enter) { emit E->returnPressed(); emit E->editingFinished(); }
                };
            else if (auto *T = qobject_cast<QPlainTextEdit *>(W)) Do = [T, V] { T->setPlainText(V); };
            else if (auto *Tx = qobject_cast<QTextEdit *>(W)) Do = [Tx, V] { Tx->setPlainText(V); };
            else if (auto *C = qobject_cast<QComboBox *>(W))
            {
                const int I = C->findText(V, Qt::MatchFixedString);
                if (I < 0) throw Command::BadArgs("'" + NameOf(W) + "' has no item '" + V.toStdString() + "'" + Items(C));
                Do = [C, I] { C->setCurrentIndex(I); emit C->activated(I); };
            }
            else if (auto *S = qobject_cast<QSpinBox *>(W)) Do = [S, V] { S->setValue(V.toInt()); };
            else if (auto *D = qobject_cast<QDoubleSpinBox *>(W)) Do = [D, V] { D->setValue(V.toDouble()); };
            else if (auto *Sl = qobject_cast<QSlider *>(W)) Do = [Sl, V] { Sl->setValue(V.toInt()); };
            else if (auto *Lv = qobject_cast<QAbstractItemView *>(W); Lv && Lv->model())   // a list: click its row of that text
            {
                const QModelIndexList Hit = Lv->model()->match(Lv->model()->index(0, 0), Qt::DisplayRole, V, 1, Qt::MatchFixedString);
                if (Hit.isEmpty()) throw Command::BadArgs("'" + NameOf(W) + "' has no row '" + V.toStdString() + "'");
                const QPersistentModelIndex Row = Hit.front();
                Do = [Lv, Row] { if (Row.isValid()) { Lv->setCurrentIndex(Row); emit Lv->clicked(Row); } };
            }
            else throw Command::BadArgs("'" + NameOf(W) + "' takes no value");
            return Act(Ctx, [P, Do] { if (P) Do(); }, [P] { return P ? StateOf(P, 0, false) : json{{"set", true}}; });
        }, {} });

    Command::Register({
        "ui cell", "tick a checkbox inside a table cell, or select a cell",
        "A table's cell by row and column (0-based, or a column's header text): the checkbox inside it is set on "
        "(default) or off, exactly as clicking it; a cell without one is selected (and activated with --activate).",
        { Command::Pos("table", "the table's name"), Command::Pos("row", "the row: its number, or the text of its first cell"),
          Command::Pos("column", "the column: its number, or its header text"),
          Command::Flag("on", "on (default) or --no-on"),
          Command::Flag("activate", "also activate the cell (a double click)") },
        { "ui cell Peers LGZ-MacBook Accepted", "ui cell Peers LGZ-MacBook 'Share: VidyaGod' --no-on" },
        { "QAbstractItemView::indexWidget", "QAbstractItemView::setCurrentIndex" }, Command::Needs::Ui, false,
        [](Command::Context &Ctx, const json &A) {
            QWidget *W = Named(Command::Str(A, "table"));
            auto *V = qobject_cast<QAbstractItemView *>(W);
            if (!V || !V->model()) throw Command::BadArgs("'" + NameOf(W) + "' is not a table");
            QAbstractItemModel *M = V->model();
            const auto Number = [](const std::string &S, int &Out) {
                if (S.empty() || S.find_first_not_of("0123456789") != std::string::npos) return false;
                Out = std::stoi(S);
                return true;
            };
            int R = -1, C = -1;
            const std::string Rs = Command::Str(A, "row"), Cs = Command::Str(A, "column");
            if (!Number(Rs, R))
                for (int I = 0; I < M->rowCount(); ++I)
                    if (M->data(M->index(I, 0)).toString().startsWith(QString::fromStdString(Rs), Qt::CaseInsensitive)) { R = I; break; }
            if (!Number(Cs, C))
                for (int I = 0; I < M->columnCount(); ++I)
                    if (M->headerData(I, Qt::Horizontal).toString().compare(QString::fromStdString(Cs), Qt::CaseInsensitive) == 0) { C = I; break; }
            if (R < 0 || R >= M->rowCount()) throw Command::BadArgs("no row '" + Rs + "'");
            if (C < 0 || C >= M->columnCount()) throw Command::BadArgs("no column '" + Cs + "'");
            const QModelIndex I = M->index(R, C);
            if (QWidget *Cell = V->indexWidget(I))
            {
                QAbstractButton *B = qobject_cast<QAbstractButton *>(Cell);
                if (!B) B = Cell->findChild<QAbstractButton *>();
                QPointer<QAbstractButton> P(B);
                if (B && B->isCheckable())
                {
                    if (!B->isEnabled()) throw std::runtime_error("that cell's checkbox is disabled");
                    const bool On = Command::Bool(A, "on", true);
                    return Act(Ctx, [P, On] { if (P && P->isChecked() != On) P->click(); },
                               [P, R, C] { return json{{"row", R}, {"column", C}, {"checked", P && P->isChecked()}}; });
                }
                if (B) return Act(Ctx, [P] { if (P) P->click(); }, [R, C] { return json{{"row", R}, {"column", C}, {"clicked", true}}; });
            }
            if (M->flags(I) & Qt::ItemIsUserCheckable)
            {
                M->setData(I, Command::Bool(A, "on", true) ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
                return json{{"row", R}, {"column", C}, {"checked", M->data(I, Qt::CheckStateRole).toInt() == Qt::Checked}};
            }
            V->setCurrentIndex(I);
            if (Command::Bool(A, "activate", false))
            {
                const QPersistentModelIndex At = I;
                QPointer<QAbstractItemView> Pv(V);
                return Act(Ctx, [Pv, At] { if (Pv && At.isValid()) { emit Pv->activated(At); emit Pv->doubleClicked(At); } },
                           [R, C] { return json{{"row", R}, {"column", C}, {"selected", true}, {"activated", true}}; });
            }
            return json{{"row", R}, {"column", C}, {"selected", true}};
        }, {} });

    Command::Register({
        "ui card", "open a game card (play, download, cancel, or its menu)",
        "Act on a card of a card grid as a person does: activate it (Library: play; Catalog: download, or cancel the "
        "download in flight) or open its context menu (--menu). The card is named by its tile UID or its title.",
        { Command::Pos("grid", "the grid's name (ui tree shows it)"), Command::Pos("card", "the card's tile UID or title"),
          Command::Flag("menu", "open its context menu instead") },
        { "ui card 'Library games' 'Age of Empires II - The Age of Kings'", "ui card 'Catalog games' 749 --json", "ui card Library 749 --menu" },
        { "LibraryView::activateCardAt", "LibraryView::requestCardMenu", "LibraryView::cardIndex" }, Command::Needs::Ui, false,
        [](Command::Context &Ctx, const json &A) {
            LibraryView *V = Grid(Command::Str(A, "grid"));
            const int I = V->cardIndex(QString::fromStdString(Command::Str(A, "card")));
            if (I < 0) throw Command::BadArgs("no single card '" + Command::Str(A, "card") + "'"
                                              + V->cardMiss(QString::fromStdString(Command::Str(A, "card"))).toStdString());
            QPointer<LibraryView> P(V);
            const bool Menu = Command::Bool(A, "menu", false);
            return Act(Ctx, [P, I, Menu] { if (!P) return; if (Menu) P->requestCardMenu(I); else P->activateCardAt(I); },
                       [I] { return json{{"card", I}}; });
        }, {} });

    Command::Register({
        "ui dialogs", "the open dialogs and message boxes, with their text and buttons",
        "Every dialog on screen: its title, its text (a message box's message and details), its buttons — what "
        "ui press answers — and any popup menu with its items.", {}, { "ui dialogs" }, {}, Command::Needs::Ui, false,
        [](Command::Context &, const json &) {
            json Out = json::array();
            for (QWidget *W : OpenDialogs())
                Out.push_back(qobject_cast<QMenu *>(W) ? MenuJson(qobject_cast<QMenu *>(W)) : DialogJson(qobject_cast<QDialog *>(W)));
            return Out;
        }, {} });

    Command::Register({
        "ui press", "press a button of the open dialog",
        "Answer the dialog on top (a message box, a confirmation) by its button's text — what a person clicks to "
        "close it — or pick a popup menu's item by its text (Escape closes it). An action that opens a dialog answers at once with it, so "
        "this reaches it.",
        { Command::Pos("button", "the button's text (OK, Cancel, Yes, …)"),
          Command::Opt("dialog", "the dialog's title, when several are open") },
        { "ui press OK", "ui press Yes --dialog 'Remove friend'", "ui press Escape" }, { "QAbstractButton::click" }, Command::Needs::Ui, false,
        [](Command::Context &, const json &A) {
            const QString Want = QString::fromStdString(Command::Str(A, "button")), Title = QString::fromStdString(Command::Str(A, "dialog", ""));
            for (QWidget *W : QApplication::topLevelWidgets())
            {
                if (auto *M = qobject_cast<QMenu *>(W); M && M->isVisible() && Want.compare("Escape", Qt::CaseInsensitive) == 0)
                {
                    QPointer<QMenu> Pm(M);
                    QTimer::singleShot(0, M, [Pm] { if (Pm) Pm->close(); });   // closed, nothing picked
                    return json{{"menu", Shown(M->title()).toStdString()}, {"pressed", "Escape"}};
                }
                if (auto *M = qobject_cast<QMenu *>(W); M && M->isVisible())   // a popup menu: its item of that text
                    for (QAction *Item : M->actions())
                        if (!Item->isSeparator() && Item->isEnabled() && Shown(Item->text()).compare(Want, Qt::CaseInsensitive) == 0)
                        {
                            QPointer<QMenu> Pm(M);
                            QPointer<QAction> Pa(Item);
                            QTimer::singleShot(0, M, [Pm, Pa] { if (Pa) Pa->trigger(); if (Pm) Pm->close(); });
                            return json{{"menu", Shown(M->title()).toStdString()}, {"pressed", Shown(Item->text()).toStdString()}};
                        }
                auto *D = qobject_cast<QDialog *>(W);
                if (!D || !D->isVisible() || (!Title.isEmpty() && !D->windowTitle().contains(Title, Qt::CaseInsensitive))) continue;
                for (QAbstractButton *B : D->findChildren<QAbstractButton *>())
                    if (B->isVisible() && Shown(B->text()).compare(Want, Qt::CaseInsensitive) == 0)
                    {
                        // Deferred: a dialog's exec() returns into the command that opened it, not into this one.
                        QTimer::singleShot(0, B, &QAbstractButton::click);
                        return json{{"dialog", D->windowTitle().toStdString()}, {"pressed", Shown(B->text()).toStdString()}};
                    }
            }
            throw Command::BadArgs("no open dialog has a '" + Want.toStdString() + "' button (ui dialogs lists them)");
        }, {} });

    Command::Register({
        "ui screenshot", "save a window (or one control) as a PNG",
        "What a control looks like, rendered by the widget itself — offscreen as well as on screen.",
        { Command::Pos("file", "the PNG to write"), Command::Opt("control", "one control or window — by its name, kind or title (default: the main window)") },
        { "ui screenshot /tmp/vg.png", "ui screenshot net.png --control Network", "ui screenshot editor.png --control PackageEditor" }, { "QWidget::grab" }, Command::Needs::Ui, false,
        [](Command::Context &Ctx, const json &A) {
            QWidget *W = A.contains("control") ? Named(Command::Str(A, "control")) : Ctx.Window;
            const QString F = QString::fromStdString(Command::Str(A, "file"));
            const QPixmap P = W->grab();
            if (!P.save(F, "PNG")) throw std::runtime_error("cannot write " + F.toStdString());
            return json{{"file", F.toStdString()}, {"width", P.width()}, {"height", P.height()}};
        }, {} });
}
