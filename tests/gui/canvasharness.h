#ifndef CANVASHARNESS_H
#define CANVASHARNESS_H

// Drives the package canvas headlessly: Dear ImGui needs no GPU to lay out and process input, so every interaction of
// the editing surface — clicks, drags, wires, typing, zoom — runs under test with a synthetic mouse and keyboard.
// ImGui 1.92 hands font textures to the renderer backend to create; with none, this plays one that accepts them.

#include "pkgcanvas.h"
#include "pkgdoc.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <string>

struct CanvasHarness
{
    PkgDoc::Document Doc;
    PkgCanvas *Canvas = nullptr;
    ImVec2 Mouse{700, 500};
    ImVec2 Display{1400, 900};

    static void InitContext()
    {
        ImGui::CreateContext();
        ImGuiIO &Io = ImGui::GetIO();
        Io.IniFilename = nullptr;
        Io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        PkgCanvas::InstallFontsAndStyle();
    }
    static void DestroyContext() { ImGui::DestroyContext(); }

    void open() { Canvas = new PkgCanvas(&Doc); }
    void close() { delete Canvas; Canvas = nullptr; }

    //What a renderer backend does with the atlas each frame: create/update requests acknowledged, destroys done.
    static void AckTextures()
    {
        for (ImTextureData *T : ImGui::GetPlatformIO().Textures)
        {
            if (T->Status == ImTextureStatus_WantCreate || T->Status == ImTextureStatus_WantUpdates)
            { T->SetTexID((ImTextureID)(intptr_t)1); T->SetStatus(ImTextureStatus_OK); }
            else if (T->Status == ImTextureStatus_WantDestroy)
            { T->SetTexID(ImTextureID_Invalid); T->SetStatus(ImTextureStatus_Destroyed); }
        }
    }

    void frame(ImVec2 M) { Mouse = M; frame(); }
    void frame()
    {
        ImGuiIO &Io = ImGui::GetIO();
        Io.DisplaySize = Display;
        Io.DeltaTime = 1.0f / 60.0f;
        Io.AddMousePosEvent(Mouse.x, Mouse.y);
        ImGui::NewFrame();
        Canvas->frame();
        ImGui::Render();
        AckTextures();
    }
    void frames(int N) { for (int I = 0; I < N; ++I) frame(); }

    void press(int B = 0) { ImGui::GetIO().AddMouseButtonEvent(B, true); frame(); }
    void release(int B = 0) { ImGui::GetIO().AddMouseButtonEvent(B, false); frame(); }
    void click(ImVec2 P, int B = 0) { frame(P); press(B); release(B); frame(); }
    void doubleClick(ImVec2 P) { frame(P); press(); release(); press(); release(); frame(); }
    void drag(ImVec2 From, ImVec2 To, int Steps = 8, int B = 0)
    {
        frame(From);
        press(B);
        for (int I = 1; I <= Steps; ++I)
            frame(ImVec2(From.x + (To.x - From.x) * (float)I / (float)Steps, From.y + (To.y - From.y) * (float)I / (float)Steps));
        release(B);
        frame();
    }
    void wheel(ImVec2 P, float Dy) { frame(P); ImGui::GetIO().AddMouseWheelEvent(0, Dy); frame(); frames(30); }
    void key(ImGuiKey K, bool Ctrl = false, bool Shift = false)
    {
        ImGuiIO &Io = ImGui::GetIO();
        if (Ctrl) Io.AddKeyEvent(ImGuiMod_Ctrl, true);
        if (Shift) Io.AddKeyEvent(ImGuiMod_Shift, true);
        Io.AddKeyEvent(K, true); frame();
        Io.AddKeyEvent(K, false);
        if (Ctrl) Io.AddKeyEvent(ImGuiMod_Ctrl, false);
        if (Shift) Io.AddKeyEvent(ImGuiMod_Shift, false);
        frame();
    }
    void type(const std::string &S) { for (char C : S) { ImGui::GetIO().AddInputCharacter((unsigned)(unsigned char)C); frame(); } }

    //Screen geometry of what the canvas drew last frame.
    ImVec4 rect(const std::string &H) const
    {
        float X0 = 0, Y0 = 0, X1 = 0, Y1 = 0;
        return Canvas->nodeRect(H, X0, Y0, X1, Y1) ? ImVec4(X0, Y0, X1, Y1) : ImVec4(0, 0, 0, 0);
    }
    ImVec2 title(const std::string &H) const { const ImVec4 R = rect(H); return ImVec2((R.x + R.z) * 0.5f, R.y + 12.0f); }
    ImVec2 port(const std::string &H, bool Out) const { float X = 0, Y = 0; Canvas->portPos(H, Out, X, Y); return ImVec2(X, Y); }
    ImVec4 canvasRect() const { float X0 = 0, Y0 = 0, X1 = 0, Y1 = 0; Canvas->canvasRect(X0, Y0, X1, Y1); return ImVec4(X0, Y0, X1, Y1); }
    //An empty point of the canvas background (far from any node): the top-left corner of the canvas region.
    ImVec2 empty() const { const ImVec4 C = canvasRect(); return ImVec2(C.x + 12.0f, C.w - 12.0f); }
};

#endif // CANVASHARNESS_H
