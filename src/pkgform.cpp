#include "pkgform.h"

#include "imgui.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <functional>
#include <sstream>

using json = nlohmann::ordered_json;
using namespace PkgGraph;

namespace PkgForm
{

namespace {

//TOTAL readers. The editor loads raw JSON off disk — it is the tool you open to FIX a node the format rejects — so
//any field may be any type, and nlohmann's value() throws on a mismatch, out of a frame with ImGui scopes open.
std::string StrOf(const json &N, const char *Key)
{
    return (N.is_object() && N.contains(Key) && N[Key].is_string()) ? N[Key].get<std::string>() : std::string();
}
bool BoolOf(const json &N, const char *Key)
{
    return N.is_object() && N.contains(Key) && N[Key].is_boolean() && N[Key].get<bool>();
}

//Entries joined by newlines, no terminator. [] and [""] both render as "": a lone empty entry is preserved (only
//an edit of its own box rewrites the list) but cannot be typed from scratch — see TextToList.
std::string ListToText(const json &Arr)
{
    std::string T;
    if (Arr.is_array())
        for (const auto &E : Arr) if (E.is_string()) { T += E.get<std::string>(); T += '\n'; }
    if (!T.empty() && T.back() == '\n') T.pop_back();
    return T;
}

//One line per entry, trimmed. KeepEmpty: a blank line is an entry (a delta's BASE_TARGETS: "" is the mount root),
//including the last one; wholly empty text is no entries.
json TextToList(const std::string &T, bool KeepEmpty)
{
    json Arr = json::array();
    std::string Line;
    for (size_t I = 0; I <= T.size(); ++I)
    {
        if (I == T.size() || T[I] == '\n')
        {
            const size_t B = Line.find_first_not_of(" \t\r"), E = Line.find_last_not_of(" \t\r");
            if (B != std::string::npos)   Arr.push_back(Line.substr(B, E - B + 1));
            else if (KeepEmpty && !T.empty()) Arr.push_back(std::string());
            Line.clear();
        }
        else Line += T[I];
    }
    return Arr;
}

//A field's label, vertically centred on the widget after it. The widget starts at a column relative to the row's
//own indent (so nested rows line up among themselves and never overlap their label), further right for a long label.
void Label(const Host &H, const char *Text)
{
    const float X0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text);
    ImGui::SameLine(std::max(X0 + H.S(84.0f), X0 + ImGui::CalcTextSize(Text).x + H.S(8.0f)));
}

//The width left on this row for a widget, to the node's right padding.
float Avail(const Host &H) { return ImGui::GetContentRegionAvail().x - H.S(H.PadRight); }

//A multi-line box's height: its lines plus one, capped at six.
float BoxHeight(const std::string &T)
{
    const int Lines = (int)std::count(T.begin(), T.end(), '\n') + 1;
    return ImGui::GetTextLineHeight() * (float)std::min(Lines + 1, 6) + ImGui::GetStyle().FramePadding.y * 2.0f;
}

void Field(json &Node, const Field &F, Host &H, int Layer);

//A REG layer's hive tree ({HKLM: {...}}), edited as flat key-path rows. The rows are held steady while a field is
//live (committing each keystroke rebuilt the tree, which re-sorts rows and merges two the moment a half-typed path
//matches another — the row being edited moved from under the cursor) and committed when the field is finished.
void RegTree(json &Obj, const PkgGraph::Field &F, Host &H, int Layer)
{
    if (Obj.contains(F.Key) && !Obj[F.Key].is_null() && !Obj[F.Key].is_object())
    { ImGui::TextDisabled("(%s is not an object - fix it in the JSON view)", F.Label); return; }
    static const json EmptyObj = json::object();
    const json &Tree = Obj.contains(F.Key) ? Obj[F.Key] : EmptyObj;
    const std::string BufKey = "reg#" + std::to_string(Layer);
    auto Buf = H.RegBuf ? H.RegBuf->find(BufKey) : std::map<std::string, std::vector<RegRow>>::iterator();
    const bool Held = H.RegBuf && Buf != H.RegBuf->end();
    std::vector<RegRow> Rows = Held ? Buf->second : RegRowsOf(Tree);
    bool Commit = false, Editing = false;
    int DelRow = -1;
    const float Row = Avail(H) - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x * 3.0f;
    for (int R = 0; R < (int)Rows.size(); ++R)
    {
        ImGui::PushID(R);
        auto Cell = [&](const char *Id, const char *Hint, std::string &Text, float W) {
            ImGui::SetNextItemWidth(W);
            ImGui::InputTextWithHint(Id, Hint, &Text);
            if (ImGui::IsItemActive()) Editing = true;
            if (ImGui::IsItemDeactivatedAfterEdit()) Commit = true;
            if (ImGui::IsItemHovered() && !Text.empty()) ImGui::SetTooltip("%s", Text.c_str());
        };
        const std::string WasName = Rows[R].Name, WasValue = Rows[R].Value;
        Cell("##p", "HKLM\\Software\\...", Rows[R].Path, Row * 0.5f); ImGui::SameLine();
        Cell("##n", "value", Rows[R].Name, Row * 0.25f);            ImGui::SameLine();
        Cell("##v", "data", Rows[R].Value, Row * 0.25f);             ImGui::SameLine();
        //A "create this key" row stops being key-only the moment a name or a value is typed into it.
        if (Rows[R].KeyOnly && (Rows[R].Name != WasName || Rows[R].Value != WasValue)) Rows[R].KeyOnly = false;
        if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) DelRow = R;
        ImGui::PopID();
    }
    if (DelRow >= 0) { Rows.erase(Rows.begin() + DelRow); Commit = true; }
    //"+ value" adds a KEY row until the author names a value — an empty DEFAULT value would write `@=""` every launch.
    if (ImGui::Button("+ value"))
    { RegRow N{"HKLM\\Software\\", "", ""}; N.KeyOnly = true; Rows.push_back(std::move(N)); Commit = true; }
    if (Commit)
    {
        json T = Tree;
        RegRowsInto(T, Rows);
        Obj[F.Key] = std::move(T);
        H.Dirty();
        if (H.RegBuf) H.RegBuf->erase(BufKey);
    }
    else if (Editing && H.RegBuf) (*H.RegBuf)[BufKey] = std::move(Rows);
    else if (H.RegBuf) H.RegBuf->erase(BufKey);
}

