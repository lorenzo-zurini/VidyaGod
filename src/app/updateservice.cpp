#include "updateservice.h"
#include "appmodel.h"
#include "asyncwork.h"

#include <QPointer>
#include <QStringList>

UpdateService::UpdateService(AppModel & M, QObject * Parent) : QObject(Parent), Model(M)
{
    connect(&Model, &AppModel::nodeReady, this, &UpdateService::sweep);
}

void UpdateService::sweep()
{
    if (Swept || !Model.config()) return;   // once a run: what this run stages, it cleans up itself
    Swept = true;
    auto Cfg = std::make_shared<const nlohmann::ordered_json>(*Model.config());
    auto Done = std::make_shared<std::vector<std::string>>();
    auto Errs = std::make_shared<std::vector<std::string>>();
    AsyncWork::Run(this, [Cfg, Done, Errs] { *Done = PackageCatalog::SweepUpdateStages(*Cfg, Errs.get()); },
        [this, Done, Errs] {
            QStringList D, E;
            for (const auto & S : *Done) D << QString::fromStdString(S);
            for (const auto & S : *Errs) E << QString::fromStdString(S);
            if (!Done->empty()) Model.refreshFromDisk();   // a finished switch changed node files
            emit swept(D, E);
        });
}

bool UpdateService::start(const PackageCatalog::PackageUpdate & U, Ask Confirm)
{
    if (Busy || !Model.config()) return false;
    Busy = true;
    Current = QString::fromStdString(U.Pkg);
    CancelFlag = std::make_shared<std::atomic<bool>>(false);
    auto Cfg = std::make_shared<const nlohmann::ordered_json>(*Model.config());
    auto S = std::make_shared<PackageCatalog::StagedUpdate>();
    auto Err = std::make_shared<std::string>();
    auto Ok = std::make_shared<bool>(false);
    auto Flag = CancelFlag;
    QPointer<UpdateService> Self(this);
    emit started(Current, QString::fromStdString(U.Nick));
    emit progress(0, "Getting " + QString::fromStdString(U.Nick) + "'s version of " + Current);
    AsyncWork::Run(this,
        [Cfg, U, S, Err, Ok, Flag, Self](const AsyncWork::Job & J) {
            const PackageCatalog::SwitchProgress Say = [J, Self](double F, const std::string & D) {
                J.Post([Self, F, D] { if (Self) emit Self->progress(F, QString::fromStdString(D)); });
            };
            *Ok = PackageCatalog::StageUpdate(*Cfg, U, S.get(), Err.get(), Say, Flag.get());
        },
        [this, S, Err, Ok, Flag, Confirm, Self] {
            if (!*Ok) { end(Flag->load() ? Outcome::Cancelled : Outcome::Failed, QString::fromStdString(*Err)); return; }
            auto Discard = [this, S](Outcome O, QString Why) {
                AsyncWork::Run(this, [S] { PackageCatalog::DiscardStagedUpdate(*S); }, [this, O, Why] { end(O, Why); });
            };
            if (Flag->load()) { Discard(Outcome::Cancelled, "cancelled"); return; }
            emit progress(-1, "Waiting for your answer");
            Confirm(*S, [this, S, Err, Flag, Discard, Self](bool Yes) {
                if (!Self) return;
                if (Flag->load()) { Discard(Outcome::Cancelled, "cancelled"); return; }   // cancelled while asked
                if (!Yes) { Discard(Outcome::Declined, "not switched"); return; }
                auto Done = std::make_shared<bool>(false);
                auto Now = std::make_shared<const nlohmann::ordered_json>(*Model.config());   // the library as it is now
                AsyncWork::Run(this,
                    [Now, S, Err, Done, Self](const AsyncWork::Job & J) {
                        const PackageCatalog::SwitchProgress Say = [J, Self](double F, const std::string & D) {
                            J.Post([Self, F, D] { if (Self) emit Self->progress(F, QString::fromStdString(D)); });
                        };
                        *Done = PackageCatalog::ApplyStagedUpdate(*Now, *S, Err.get(), Say);
                        if (!*Done) PackageCatalog::DiscardStagedUpdate(*S);   // refused: nothing began (a begun one keeps its journal)
                    },
                    [this, S, Err, Done] {
                        if (!*Done) { end(Outcome::Failed, QString::fromStdString(*Err)); return; }
                        //The node files changed: the index and what of it is on disk are worked out on a worker and swapped
                        //in (a rebuild here, on the GUI thread, froze a Deck for 20 s).
                        emit progress(-1, "Reading the library again");
                        const QString Result = "Switched to " + QString::fromStdString(S->U.Nick) + "'s version";
                        auto Conn = std::make_shared<QMetaObject::Connection>();
                        *Conn = connect(&Model, &AppModel::diskChecked, this, [this, Conn, Result](bool) {
                            disconnect(*Conn);
                            end(Outcome::Done, Result);
                        });
                        Model.refreshFromDisk();
                    });
            });
        });
    return true;
}

void UpdateService::cancel()
{
    if (CancelFlag) CancelFlag->store(true);
}

void UpdateService::end(Outcome O, const QString & Result)
{
    Busy = false;
    Current.clear();
    emit finished(O, Result);
}
