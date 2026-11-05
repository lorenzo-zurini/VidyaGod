#include "imguiprobe.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    struct Item
    {
        ImGuiID Id = 0;
        std::string Label, Window;
        ImRect Rect;
        ImGuiItemStatusFlags Status = 0;
    };
    std::vector<Item> Items;
    long long Frame = 0;
    bool Recording = false;

    //What an item shows: its label up to "##" - or, for an unlabeled input, its hidden name after it.
    std::string ShownLabel(const Item & I)
    {
        const size_t Hash = I.Label.find("##");
        std::string Vis = I.Label.substr(0, Hash);
        if (Vis.find_first_not_of(' ') == std::string::npos && Hash != std::string::npos && I.Label.find("###") == std::string::npos)
            Vis = I.Label.substr(I.Label.rfind("##") + 2);
        return Vis;
    }

    const char * ProbePath()
    {
        static const char * P = std::getenv("VG_UI_PROBE");
        return P && *P ? P : nullptr;
    }

    Item * Find(ImGuiID Id)
    {
        for (auto It = Items.rbegin(); It != Items.rend(); ++It)
            if (It->Id == Id) return &*It;
        return nullptr;
    }

    void Escaped(std::string & Out, const std::string & S)
    {
        Out += '"';
        for (const unsigned char C : S)
        {
            if (C == '"' || C == '\\') { Out += '\\'; Out += (char)C; }
            else if (C < 0x20) { char B[8]; std::snprintf(B, sizeof B, "\\u%04x", C); Out += B; }
            else Out += (char)C;
        }
        Out += '"';
    }
}

//---- the hooks imgui.cpp calls when IMGUI_ENABLE_TEST_ENGINE is defined -------------------------------------------

void ImGuiTestEngineHook_ItemAdd(ImGuiContext * Ctx, ImGuiID Id, const ImRect & Bb, const ImGuiLastItemData *)
{
    Item I;
    I.Id = Id;
    I.Rect = Bb;
    if (Ctx->CurrentWindow) I.Window = Ctx->CurrentWindow->Name;
    Items.push_back(std::move(I));
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext *, ImGuiID Id, const char * Label, ImGuiItemStatusFlags Flags)
{
    if (Item * I = Find(Id)) { I->Label = Label ? Label : ""; I->Status = Flags; }
}

void ImGuiTestEngineHook_Log(ImGuiContext *, const char *, ...) {}

const char * ImGuiTestEngine_FindItemDebugLabel(ImGuiContext *, ImGuiID Id)
{
    const Item * I = Find(Id);
    return I && !I->Label.empty() ? I->Label.c_str() : nullptr;
}

//---- the probe ----------------------------------------------------------------------------------------------------

bool ImGuiProbe::Enabled() { return Recording || ProbePath() != nullptr; }

void ImGuiProbe::SetRecording(bool On) { Recording = On; }

std::string ImGuiProbe::FrameJson()
{
    std::string Out = "{\"frame\":" + std::to_string(Frame) + ",\"items\":[";
    char B[256];
    bool First = true;
    for (const Item & I : Items)
    {
        if (I.Rect.GetWidth() <= 0 || I.Rect.GetHeight() <= 0) continue;
        if (!First) Out += ',';
        First = false;
        std::snprintf(B, sizeof B, "{\"id\":%u,\"rect\":[%.1f,%.1f,%.1f,%.1f],\"status\":%d,\"label\":",
                      I.Id, I.Rect.Min.x, I.Rect.Min.y, I.Rect.Max.x, I.Rect.Max.y, (int)I.Status);
        Out += B;
        Escaped(Out, ShownLabel(I));
        Out += ",\"name\":";
        Escaped(Out, I.Label);
        Out += ",\"window\":";
        Escaped(Out, I.Window);
        Out += '}';
    }
    return Out + "]}";
}

void ImGuiProbe::NameLast(const char * Label)
{
    ImGuiContext * Ctx = ImGui::GetCurrentContext();
    if (!Ctx || !Ctx->TestEngineHookItems) return;
    if (Item * I = Find(Ctx->LastItemData.ID); I && I->Label.empty()) I->Label = Label;
}

void ImGuiProbe::BeginFrame(ImGuiContext * Ctx)
{
    if (!Enabled() || !Ctx) return;
    Items.clear();
    Ctx->TestEngineHookItems = true;
    ++Frame;
}

bool ImGuiProbe::FindItem(const char * Shown, float & X0, float & Y0, float & X1, float & Y1, int Nth, const char * Window)
{
    for (const Item & I : Items)
    {
        if (ShownLabel(I) != Shown || (Window && I.Window.find(Window) == std::string::npos) || I.Rect.GetWidth() <= 0 || I.Rect.GetHeight() <= 0 || Nth-- > 0) continue;
        X0 = I.Rect.Min.x; Y0 = I.Rect.Min.y; X1 = I.Rect.Max.x; Y1 = I.Rect.Max.y;
        return true;
    }
    return false;
}

void ImGuiProbe::WriteFrame(int OriginX, int OriginY, int Width, int Height, double DevicePixelRatio)
{
    const char * Path = ProbePath();
    if (!Path) return;
    std::string Out;
    Out.reserve(Items.size() * 160 + 256);
    char B[256];
    std::snprintf(B, sizeof B, "{\"frame\":%lld,\"origin\":[%d,%d],\"size\":[%d,%d],\"dpr\":%.3f,\"items\":[",
                  Frame, OriginX, OriginY, Width, Height, DevicePixelRatio);
    Out += B;
    bool First = true;
    for (const Item & I : Items)
    {
        if (I.Rect.GetWidth() <= 0 || I.Rect.GetHeight() <= 0) continue;
        if (!First) Out += ',';
        First = false;
        std::snprintf(B, sizeof B, "{\"id\":%u,\"rect\":[%.1f,%.1f,%.1f,%.1f],\"status\":%d,\"label\":",
                      I.Id, I.Rect.Min.x, I.Rect.Min.y, I.Rect.Max.x, I.Rect.Max.y, (int)I.Status);
        Out += B;
        Escaped(Out, I.Label);
        Out += ",\"window\":";
        Escaped(Out, I.Window);
        Out += '}';
    }
    Out += "]}\n";
    const std::string Tmp = std::string(Path) + ".tmp";
    if (FILE * F = std::fopen(Tmp.c_str(), "wb"))
    {
        std::fwrite(Out.data(), 1, Out.size(), F);
        std::fclose(F);
#ifdef _WIN32
        std::remove(Path);                                //Windows' rename does not replace an existing file
#endif
        std::rename(Tmp.c_str(), Path);
    }
}
