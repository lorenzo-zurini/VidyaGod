#pragma once
//Dear ImGui's test-engine hooks, implemented to record every item a frame lays out (its ID, label, rectangle and
//window), so a driver can find a control on the package editor's canvas by what it shows and click it with real
//input - the canvas has no widgets for accessibility to see. Off unless VG_UI_PROBE names the file each frame's items
//are written to; nothing is recorded or written otherwise.

struct ImGuiContext;

namespace ImGuiProbe
{
    bool Enabled();
    //Before ImGui::NewFrame(): forget the last frame's items and turn the hooks on.
    void BeginFrame(ImGuiContext * Ctx);
    //What a headless test finds controls by: the Nth item of the frame recorded last whose shown label (the part before "##"; an unlabeled input answers to its
    //hidden name) is Shown, in a window whose name holds Window (when given): its rectangle, in the canvas's own
    //coordinates.
    bool FindItem(const char * Shown, float & X0, float & Y0, float & X1, float & Y1, int Nth = 0, const char * Window = nullptr);
    //Names the item laid out last, for a widget ImGui reports no label for (BeginCombo): call it right after one.
    void NameLast(const char * Label);
    //After ImGui::Render(): write the frame's items, with the canvas's origin on screen (logical pixels), to the file
    //VG_UI_PROBE names (atomically: a temp file renamed over it).
    void WriteFrame(int OriginX, int OriginY, int Width, int Height, double DevicePixelRatio);
}