//A variable's launcher facet (08-variables.md): its PRESENCE makes the variable user-facing. "Visible" toggles it.
void VarUI(json &Decl, Host &H)
{
    ImGui::PushID("uifacet");
    bool Visible = Decl.contains("UI") && Decl["UI"].is_object();
    if (ImGui::Checkbox("Visible in launcher", &Visible)) { SetVarVisible(Decl, Visible); H.Dirty(); }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shown as a control in the pre-launch dialog. Off = internal binding: it resolves from "
                          "DEFAULT (or another variable) and never appears.");
    if (!Visible) { ImGui::PopID(); return; }
    json &UI = Decl["UI"];
    auto TextRow = [&](const char *Key, const char *Text, const char *Hint) {
        std::string V = StrOf(UI, Key);
        Label(H, Text); ImGui::SetNextItemWidth(H.RightW());
        if (ImGui::InputTextWithHint((std::string("##") + Key).c_str(), Hint, &V))
        { if (V.empty()) UI.erase(Key); else UI[Key] = V; H.Dirty(); }
    };
    TextRow("LABEL", "Label", "shown in the dialog");
    TextRow("SECTION", "Section", "dialog tree folder - / nests");
    static const char *Controls[] = {"text", "bool", "int", "float", "enum", "secret"};
    std::string Ctl = UI.contains("CONTROL") && UI["CONTROL"].is_string() ? UI["CONTROL"].get<std::string>() : std::string("text");
    Label(H, "Control"); ImGui::SetNextItemWidth(H.RightW());
    if (ImGui::BeginCombo("##ctl", Ctl.c_str()))
    {
        for (const char *C : Controls) if (ImGui::Selectable(C, Ctl == C)) { UI["CONTROL"] = std::string(C); H.Dirty(); }
        ImGui::EndCombo();
    }
    Ctl = UI.contains("CONTROL") && UI["CONTROL"].is_string() ? UI["CONTROL"].get<std::string>() : std::string("text");
    auto IntRow = [&](const char *Key, const char *Text) {
        int V = UI.contains(Key) && UI[Key].is_number_integer() ? UI[Key].get<int>() : 0;
        Label(H, Text); ImGui::SetNextItemWidth(H.RightW());
        if (ImGui::InputInt((std::string("##") + Key).c_str(), &V)) { UI[Key] = V; H.Dirty(); }
    };
    if (Ctl == "int" || Ctl == "float") { IntRow("MIN", "Min"); IntRow("MAX", "Max"); }
    else if (Ctl == "text" || Ctl == "secret") TextRow("PATTERN", "Pattern", "regex the value must match (optional)");
    if (Ctl == "secret")
    {
        //POOL: seed values one per line (a CD-key list): the runtime draws one at first launch.
        std::string Text;
        if (UI.contains("POOL") && UI["POOL"].is_array())
            for (const auto &V : UI["POOL"]) if (V.is_string()) Text += V.get<std::string>() + "\n";
        Label(H, "Pool");
        if (ImGui::InputTextMultiline("##pool", &Text, ImVec2(H.RightW(), BoxHeight(Text))))
        {
            json Arr = json::array();
            std::stringstream SS(Text); std::string Line;
            while (std::getline(SS, Line)) { if (!Line.empty() && Line.back() == '\r') Line.pop_back(); if (!Line.empty()) Arr.push_back(Line); }
            UI["POOL"] = std::move(Arr); H.Dirty();
        }
    }
    if (Ctl == "enum")
    {
        //CHOICES: one per line, "Label = value", or a bare value (kept a bare string, as authored).
        std::string Text;
        if (UI.contains("CHOICES") && UI["CHOICES"].is_array())
            for (const auto &C : UI["CHOICES"])
            {
                if (C.is_string()) Text += C.get<std::string>() + "\n";
                else if (C.is_object())
                {
                    const std::string L = StrOf(C, "LABEL"), V = StrOf(C, "VALUE");
                    Text += (L.empty() || L == V) ? V + "\n" : L + " = " + V + "\n";
                }
            }
        if (!Text.empty()) Text.pop_back();
        Label(H, "Choices");
        if (ImGui::InputTextMultiline("##choices", &Text, ImVec2(H.RightW(), BoxHeight(Text))))
        {
            auto Trim = [](const std::string &X) {
                const size_t A = X.find_first_not_of(" \t\r"), B = X.find_last_not_of(" \t\r");
                return A == std::string::npos ? std::string() : X.substr(A, B - A + 1);
            };
            json Arr = json::array();
            std::stringstream SS(Text); std::string Line;
            while (std::getline(SS, Line))
            {
                Line = Trim(Line);
                if (Line.empty()) continue;
                const size_t Eq = Line.find('=');
                if (Eq == std::string::npos) Arr.push_back(Line);
                else Arr.push_back(json{{"LABEL", Trim(Line.substr(0, Eq))}, {"VALUE", Trim(Line.substr(Eq + 1))}});
            }
            UI["CHOICES"] = std::move(Arr); H.Dirty();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("One choice per line: Label = value (or just a value)");
    }
    ImGui::PopID();
}

