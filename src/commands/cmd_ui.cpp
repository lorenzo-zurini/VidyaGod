// `VidyaGod ui …` — the app's windows, seen and driven by name: the exact state of every widget (texts, check states,
// table cells and the checkboxes inside them, combo items, the card grids as painted, open dialogs) and every action
// a person takes on them (click, type, tick, pick, switch tab, open a card, answer a dialog). No accessibility bridge,
// no coordinates, no synthetic input: the widgets themselves.
#include "command.h"
#include "libraryview.h"

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
#include <QMessageBox>
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

namespace
{

using json = nlohmann::ordered_json;

std::string NameOf(const QWidget *W)
{
    if (!W->objectName().isEmpty()) return W->objectName().toStdString();
    return W->accessibleName().toStdString();
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
    json Headers = json::array();
    for (int C = 0; C < M->columnCount(); ++C) Headers.push_back(M->headerData(C, Qt::Horizontal).toString().toStdString());
    Out["columns"] = Headers;
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
        J["text"] = B->text().toStdString();
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
        json Tabs = json::array();
        for (int I = 0; I < Tw->count(); ++I) Tabs.push_back(Tw->tabText(I).toStdString());
        J["tabs"] = Tabs;
        J["current"] = Tw->tabText(Tw->currentIndex()).toStdString();
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

// A control by its name: exact objectName or accessible name first, then a unique case-insensitive part of one. An
// ambiguous name is refused with the candidates — never a guess.
QWidget *Named(const std::string &Name, const QWidget *Under = nullptr)
{
    const QString Q = QString::fromStdString(Name);
    std::vector<QWidget *> Exact, Part;
    std::function<void(QWidget *)> Walk = [&](QWidget *W) {
        const QString O = W->objectName(), A = W->accessibleName();
        if (O == Q || A == Q) Exact.push_back(W);
        else if ((!O.isEmpty() && O.contains(Q, Qt::CaseInsensitive)) || (!A.isEmpty() && A.contains(Q, Qt::CaseInsensitive)))
            Part.push_back(W);
        for (QObject *C : W->children()) if (auto *Cw = qobject_cast<QWidget *>(C)) Walk(Cw);
    };
    if (Under) Walk(const_cast<QWidget *>(Under));
    else for (QWidget *W : QApplication::topLevelWidgets()) Walk(W);
    const auto Pick = [&](std::vector<QWidget *> &V) -> QWidget * {
        std::vector<QWidget *> Visible;
        for (QWidget *W : V) if (W->isVisible()) Visible.push_back(W);
        std::vector<QWidget *> &Use = Visible.empty() ? V : Visible;
        if (Use.size() == 1) return Use.front();
        std::string Names;
        for (size_t I = 0; I < Use.size() && I < 8; ++I) Names += (I ? ", " : "") + NameOf(Use[I]);
        throw Command::BadArgs("'" + Name + "' names " + std::to_string(Use.size()) + " controls: " + Names);
    };
    if (!Exact.empty()) return Pick(Exact);
    if (!Part.empty()) return Pick(Part);
    throw Command::BadArgs("no control named '" + Name + "' (VidyaGod ui tree lists them)");
}

LibraryView *Grid(const std::string &Name)
{
    auto *V = qobject_cast<LibraryView *>(Named(Name));
    if (!V) throw Command::BadArgs("'" + Name + "' is not a card grid");
    return V;
}

// ---- how a widget state reads for a person: an outline, one control a line, tables and grids drawn in place ----

std::string Paint(const std::string &S, const char *Code, bool Colour) { return Colour ? std::string("\033[") + Code + "m" + S + "\033[0m" : S; }

std::string CellText(const json &C)
{
    if (C.is_string()) return C.get<std::string>();
    if (C.is_object() && C.contains("checked") && !C.contains("class"))
        return std::string(C["checked"].get<bool>() ? "[x] " : "[ ] ") + C.value("text", std::string());
    if (C.is_object() && C.contains("checked")) return C["checked"].get<bool>() ? "[x]" : "[ ]";
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
    if (N.contains("tabs")) { std::string T; for (const auto &X : N["tabs"]) T += (T.empty() ? "" : " · ") + X.get<std::string>(); Line += "  tabs: " + T; }
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
            Grid.push_back(std::move(Row));
        }
        std::vector<size_t> W(Head.size(), 0);
        for (size_t C = 0; C < Head.size(); ++C) W[C] = Head[C].size();
        for (const auto &R : Grid) for (size_t C = 0; C < R.size() && C < W.size(); ++C) W[C] = std::max(W[C], std::min<size_t>(40, R[C].size()));
        const std::string P2 = Pad + "  ";
        O << P2;
        for (size_t C = 0; C < Head.size(); ++C) O << Paint(Head[C], "1", Colour) << std::string(W[C] - Head[C].size() + 2, ' ');
        O << "\n";
        for (const auto &R : Grid)
        {
            O << P2;
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

std::string RenderTree(const json &R, bool Colour)
{
    std::ostringstream O;
    if (R.is_array()) for (const auto &W : R) Outline(O, W, 0, Colour);
    else Outline(O, R, 0, Colour);
    return O.str();
}

} // namespace

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
        { "ui show 'Library'", "ui show Networking --json" },
        {}, Command::Needs::Ui, false,
        [](Command::Context &, const json &A) { return StateOf(Named(Command::Str(A, "control")), 1, true); }, &RenderTree });

    Command::Register({
        "ui tab", "switch to a tab", "Make a tab current, exactly as clicking its title does.",
        { Command::Pos("tab", "the tab's title (Library, Catalog, Settings, IPFS, Network)") },
        { "ui tab Network" }, { "QTabWidget::setCurrentIndex" }, Command::Needs::Ui, false,
        [](Command::Context &, const json &A) {
            const QString T = QString::fromStdString(Command::Str(A, "tab"));
            std::function<QTabWidget *(QWidget *)> Find = [&](QWidget *W) -> QTabWidget * {
                if (auto *Tw = qobject_cast<QTabWidget *>(W))
                    for (int I = 0; I < Tw->count(); ++I)
                        if (Tw->tabText(I).compare(T, Qt::CaseInsensitive) == 0) { Tw->setCurrentIndex(I); return Tw; }
                for (QObject *C : W->children()) if (auto *Cw = qobject_cast<QWidget *>(C)) if (auto *R = Find(Cw)) return R;
                return nullptr;
            };
            for (QWidget *W : Windows()) if (Find(W)) return json{{"tab", T.toStdString()}};
            throw Command::BadArgs("no tab '" + T.toStdString() + "'");
        }, {} });

    Command::Register({
        "ui click", "press a button (or any clickable control)",
        "Click a button, a checkbox, a radio button or a tool button by name — the control's own click(), so "
        "everything connected to it runs exactly as when a person clicks.",
        { Command::Pos("control", "the control's name") },
        { "ui click 'Verify  Publish'", "ui click 'You Send request'" }, { "QAbstractButton::click" }, Command::Needs::Ui, false,
        [](Command::Context &, const json &A) {
            QWidget *W = Named(Command::Str(A, "control"));
            auto *B = qobject_cast<QAbstractButton *>(W);
            if (!B) throw Command::BadArgs("'" + NameOf(W) + "' is a " + W->metaObject()->className() + ", not something to click");
            if (!B->isEnabled()) throw std::runtime_error("'" + NameOf(W) + "' is disabled");
            B->click();
            return StateOf(B, 0, false);
        }, {} });

    Command::Register({
        "ui check", "tick or untick a checkbox (or any checkable control)",
        "Set a checkable control on (default) or off — only toggling it when it differs, as a person would.",
        { Command::Pos("control", "the control's name"),
          Command::Flag("on", "on (default) or --no-on for off") },
        { "ui check 'Auto-accept peer requests'", "ui check 'Close to tray' --no-on" },
        { "QAbstractButton::setChecked" }, Command::Needs::Ui, false,
        [](Command::Context &, const json &A) {
            QWidget *W = Named(Command::Str(A, "control"));
            const bool On = Command::Bool(A, "on", true);
            if (auto *B = qobject_cast<QAbstractButton *>(W); B && B->isCheckable())
            {
                if (!B->isEnabled()) throw std::runtime_error("'" + NameOf(W) + "' is disabled");
                if (B->isChecked() != On) B->click();
                return StateOf(B, 0, false);
            }
            if (auto *G = qobject_cast<QGroupBox *>(W); G && G->isCheckable()) { G->setChecked(On); return StateOf(G, 0, false); }
            throw Command::BadArgs("'" + NameOf(W) + "' cannot be ticked");
        }, {} });

    Command::Register({
        "ui set", "type into a field, pick an item, set a value",
        "Set a control's value as a person would: the text of a field (then Enter), the item of a combo box (by its "
        "text), the value of a spin box or slider.",
        { Command::Pos("control", "the control's name"), Command::Pos("value", "the new value"),
          Command::Flag("enter", "press Enter in a text field afterwards (default on)") },
        { "ui set 'You Paste a friend' 12D3KooW…", "ui set 'Max simultaneous downloads' 4" },
        { "QLineEdit::setText", "QComboBox::setCurrentIndex", "QSpinBox::setValue" }, Command::Needs::Ui, false,
        [](Command::Context &, const json &A) {
            QWidget *W = Named(Command::Str(A, "control"));
            const QString V = QString::fromStdString(Command::Str(A, "value"));
            if (!W->isEnabled()) throw std::runtime_error("'" + NameOf(W) + "' is disabled");
            if (auto *E = qobject_cast<QLineEdit *>(W))
            {
                E->setText(V);
                emit E->textEdited(V);
                if (Command::Bool(A, "enter", true)) { emit E->returnPressed(); emit E->editingFinished(); }
            }
            else if (auto *T = qobject_cast<QPlainTextEdit *>(W)) T->setPlainText(V);
            else if (auto *Tx = qobject_cast<QTextEdit *>(W)) Tx->setPlainText(V);
            else if (auto *C = qobject_cast<QComboBox *>(W))
            {
                const int I = C->findText(V, Qt::MatchFixedString);
                if (I < 0) throw Command::BadArgs("'" + NameOf(W) + "' has no item '" + V.toStdString() + "'");
                C->setCurrentIndex(I);
                emit C->activated(I);
            }
            else if (auto *S = qobject_cast<QSpinBox *>(W)) S->setValue(V.toInt());
            else if (auto *D = qobject_cast<QDoubleSpinBox *>(W)) D->setValue(V.toDouble());
            else if (auto *Sl = qobject_cast<QSlider *>(W)) Sl->setValue(V.toInt());
            else throw Command::BadArgs("'" + NameOf(W) + "' takes no value");
            return StateOf(W, 0, false);
        }, {} });

    Command::Register({
        "ui cell", "tick a checkbox inside a table cell, or select a cell",
        "A table's cell by row and column (0-based, or a column's header text): the checkbox inside it is set on "
        "(default) or off, exactly as clicking it; a cell without one is selected (and activated with --activate).",
        { Command::Pos("table", "the table's name"), Command::Pos("row", "the row: its number, or the text of its first cell"),
          Command::Pos("column", "the column: its number, or its header text"),
          Command::Flag("on", "on (default) or --no-on"),
          Command::Flag("activate", "also activate the cell (a double click)") },
        { "ui cell Networking LGZ-MacBook Accepted", "ui cell Networking LGZ-MacBook 'Share: VidyaGod' --no-on" },
        { "QAbstractItemView::indexWidget", "QAbstractItemView::setCurrentIndex" }, Command::Needs::Ui, false,
        [](Command::Context &, const json &A) {
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
                if (B && B->isCheckable())
                {
                    if (!B->isEnabled()) throw std::runtime_error("that cell's checkbox is disabled");
                    if (B->isChecked() != Command::Bool(A, "on", true)) B->click();
                    return json{{"row", R}, {"column", C}, {"checked", B->isChecked()}};
                }
                if (B) { B->click(); return json{{"row", R}, {"column", C}, {"clicked", true}}; }
            }
            if (M->flags(I) & Qt::ItemIsUserCheckable)
            {
                M->setData(I, Command::Bool(A, "on", true) ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
                return json{{"row", R}, {"column", C}, {"checked", M->data(I, Qt::CheckStateRole).toInt() == Qt::Checked}};
            }
            V->setCurrentIndex(I);
            if (Command::Bool(A, "activate", false))
            {
                emit V->activated(I);
                emit V->doubleClicked(I);
            }
            return json{{"row", R}, {"column", C}, {"selected", true}};
        }, {} });

    Command::Register({
        "ui card", "open a game card (play, download, cancel, or its menu)",
        "Act on a card of a card grid as a person does: activate it (Library: play; Catalog: download, or cancel the "
        "download in flight) or open its context menu (--menu). The card is named by its tile UID or its title.",
        { Command::Pos("grid", "the grid's name (ui tree shows it)"), Command::Pos("card", "the card's tile UID or title"),
          Command::Flag("menu", "open its context menu instead") },
        { "ui card LibraryView 'Age of Empires II'", "ui card AvailableView 749 --json" },
        { "LibraryView::activateCardAt", "LibraryView::requestCardMenu", "LibraryView::cardIndex" }, Command::Needs::Ui, false,
        [](Command::Context &, const json &A) {
            LibraryView *V = Grid(Command::Str(A, "grid"));
            const int I = V->cardIndex(QString::fromStdString(Command::Str(A, "card")));
            if (I == -2) throw Command::BadArgs("'" + Command::Str(A, "card") + "' matches several cards — use the tile UID");
            if (I < 0) throw Command::BadArgs("no card '" + Command::Str(A, "card") + "'");
            if (Command::Bool(A, "menu", false)) V->requestCardMenu(I); else V->activateCardAt(I);
            return json{{"card", I}};
        }, {} });

    Command::Register({
        "ui dialogs", "the open dialogs and message boxes, with their text and buttons",
        "Every dialog on screen: its title, its text (a message box's message and details), its buttons — what "
        "ui press answers.", {}, { "ui dialogs" }, {}, Command::Needs::Ui, false,
        [](Command::Context &, const json &) {
            json Out = json::array();
            for (QWidget *W : QApplication::topLevelWidgets())
            {
                auto *D = qobject_cast<QDialog *>(W);
                if (!D || !D->isVisible()) continue;
                json J = {{"title", D->windowTitle().toStdString()}, {"class", D->metaObject()->className()}, {"modal", D->isModal()}};
                if (auto *M = qobject_cast<QMessageBox *>(D))
                {
                    J["text"] = M->text().toStdString();
                    if (!M->informativeText().isEmpty()) J["informative"] = M->informativeText().toStdString();
                    if (!M->detailedText().isEmpty()) J["details"] = M->detailedText().toStdString();
                }
                json Buttons = json::array();
                for (QAbstractButton *B : D->findChildren<QAbstractButton *>())
                    if (B->isVisible() && !B->text().isEmpty()) Buttons.push_back(B->text().toStdString());
                J["buttons"] = Buttons;
                J["state"] = StateOf(D, 1000, true);
                Out.push_back(std::move(J));
            }
            return Out;
        }, {} });

    Command::Register({
        "ui press", "press a button of the open dialog",
        "Answer the dialog on top (a message box, a confirmation) by its button's text — what a person clicks to "
        "close it. A dialog opened by a command keeps the app answering, so this reaches it.",
        { Command::Pos("button", "the button's text (OK, Cancel, Yes, …)"),
          Command::Opt("dialog", "the dialog's title, when several are open") },
        { "ui press OK", "ui press Yes --dialog 'Remove friend'" }, { "QAbstractButton::click" }, Command::Needs::Ui, false,
        [](Command::Context &, const json &A) {
            const QString Want = QString::fromStdString(Command::Str(A, "button")), Title = QString::fromStdString(Command::Str(A, "dialog", ""));
            for (QWidget *W : QApplication::topLevelWidgets())
            {
                auto *D = qobject_cast<QDialog *>(W);
                if (!D || !D->isVisible() || (!Title.isEmpty() && !D->windowTitle().contains(Title, Qt::CaseInsensitive))) continue;
                for (QAbstractButton *B : D->findChildren<QAbstractButton *>())
                    if (B->isVisible() && QString(B->text()).remove('&').compare(Want, Qt::CaseInsensitive) == 0)
                    {
                        // Deferred: a dialog's exec() returns into the command that opened it, not into this one.
                        QTimer::singleShot(0, B, &QAbstractButton::click);
                        return json{{"dialog", D->windowTitle().toStdString()}, {"pressed", B->text().toStdString()}};
                    }
            }
            throw Command::BadArgs("no open dialog has a '" + Want.toStdString() + "' button (ui dialogs lists them)");
        }, {} });

    Command::Register({
        "ui screenshot", "save a window (or one control) as a PNG",
        "What a control looks like, rendered by the widget itself — offscreen as well as on screen.",
        { Command::Pos("file", "the PNG to write"), Command::Opt("control", "one control (default: the main window)") },
        { "ui screenshot /tmp/vg.png", "ui screenshot net.png --control Networking" }, { "QWidget::grab" }, Command::Needs::Ui, false,
        [](Command::Context &Ctx, const json &A) {
            QWidget *W = A.contains("control") ? Named(Command::Str(A, "control")) : Ctx.Window;
            const QString F = QString::fromStdString(Command::Str(A, "file"));
            const QPixmap P = W->grab();
            if (!P.save(F, "PNG")) throw std::runtime_error("cannot write " + F.toStdString());
            return json{{"file", F.toStdString()}, {"width", P.width()}, {"height", P.height()}};
        }, {} });
}
