#include "activitylog.h"

#include <algorithm>

ActivityLog & ActivityLog::Get()
{
    static ActivityLog * L = new ActivityLog();   // lives as long as the process: jobs report until the very end
    return *L;
}

ActivityLog::Entry * ActivityLog::at(int Id)
{
    for (Entry & E : Items) if (E.Id == Id) return &E;
    return nullptr;
}

const ActivityLog::Entry * ActivityLog::find(int Id) const
{
    for (const Entry & E : Items) if (E.Id == Id) return &E;
    return nullptr;
}

int ActivityLog::start(const QString & Kind, const QString & Title, std::function<void()> Cancel)
{
    Entry E;
    E.Id = NextId++;
    E.Kind = Kind;
    E.Title = Title;
    E.CanCancel = static_cast<bool>(Cancel);
    E.Started = QDateTime::currentDateTime();
    Items.push_back(E);
    if (Cancel) Cancels.push_back({ E.Id, std::move(Cancel) });
    emit changed();
    return E.Id;
}

void ActivityLog::progress(int Id, double Fraction, const QString & Detail)
{
    Entry * E = at(Id);
    if (!E || !E->Running) return;
    E->Fraction = Fraction < 0 ? -1 : std::min(1.0, Fraction);
    if (!Detail.isNull()) E->Detail = Detail;
    emit changed();
}

void ActivityLog::finish(int Id, bool Ok, const QString & Result)
{
    Entry * E = at(Id);
    if (!E || !E->Running) return;
    E->Running = false;
    E->Failed = !Ok;
    E->CanCancel = false;
    E->Detail = Result;
    E->Fraction = Ok ? 1.0 : E->Fraction;
    E->Ended = QDateTime::currentDateTime();
    if (!Ok) ++Unseen;
    Cancels.erase(std::remove_if(Cancels.begin(), Cancels.end(), [Id](const auto & C) { return C.first == Id; }), Cancels.end());
    emit changed();
}

int ActivityLog::note(const QString & Kind, const QString & Title, bool Ok, const QString & Result)
{
    const int Id = start(Kind, Title);
    finish(Id, Ok, Result);
    return Id;
}

int ActivityLog::runningOf(const QString & Kind, const QString & Title) const
{
    for (const Entry & E : Items)
        if (E.Running && E.Kind == Kind && (Title.isEmpty() || E.Title == Title)) return E.Id;
    return 0;
}

void ActivityLog::cancel(int Id)
{
    for (const auto & [C, Fn] : Cancels)
        if (C == Id) { const auto F = Fn; F(); return; }   // a copy: the job may finish (and erase it) from inside
}

void ActivityLog::dismiss(int Id)
{
    const auto It = std::find_if(Items.begin(), Items.end(), [Id](const Entry & E) { return E.Id == Id; });
    if (It == Items.end() || It->Running) return;
    Items.erase(It);
    emit changed();
}

void ActivityLog::dismissFinished()
{
    const auto Before = Items.size();
    Items.erase(std::remove_if(Items.begin(), Items.end(), [](const Entry & E) { return !E.Running; }), Items.end());
    Unseen = 0;
    if (Items.size() != Before) emit changed();
}

int ActivityLog::running() const
{
    return (int)std::count_if(Items.begin(), Items.end(), [](const Entry & E) { return E.Running; });
}

void ActivityLog::markSeen()
{
    if (Unseen == 0) return;
    Unseen = 0;
    emit changed();
}