void Field(json &Node, const PkgGraph::Field &F, Host &H, int Layer)
{
    ImGui::PushID(F.Key);
    switch (F.Kind)
    {
    case FieldKind::Text:
    {
        std::string V = StrOf(Node, F.Key);
        Label(H, F.Label); ImGui::SetNextItemWidth(H.RightW());
        if (ImGui::InputTextWithHint("##v", F.Hint, &V)) { Node[F.Key] = V; H.Dirty(); }   // a type key must survive being emptied
        if (ImGui::IsItemHovered() && F.Hint[0]) ImGui::SetTooltip("%s", F.Hint);
        break;
    }
    case FieldKind::Enum:
    {
        const std::string Cur = StrOf(Node, F.Key);
        const char *Shown = Cur.c_str();
        for (const auto &O : F.Options) if (Cur == O.first) Shown = O.second;
        Label(H, F.Label); ImGui::SetNextItemWidth(H.RightW());
        if (ImGui::BeginCombo("##v", Shown))
        {
            for (const auto &O : F.Options)
                if (ImGui::Selectable(O.second, Cur == O.first)) { Node[F.Key] = O.first; H.Dirty(); }
            ImGui::EndCombo();
        }
        break;
    }
    case FieldKind::Check:
    {
        bool V = BoolOf(Node, F.Key);
        if (ImGui::Checkbox(F.Label, &V)) { Node[F.Key] = V; H.Dirty(); }
        if (ImGui::IsItemHovered() && F.Hint[0]) ImGui::SetTooltip("%s", F.Hint);
        break;
    }
    case FieldKind::StringList:
    case FieldKind::StringListKeepEmpty:
    {
        const bool KeepEmpty = (F.Kind == FieldKind::StringListKeepEmpty);
        //A value of the WRONG SHAPE is shown, never edited: the list round trip drops non-string entries, so the
        //first keystroke would destroy them — in the tool opened to repair them.
        const json *Val = (Node.is_object() && Node.contains(F.Key)) ? &Node[F.Key] : nullptr;
        const int BadAt = StringListFault(Val);
        if (BadAt != kStringListOk)
        {
            const std::string What = (BadAt >= 0) ? "entry " + std::to_string(BadAt) + " is " + DescribeValue(Val->at((size_t)BadAt))
                                                  : DescribeValue(*Val);
            Label(H, F.Label);
            ImGui::TextDisabled("%s", What.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", BadAt >= 0 ? "Every entry of this list has to be a string. Fix it in the JSON view."
                                                   : "This is not a list. Fix it in the JSON view - editing here would replace it.");
            break;
        }
        std::string T = ListToText(Val ? *Val : json::array());
        auto Write = [&](const std::string &Text) {
            json A = TextToList(Text, KeepEmpty);
            if (A.empty()) Node.erase(F.Key); else Node[F.Key] = std::move(A);   // an emptied list omits the key
            H.Dirty();
        };
        Label(H, F.Label);
        //A KeepEmpty list is always a box (a blank line is data); others are one line until they hold several.
        const bool Multi = KeepEmpty || T.find('\n') != std::string::npos;
        if (!Multi) { ImGui::SetNextItemWidth(H.RightW()); if (ImGui::InputTextWithHint("##v", F.Hint, &T)) Write(T); }
        else if (ImGui::InputTextMultiline("##v", &T, ImVec2(H.RightW(), BoxHeight(T)))) Write(T);
        if (ImGui::IsItemHovered() && F.Hint[0]) ImGui::SetTooltip("%s\n(one per line)", F.Hint);
        break;
    }
    case FieldKind::KeyValue:
    {
        static const json EmptyObj = json::object();
        const bool Bad = Node.contains(F.Key) && !Node[F.Key].is_null() && !Node[F.Key].is_object();
        if (Bad) { ImGui::TextDisabled("(%s is not an object - fix it in the JSON view)", F.Label); break; }
        const json &Map = Node.contains(F.Key) && Node[F.Key].is_object() ? Node[F.Key] : EmptyObj;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(F.Label);
        std::string DelKey; std::pair<std::string, std::string> Rename;
        int Row = 0;
        const float Line = Avail(H) - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x * 2.0f;
        const float KW = Line * 0.42f, VW = Line - KW;
        for (const auto &[K, V] : Map.items())
        {
            //Identified by ROW, not by the key being typed (a changing id loses the active item every keystroke),
            //and renamed only when the field is finished (a half-typed key must not collide and erase an entry).
            ImGui::PushID(Row++);
            std::string Key = K, Val = V.is_string() ? V.get<std::string>() : V.dump();
            ImGui::SetNextItemWidth(KW);
            ImGui::InputText("##k", &Key);
            if (ImGui::IsItemDeactivatedAfterEdit() && Key != K && !Key.empty() && !Map.contains(Key)) Rename = {K, Key};
            ImGui::SameLine();
            ImGui::SetNextItemWidth(VW);
            if (!F.Options.empty())
            {
                const char *Shown = Val.c_str();
                for (const auto &O : F.Options) if (Val == O.first) Shown = O.second;
                if (ImGui::BeginCombo("##v", Shown))
                {
                    for (const auto &O : F.Options)
                        if (ImGui::Selectable(O.second, Val == O.first)) { Node[F.Key][K] = O.first; H.Dirty(); }
                    //An order the enum does not list stays editable, or picking anything discards it.
                    ImGui::Separator();
                    std::string Free = Val;
                    ImGui::SetNextItemWidth(H.S(150.0f));
                    if (ImGui::InputTextWithHint("##free", "custom", &Free) && Free != Val) { Node[F.Key][K] = Free; H.Dirty(); }
                    ImGui::EndCombo();
                }
            }
            else if (ImGui::InputText("##v", &Val)) { Node[F.Key][K] = Val; H.Dirty(); }
            ImGui::SameLine();
            if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) DelKey = K;
            ImGui::PopID();
        }
        if (!DelKey.empty()) { Node[F.Key].erase(DelKey); H.Dirty(); }   // an emptied ENV/DLL stays: it is the type key
        if (!Rename.first.empty())
        {
            json V = Node[F.Key][Rename.first];
            if (WritableObject(Node, F.Key))
            {
                Node[F.Key].erase(Rename.first);
                if (WriteSubKey(Node, F.Key, Rename.second, V)) H.Dirty();
            }
        }
        if (ImGui::Button("+ add") && WriteSubKey(Node, F.Key, "", F.Options.empty() ? "" : F.Options.front().first)) H.Dirty();
        break;
    }
    case FieldKind::ObjArray:
    {
        const bool Present = Node.contains(F.Key) && Node[F.Key].is_array();
        const bool Replaceable = !Node.contains(F.Key) || Node[F.Key].is_null() || Present;
        if (!Replaceable) { ImGui::TextDisabled("(%s is not a list - fix it in the JSON view)", F.Label); break; }
        static json EmptyArr = json::array();            // stays empty: every write goes through Node[F.Key]
        json &Arr = Present ? Node[F.Key] : EmptyArr;
        int Del = -1;
        //One folded row per entry, named by EntryTitle (LABEL, COMMENT, else mode and site): a layer of 187 byte
        //patches reads as 187 lines, every one reachable.
        for (int I = 0; I < (int)Arr.size(); ++I)
        {
            ImGui::PushID(I);
            if (!Arr[I].is_object())
            {
                //Malformed content drawn as fields would throw on the first write, out of the frame.
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("(entry %d is not an object - fix it in the JSON view)", I);
                ImGui::SameLine();
                if (ImGui::Button("remove")) Del = I;
                ImGui::PopID();
                continue;
            }
            const std::string Title = EntryTitle(Arr[I]);
            const std::string Facts = EntrySummary(Arr[I]);
            const bool Open = H.Fold("L" + std::to_string(Layer) + "/" + F.Key + "/" + std::to_string(I),
                                     FitWidth(Title.empty() ? "(new entry)" : Title, Avail(H) - ImGui::GetFrameHeight() * 1.5f));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", (Title + (Facts != Title && !Facts.empty() ? "\n" + Facts : std::string())).c_str());
            if (ImGui::BeginPopupContextItem("##entrymenu"))
            {
                if (ImGui::Selectable("Remove this entry")) Del = I;
                ImGui::EndPopup();
            }
            if (Open)
            {
                for (const PkgGraph::Field &S : F.Sub)
                {
                    //BASE_TARGETS means nothing except on a delta (NodeLower refuses it elsewhere).
                    if (S.Key == std::string("BASE_TARGETS") && StrOf(Arr[I], "FORM") != "delta") continue;
                    Field(Arr[I], S, H, Layer);
                }
                if (F.VarUI) VarUI(Arr[I], H);
                if (ImGui::Button("remove entry")) Del = I;
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (Del >= 0) { Arr.erase((size_t)Del); H.Dirty(); }   // an emptied EDIT/EXEC stays: it is the type key
        const std::string Add = std::string("+ ") + (F.Kind == FieldKind::ObjArray && std::string(F.Key) == "EXEC" ? "entry" : "op");
        if (ImGui::Button(Add.c_str()))
        {
            if (!Present) Node[F.Key] = json::array();   // materialised only when something is added
            json E = json::object();
            for (const PkgGraph::Field &S : F.Sub) if (S.Kind == FieldKind::Enum && !S.Options.empty()) E[S.Key] = S.Options.front().first;
            Node[F.Key].push_back(std::move(E));
            H.Dirty();
        }
        break;
    }
    case FieldKind::Object:
    {
        //ONE optional nested object (an entry's TILE): absent, a button adds it — drawing never materialises it.
        if (!Node.contains(F.Key) || Node[F.Key].is_null())
        {
            if (ImGui::Button((std::string("+ ") + F.Label).c_str())) { Node[F.Key] = json::object(); H.Dirty(); }
            break;
        }
        if (!Node[F.Key].is_object()) { ImGui::TextDisabled("(%s is not an object - fix it in the JSON view)", F.Label); break; }
        const bool Open = H.Fold("L" + std::to_string(Layer) + "/obj/" + F.Key, F.Label);
        bool Remove = false;
        if (ImGui::BeginPopupContextItem("##objmenu"))
        {
            if (ImGui::Selectable((std::string("Remove ") + F.Label).c_str())) Remove = true;
            ImGui::EndPopup();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("right-click: remove the %s", F.Label);
        if (Open)
        {
            json &O = Node[F.Key];
            for (const PkgGraph::Field &S : F.Sub) Field(O, S, H, Layer);
            ImGui::TreePop();
        }
        if (Remove) { Node.erase(F.Key); H.Dirty(); }
        break;
    }
    case FieldKind::TakeList:
    {
        const json *Val = (Node.is_object() && Node.contains(F.Key)) ? &Node[F.Key] : nullptr;
        Label(H, F.Label);
        if (TakeFault(Val)) { ImGui::TextDisabled("%s", DescribeValue(*Val).c_str()); break; }
        std::string T = Val ? TakeToText(*Val) : std::string();
        auto Write = [&](const std::string &Text) {
            json A = TextToTake(Text);
            if (A.empty()) Node.erase(F.Key); else Node[F.Key] = std::move(A);   // no TAKE = the whole node
            H.Dirty();
        };
        if (T.find('\n') == std::string::npos) { ImGui::SetNextItemWidth(H.RightW()); if (ImGui::InputTextWithHint("##v", F.Hint, &T)) Write(T); }
        else if (ImGui::InputTextMultiline("##v", &T, ImVec2(H.RightW(), BoxHeight(T)))) Write(T);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n(one per line)", F.Hint);
        break;
    }
    case FieldKind::Arch:
    {
        const json Arch = (Node.contains(F.Key) && Node[F.Key].is_array()) ? Node[F.Key] : json::array();
        Label(H, F.Label);
        for (const char *A : {"32", "64"})
        {
            bool On = false;
            for (const auto &X : Arch) if (X.is_string() && X.get<std::string>() == A) On = true;
            if (ImGui::Checkbox(A, &On))
            {
                json Next = json::array();
                for (const auto &X : Arch) if (!(X.is_string() && X.get<std::string>() == A)) Next.push_back(X);
                if (On) Next.push_back(A);
                if (Next.empty()) Node.erase(F.Key); else Node[F.Key] = std::move(Next);
                H.Dirty();
            }
            ImGui::SameLine();
        }
        ImGui::NewLine();
        break;
    }
    case FieldKind::RegTree:
        RegTree(Node, F, H, Layer);
        break;
    case FieldKind::VarMap:
    {
        if (Node.contains(F.Key) && !Node[F.Key].is_null() && !Node[F.Key].is_object())
        { ImGui::TextDisabled("(%s is not an object - fix it in the JSON view)", F.Label); break; }
        static json EmptyObj = json::object();           // stays empty: every write goes through Node[F.Key]
        json &Map = Node.contains(F.Key) ? Node[F.Key] : EmptyObj;
        std::string DelKey; std::pair<std::string, std::string> Rename;
        int Row = 0;
        //One folded row per variable, named by its launcher label.
        for (auto &[K, D] : Map.items())
        {
            ImGui::PushID(Row++);
            const bool Open = H.Fold("L" + std::to_string(Layer) + "/var/" + K,
                                     FitWidth(VarTitle(K, D), Avail(H) - ImGui::GetFrameHeight() * 1.5f));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%%%s%%%s", K.c_str(), D.is_object() && D.contains("DEFAULT") && D["DEFAULT"].is_string()
                                                                       ? (" = " + D["DEFAULT"].get<std::string>()).c_str() : "");
            if (ImGui::BeginPopupContextItem("##varmenu"))
            {
                if (ImGui::Selectable("Remove this variable")) DelKey = K;
                ImGui::EndPopup();
            }
            if (!Open) { ImGui::PopID(); continue; }
            std::string Key = K;
            Label(H, "Key"); ImGui::SetNextItemWidth(H.RightW());
            ImGui::InputTextWithHint("##k", "used as %KEY%", &Key);
            if (ImGui::IsItemDeactivatedAfterEdit() && Key != K && !Key.empty() && !Map.contains(Key)) Rename = {K, Key};
            if (!D.is_object()) ImGui::TextDisabled("(malformed declaration - fix it in the JSON view)");
            else
            {
                for (const PkgGraph::Field &S : F.Sub) Field(D, S, H, Layer);
                if (F.VarUI) VarUI(D, H);
            }
            ImGui::TreePop();
            ImGui::PopID();
        }
        if (!DelKey.empty()) { Node[F.Key].erase(DelKey); H.Dirty(); }
        if (!Rename.first.empty())
        {
            //Renamed IN PLACE: moving the key to the end would reorder the variables under the author's cursor.
            json Out = json::object();
            for (auto &[K, D] : Node[F.Key].items()) Out[K == Rename.first ? Rename.second : K] = D;
            Node[F.Key] = std::move(Out);
            H.Dirty();
        }
        if (ImGui::Button("+ variable"))
        {
            std::string K = "var";
            for (int N = 2; Map.contains(K); ++N) K = "var" + std::to_string(N);
            if (!Node.contains(F.Key)) Node[F.Key] = json::object();
            Node[F.Key][K] = json::object({{"DEFAULT", ""}});
            H.Dirty();
        }
        break;
    }
    case FieldKind::KeepMap:
    {
        if (Node.contains(F.Key) && !Node[F.Key].is_null() && !Node[F.Key].is_object())
        { ImGui::TextDisabled("(%s is not an object - fix it in the JSON view)", F.Label); break; }
        static json EmptyObj = json::object();
        json &Map = Node.contains(F.Key) ? Node[F.Key] : EmptyObj;
        std::string DelKey; std::pair<std::string, std::string> Rename;
        int Row = 0;
        const float Line = Avail(H) - ImGui::GetStyle().ItemSpacing.x * 3.0f - ImGui::GetFrameHeight();
        for (auto &[K, E] : Map.items())
        {
            ImGui::PushID(Row++);
            //Three shapes, one meaning each: true (the user owns it), {NAME, CLOUD} (owns it, stored under NAME),
            //false (a more specific address the package takes back).
            const bool Back = E.is_boolean() && !E.get<bool>();
            std::string Addr = K;
            ImGui::SetNextItemWidth(Line * 0.68f);
            ImGui::InputTextWithHint("##a", F.Hint, &Addr);
            if (ImGui::IsItemDeactivatedAfterEdit() && Addr != K && !Addr.empty() && !Map.contains(Addr)) Rename = {K, Addr};
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", K.c_str());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Line * 0.32f);
            if (ImGui::BeginCombo("##m", Back ? "take back" : "keep"))
            {
                if (ImGui::Selectable("keep", !Back) && Back) { Node[F.Key][K] = true; H.Dirty(); }
                if (ImGui::Selectable("take back", Back) && !Back) { Node[F.Key][K] = false; H.Dirty(); }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) DelKey = K;
            if (!Back && E.is_object())
            {
                std::string Name = StrOf(E, "NAME");
                bool Cloud = !(E.contains("CLOUD") && E["CLOUD"].is_boolean() && !E["CLOUD"].get<bool>());
                ImGui::SetNextItemWidth(Line * 0.68f);
                const bool NameEdit = ImGui::InputTextWithHint("##n", "stored as (optional)", &Name);
                ImGui::SameLine();
                const bool CloudEdit = ImGui::Checkbox("cloud", &Cloud);
                if (NameEdit || CloudEdit)
                {
                    json O = E;
                    if (Name.empty()) O.erase("NAME"); else O["NAME"] = Name;
                    if (Cloud) O.erase("CLOUD"); else O["CLOUD"] = false;
                    Node[F.Key][K] = O.empty() ? json(true) : O;
                    H.Dirty();
                }
            }
            ImGui::PopID();
        }
        if (!DelKey.empty()) { Node[F.Key].erase(DelKey); H.Dirty(); }
        if (!Rename.first.empty())
        {
            json Out = json::object();
            for (auto &[K, V] : Node[F.Key].items()) Out[K == Rename.first ? Rename.second : K] = V;
            Node[F.Key] = std::move(Out);
            H.Dirty();
        }
        if (ImGui::Button("+ keep"))
        {
            if (!Node.contains(F.Key)) Node[F.Key] = json::object();
            std::string K = "FILES/";
            for (int N = 2; Node[F.Key].contains(K); ++N) K = "FILES/new" + std::to_string(N) + "/";
            Node[F.Key][K] = true;
            H.Dirty();
        }
        break;
    }
    case FieldKind::Cover:
    {
        //COVER is {FILE, SOURCE, SIZE}; a bare string is shown and edited as a string (never promoted: that would
        //change the package's bytes for a cosmetic edit); anything else is shown, never written through.
        if (Node.contains(F.Key) && !Node[F.Key].is_null() && !Node[F.Key].is_object() && !Node[F.Key].is_string())
        { ImGui::TextDisabled("(%s is not an object - fix it in the JSON view)", F.Label); break; }
        std::string P;
        if (Node.contains(F.Key)) P = Node[F.Key].is_object() ? StrOf(Node[F.Key], "FILE") : Node[F.Key].is_string() ? Node[F.Key].get<std::string>() : "";
        Label(H, F.Label);
        const float Btn = ImGui::CalcTextSize("browse").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetNextItemWidth(Avail(H) - Btn - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::InputTextWithHint("##v", "cover image in this bundle", &P))
        {
            if (Node.contains(F.Key) && Node[F.Key].is_string()) { Node[F.Key] = P; H.Dirty(); }
            else if (WriteSubKey(Node, F.Key, "FILE", P)) H.Dirty();
        }
        ImGui::SameLine();
        if (ImGui::Button("browse")) H.Request("browse_cover");
        break;
    }
    }
    ImGui::PopID();
}

//VARIANT, RECOMMENDED, SECTION describe the node itself (they never fold) — one folded row above its layers, whose
//title says what is set.
void Properties(json &Node, Host &H)
{
    const std::string Variant = StrOf(Node, "VARIANT"), Section = StrOf(Node, "SECTION");
    json Rec = (Node.contains("RECOMMENDED") && Node["RECOMMENDED"].is_array()) ? Node["RECOMMENDED"] : json::array();
    std::string Summary = "Properties";
    if (!Variant.empty()) Summary += "  -  variant \"" + Variant + "\"";
    else if (!Section.empty()) Summary += "  -  in " + Section;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const bool Open = H.Fold("props", FitWidth(Summary, Avail(H) - ImGui::GetFrameHeight()));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("What this node is, rather than what it holds:\nvariant - its name on the shelf (empty = not a row)"
                          "\nsection - its folder in the pre-launch graft tree\nrecommended - tiles where it comes first / is pre-ticked");
    if (!Open) return;
    std::string V = Variant;
    Label(H, "Variant"); ImGui::SetNextItemWidth(H.RightW());
    if (ImGui::InputTextWithHint("##variant", "shelf name - empty = not on the shelf", &V))
    { if (V.empty()) Node.erase("VARIANT"); else Node["VARIANT"] = V; H.Dirty(); }
    std::string Sec = Section;
    Label(H, "Section"); ImGui::SetNextItemWidth(H.RightW());
    if (ImGui::InputTextWithHint("##section", "graft tree folder - / nests", &Sec))
    { if (Sec.empty()) Node.erase("SECTION"); else Node["SECTION"] = Sec; H.Dirty(); }
    std::string R;
    for (const auto &U : Rec) if (U.is_string()) R += (R.empty() ? "" : " ") + U.get<std::string>();
    Label(H, "Recommended"); ImGui::SetNextItemWidth(H.RightW());
    if (ImGui::InputTextWithHint("##recommended", "tile UIDs, space separated", &R))
    {
        json Out = json::array();
        std::stringstream SS(R); std::string U;
        while (SS >> U) Out.push_back(U);
        if (Out.empty()) Node.erase("RECOMMENDED"); else Node["RECOMMENDED"] = std::move(Out);
        H.Dirty();
    }
    ImGui::TreePop();
}

} // namespace

std::string FitWidth(const std::string &Text, float Width)
{
    if (ImGui::CalcTextSize(Text.c_str()).x <= Width) return Text;
    static const std::string Ell = "\xE2\x80\xA6";       // U+2026: the editor's font has it
    size_t Lo = 0, Hi = Text.size();
    while (Lo < Hi)                                       // the longest prefix that fits with the ellipsis
    {
        const size_t Mid = (Lo + Hi + 1) / 2;
        if (ImGui::CalcTextSize((Text.substr(0, Mid) + Ell).c_str()).x <= Width) Lo = Mid; else Hi = Mid - 1;
    }
    while (Lo > 0 && ((unsigned char)Text[Lo] & 0xC0) == 0x80) --Lo;   // never split a UTF-8 sequence
    return Text.substr(0, Lo) + Ell;
}

void Body(json &Node, Host &H)
{
    Properties(Node, H);
    if (Node.contains("LAYERS") && !Node["LAYERS"].is_array())
    { ImGui::TextDisabled("(LAYERS is not a list - fix it in the JSON view)"); return; }
    static json EmptyArr = json::array();
    json &Ls = Node.contains("LAYERS") ? Node["LAYERS"] : EmptyArr;
    const std::vector<LayerItem> Items = LayerItems(Node, H.Facets ? *H.Facets : VarFacets{}, H.RefLabel);

    //The section tree: folders in first-appearance order, layers in fold order within each.
    struct Group { std::string Name, Path; std::vector<std::pair<bool, int>> Kids; };
    std::vector<Group> Gs(1);
    for (int I = 0; I < (int)Items.size(); ++I)
    {
        int G = 0;
        std::string Path, Seg;
        std::stringstream SS(Items[(size_t)I].Section);
        while (std::getline(SS, Seg, '/'))
        {
            Path += (Path.empty() ? "" : "/") + Seg;
            int Found = -1;
            for (const auto &[IsG, K] : Gs[(size_t)G].Kids) if (IsG && Gs[(size_t)K].Name == Seg) { Found = K; break; }
            if (Found < 0) { Gs.push_back({Seg, Path, {}}); Found = (int)Gs.size() - 1; Gs[(size_t)G].Kids.push_back({true, Found}); }
            G = Found;
        }
        Gs[(size_t)G].Kids.push_back({false, I});
    }

    int Del = -1, MoveFrom = -1, MoveBy = 0;
    auto DrawLayer = [&](const LayerItem &It) {
        const int I = It.Layer;
        ImGui::PushID(I);
        int R, G, B;
        TypeColour(It.Type, R, G, B);
        const std::string Type = It.Type.empty() ? std::string("?") : It.Type;
        const float Buttons = ImGui::GetFrameHeight() * 2.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;
        const float Room = Avail(H) - Buttons - ImGui::GetTreeNodeToLabelSpacing();
        //The type tag in its colour, then the name: one label so the whole row is the fold's hit target.
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4((float)std::min(255, R + 90) / 255.0f, (float)std::min(255, G + 90) / 255.0f,
                                                    (float)std::min(255, B + 90) / 255.0f, 1.0f));
        const std::string Tag = Type + "  ";
        const bool Open = H.Fold("L" + std::to_string(I), Tag + FitWidth(It.Title, Room - ImGui::CalcTextSize(Tag.c_str()).x));
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
        {
            std::string Tip = It.Title + "\n" + Type + " layer #" + std::to_string(I + 1) + " of " + std::to_string(Ls.size());
            if (It.Summary != It.Title && !It.Summary.empty()) Tip += "\n" + It.Summary;
            if (const std::string W = StrOf(Ls[(size_t)I], "WHEN"); !W.empty()) Tip += "\nwhen " + W;
            if (const std::string C = StrOf(Ls[(size_t)I], "COMMENT"); !C.empty()) Tip += "\n\n" + C;
            Tip += std::string("\n\n") + TypeHelp(It.Type) + "\n(right-click: move / remove)";
            ImGui::SetTooltip("%s", Tip.c_str());
        }
        if (ImGui::BeginPopupContextItem("##layermenu"))
        {
            if (ImGui::Selectable("Move up", false, I > 0 ? 0 : ImGuiSelectableFlags_Disabled)) { MoveFrom = I; MoveBy = -1; }
            if (ImGui::Selectable("Move down", false, I + 1 < (int)Ls.size() ? 0 : ImGuiSelectableFlags_Disabled)) { MoveFrom = I; MoveBy = +1; }
            ImGui::Separator();
            if (ImGui::Selectable("Remove layer")) Del = I;
            ImGui::EndPopup();
        }
        //Reordering is the fold order, so it is one click away — on the row under the cursor only (a column of arrows
        //on every row is noise), right-aligned so they line up. The space is always kept: rows never reflow on hover.
        const ImVec2 RowMin = ImGui::GetItemRectMin(), RowMax(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x, ImGui::GetItemRectMax().y);
        const ImVec2 Ms = ImGui::GetIO().MousePos;
        const bool RowHot = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && Ms.x >= RowMin.x && Ms.x <= RowMax.x && Ms.y >= RowMin.y && Ms.y < RowMax.y;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - H.S(H.PadRight) - Buttons + ImGui::GetStyle().ItemSpacing.x);
        if (RowHot)
        {
            if (ImGui::ArrowButton("##up", ImGuiDir_Up)) { MoveFrom = I; MoveBy = -1; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("move up (earlier in the fold)");
            ImGui::SameLine();
            if (ImGui::ArrowButton("##dn", ImGuiDir_Down)) { MoveFrom = I; MoveBy = +1; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("move down (later in the fold - later wins)");
        }
        else ImGui::Dummy(ImVec2(Buttons - ImGui::GetStyle().ItemSpacing.x, ImGui::GetFrameHeight()));
        if (Open)
        {
            if (It.Type.empty()) ImGui::TextDisabled("(needs exactly one type key - fix it in the JSON view)");
            else for (const PkgGraph::Field &F : FieldsFor(It.Type)) Field(Ls[(size_t)I], F, H, I);
            ImGui::TreePop();
        }
        ImGui::PopID();
    };
    std::function<void(int)> DrawGroup = [&](int G) {
        for (const auto &[IsG, K] : Gs[(size_t)G].Kids)
        {
            if (!IsG) { DrawLayer(Items[(size_t)K]); continue; }
            if (H.Fold("S:" + Gs[(size_t)K].Path, FitWidth(Gs[(size_t)K].Name, Avail(H) - ImGui::GetFrameHeight())))
            { DrawGroup(K); ImGui::TreePop(); }
        }
    };
    if (Ls.empty()) { ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("no layers yet"); }
    DrawGroup(0);
    if (Del >= 0) { Ls.erase((size_t)Del); if (H.RegBuf) H.RegBuf->clear(); H.Dirty(); }
    else if (MoveFrom >= 0 && MoveLayer(Node, MoveFrom, MoveBy)) { if (H.RegBuf) H.RegBuf->clear(); H.Dirty(); }
}

}
