#include "ipfsmodel.h"
#include "appmodel.h"
#include "packagecatalog.h"
#include "manifestmodel.h"
#include "downloadqueue.h"   // IpfsWrapper::CancelDownload / PrioritizeDownload
#include "asyncwork.h"       // guarded off-thread publish

#include <QTimer>
#include <QDateTime>
#include <QStringList>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

// Top-level tree category labels (three siblings): game content files, cover/asset images, and JSON-only node
// collections. A CID's category drives which top-level row the IPFS tab shows it under.
static const QString CatContent = QStringLiteral("Content");
static const QString CatAssets  = QStringLiteral("Assets");
static const QString CatMeta    = QStringLiteral("Meta");

// Map every ipfs SOURCE CID in the catalog to a human label ("<package> — <component>"), its owning package name, and
// its top-level category. Reads the ALREADY-BUILT in-memory catalog index (no disk rescan — this runs on the GUI
// thread during refreshes).
// The LIBRARY collection a bundle belongs to (the path component right under ".../LIBRARY/") — VidyaGod (games),
// VidyaGodRunners, VidyaGodLibraries. "" when the path has no LIBRARY component. Used to split Content by source.
static QString SourceOfBundle(const std::filesystem::path & BundleDir)
{
    bool afterLib = false;
    for (const auto & Part : BundleDir)
    {
        if (afterLib) return QString::fromStdString(Part.string());
        if (Part.string() == "LIBRARY") afterLib = true;
    }
    return {};
}

static QHash<QString, QString> BuildCidLabels(const NodeIndex & Idx, const nlohmann::ordered_json & Config,
                                              QHash<QString, QString> * OutPackages,
                                              QHash<QString, QString> * OutCategory,
                                              QHash<QString, QString> * OutPkgDirs = nullptr,
                                              QHash<QString, QString> * OutSource = nullptr)
{
    QHash<QString, QString> Labels;
    for (const auto & [NodeId, N] : Idx.Nodes)
    {
        const QString Src = SourceOfBundle(N.BundleDir);
        std::string PkgName;
        {
            std::string F = N.BundleDir.filename().string();
            size_t i = 0;
            while (i < F.size()) { if (F[i] == '[') { size_t c = F.find(']', i); if (c == std::string::npos) break; i = c + 1; }
                                   else if (F[i] == ' ') ++i; else break; }
            PkgName = (i < F.size()) ? F.substr(i) : F;
        }
        if (PkgName.empty()) PkgName = N.Meta.is_object() ? N.Meta.value("TITLE", N.GameKey()) : N.GameKey();
        if (OutPkgDirs && !PkgName.empty() && !N.BundleDir.empty())
            OutPkgDirs->insert(QString::fromStdString(PkgName), QString::fromStdString(N.BundleDir.string()));

        if (N.Layers.is_array())
            for (const auto & L : N.Layers)
            {
                if (!ManifestModel::IsVfsLayer(L.value("TYPE", std::string()))) continue;
                if (!L.contains("SOURCE") || !L["SOURCE"].is_object() || L["SOURCE"].value("TYPE", std::string()) != "ipfs") continue;
                const std::string Cid = L["SOURCE"].value("CID", std::string());
                if (Cid.empty()) continue;
                Labels.insert(QString::fromStdString(Cid), QString::fromStdString(NodeId));
                if (OutPackages) OutPackages->insert(QString::fromStdString(Cid),
                    QString::fromStdString(PkgName.empty() ? std::string("(unnamed)") : PkgName));
                if (OutCategory) OutCategory->insert(QString::fromStdString(Cid), CatContent);
                if (OutSource)   OutSource->insert(QString::fromStdString(Cid), Src.isEmpty() ? QStringLiteral("Other") : Src);
            }

        if (N.Meta.is_object() && N.Meta.contains("COVER") && N.Meta["COVER"].is_object())
        {
            const auto & Cv = N.Meta["COVER"];
            if (Cv.contains("SOURCE") && Cv["SOURCE"].is_object() && Cv["SOURCE"].value("TYPE", std::string()) == "ipfs")
            {
                const std::string Cid = Cv["SOURCE"].value("CID", std::string());
                if (!Cid.empty())
                {
                    Labels.insert(QString::fromStdString(Cid), QString::fromStdString(PkgName + " — cover"));
                    if (OutPackages) OutPackages->insert(QString::fromStdString(Cid), QString::fromStdString(PkgName));
                    if (OutCategory) OutCategory->insert(QString::fromStdString(Cid), CatAssets);
                    if (OutSource)   OutSource->insert(QString::fromStdString(Cid), Src.isEmpty() ? QStringLiteral("Other") : Src);
                }
            }
        }
    }

    if (Config.contains("Settings") && Config["Settings"].is_object()
        && Config["Settings"].contains("PackageSources") && Config["Settings"]["PackageSources"].is_array())
        for (const auto & S : Config["Settings"]["PackageSources"])
        {
            const std::string Cid = S.is_object() ? S.value("CID", std::string())
                                  : (S.is_string() ? std::string(S) : std::string());
            if (Cid.empty()) continue;
            std::string Name = S.is_object() ? S.value("NAME", std::string()) : std::string();
            if (Name.empty()) Name = Cid.size() > 12 ? Cid.substr(0, 12) : Cid;
            const QString QCid = QString::fromStdString(Cid);
            Labels.insert(QCid, QString::fromStdString(Name + " (source)"));
            if (OutPackages) OutPackages->insert(QCid, QString::fromStdString(Name));
            if (OutCategory) OutCategory->insert(QCid, CatMeta);
            if (OutSource)   OutSource->insert(QCid, QString::fromStdString(Name));
        }
    return Labels;
}

IpfsModel::IpfsModel(AppModel & model, QObject * parent)
    : QObject(parent), Model(model)
{
    // Single consumer of the node's transfer events (relayed onto the GUI thread by IpfsManager).
    IpfsManager * mgr = IpfsManager::instance();
    connect(mgr, &IpfsManager::transferStarted,    this, [this](const QString & cid){
        ensureLabels(cid);
        CidState & s = Cids[cid];
        s.phase = CidState::Downloading; s.pct = -1.0; s.speedBps = -1.0; s.error.clear();
        const qlonglong Now = QDateTime::currentMSecsSinceEpoch();
        Speed.insert(cid, qMakePair((qlonglong)0, Now));
        LastProgress.insert(cid, Now);
        ensureSize(cid);   // so speed + package-progress weighting have the byte size
        emit cidChanged(cid);
    });
    // Queued state flows from the download QUEUE (relayed by IpfsManager) — single source of truth.
    connect(mgr, &IpfsManager::queueEnqueued, this, &IpfsModel::markQueued);
    connect(mgr, &IpfsManager::queueRemoved,  this, &IpfsModel::clearQueued);
    connect(mgr, &IpfsManager::transferProgress,   this, [this](const QString & cid, double pct){
        if (!Cids.contains(cid)) return;
        CidState & s = Cids[cid];
        s.pct = pct;
        const qlonglong Now = QDateTime::currentMSecsSinceEpoch();
        LastProgress.insert(cid, Now);
        if (s.phase == CidState::Stalled) s.phase = CidState::Downloading;   // peers came back
        // Speed from bytes-since-last-sample (needs the size).
        if (s.size > 0) {
            const qlonglong Bytes = (qlonglong)(pct / 100.0 * double(s.size));
            const QPair<qlonglong,qlonglong> Sample = Speed.value(cid, qMakePair((qlonglong)0, Now));
            const qlonglong Dt = Now - Sample.second;
            if (Dt >= 500) {
                const double Inst = Dt > 0 ? double(Bytes - Sample.first) * 1000.0 / double(Dt) : 0.0;
                // EMA over the raw ~500ms samples (~2-3s time constant): wifi airtime jitter makes the instantaneous
                // rate swing ±50% second-to-second even on a healthy transfer, and a raw readout reads as "jumping".
                // Smooth the DISPLAYED speed only — progress/stall logic still runs on raw samples.
                s.speedBps = s.speedBps < 0 ? Inst : 0.7 * s.speedBps + 0.3 * Inst;
                Speed.insert(cid, qMakePair(Bytes, Now));
            }
        }
        emit cidChanged(cid);
    });
    connect(mgr, &IpfsManager::transferFinalizing, this, [this](const QString & cid, double pct){
        Speed.remove(cid); LastProgress.remove(cid);
        if (!Cids.contains(cid)) return;
        CidState & s = Cids[cid];
        s.phase = CidState::Pinning; s.pct = pct; s.speedBps = -1.0;
        emit cidChanged(cid);
    });
    connect(mgr, &IpfsManager::transferFinished,   this, [this](const QString & cid, bool ok, const QString & error){
        Speed.remove(cid); LastProgress.remove(cid);
        if (Cids.contains(cid)) {
            CidState & s = Cids[cid];
            s.speedBps = -1.0;
            if (ok) { s.phase = CidState::Seeded; s.pct = 100.0; s.error.clear(); emit cidChanged(cid); }
            else {
                const QString Tail = error.section('\n', -1).trimmed();
                if (Tail == "cancelled") {
                    if (!IpfsWrapper::HasLocal(cid.toStdString())) { Cids.remove(cid); emit cidRemoved(cid); }
                    else { s.phase = CidState::Seeded; s.pct = 100.0; emit cidChanged(cid); }
                } else {
                    s.phase = CidState::Errored; s.error = error; emit cidChanged(cid);
                }
            }
        }
        refresh();   // a finished fetch likely added a pin
    });

    // Unservable content found by the seed-time self-check: show it as an ERROR row. Before this a stale
    // reference was invisible here — the tab showed it as happily Seeded while every peer request for it hung.
    connect(&model, &AppModel::contentUnservable, this, [this](const QString & cid, const QString & reason){
        if (!Cids.contains(cid)) return;
        CidState & s = Cids[cid];
        s.phase = CidState::Errored;
        s.error = reason;
        emit cidChanged(cid);
    });

    RefreshTimer = new QTimer(this);
    connect(RefreshTimer, &QTimer::timeout, this, [this]{ refresh(); });

    StallTimer = new QTimer(this);
    StallTimer->setInterval(2000);
    connect(StallTimer, &QTimer::timeout, this, [this]{ tick(); });
    StallTimer->start();
}

IpfsModel::~IpfsModel()
{
    // Detached refresh/health/size/seed workers may still be blocked in a network call; flip the guard so they skip
    // their invokeMethod-back instead of dereferencing this destroyed object.
    Alive->store(false);
}

void IpfsModel::setActive(bool on)
{
    Active = on;
    if (!RefreshTimer) return;
    if (on) { if (!RefreshTimer->isActive()) RefreshTimer->start(5000); refresh(); }
    else    RefreshTimer->stop();
}

void IpfsModel::refreshNow()
{
    for (auto it = Cids.begin(); it != Cids.end(); ++it) { it->providers = -2; it->size = -1; }
    SeedVerdict.clear();   // explicit user refresh = re-verify deliverability from scratch (off-thread)
    refresh();
}

void IpfsModel::ensureLabels(const QString & cid)
{
    if (Cids.contains(cid) && !Cids[cid].label.isEmpty()) return;
    QHash<QString, QString> Pkgs, Cats, Srcs;
    const QHash<QString, QString> Labels = BuildCidLabels(Model.catalogIndex(), *Model.config(), &Pkgs, &Cats, &PkgDirs, &Srcs);
    CidState & s = Cids[cid];
    s.label    = Labels.value(cid, QStringLiteral("(unknown)"));
    s.package  = Pkgs.value(cid, QStringLiteral("Unknown / not in your library"));
    s.category = Cats.value(cid, CatContent);
    s.source   = Srcs.value(cid);
}

void IpfsModel::rebuildLabels()
{
    QHash<QString, QString> Pkgs, Cats, Srcs;
    const QHash<QString, QString> Labels = BuildCidLabels(Model.catalogIndex(), *Model.config(), &Pkgs, &Cats, &PkgDirs, &Srcs);
    for (auto it = Cids.begin(); it != Cids.end(); ++it) {
        const QString & cid = it.key();
        it->label    = Labels.value(cid, it->label.isEmpty() ? QStringLiteral("(unknown)") : it->label);
        it->package  = Pkgs.value(cid, it->package.isEmpty() ? QStringLiteral("Unknown / not in your library") : it->package);
        it->category = Cats.value(cid, it->category.isEmpty() ? CatContent : it->category);
        it->source   = Srcs.value(cid, it->source);
    }
}

void IpfsModel::cancel(const QString & cid)      { IpfsWrapper::CancelDownload(cid.toStdString()); }
void IpfsModel::prioritize(const QString & cid)  { IpfsWrapper::PrioritizeDownload(cid.toStdString()); }

QString IpfsModel::packageCid(const QString & pkg) const
{
    const QString Dir = PkgDirs.value(pkg);
    if (Dir.isEmpty()) return {};
    const std::string Key = std::filesystem::path(Dir.toStdString()).filename().string();
    const auto & Cfg = *Model.config();
    if (Cfg.contains("Settings") && Cfg["Settings"].is_object()
        && Cfg["Settings"].contains("PackageCids") && Cfg["Settings"]["PackageCids"].is_object())
        return QString::fromStdString(Cfg["Settings"]["PackageCids"].value(Key, std::string()));
    return {};
}

// Mint (or re-mint after edits) the package-level meta-CID: the text-only folder CID of ONE package's bundle dir —
// the shareable unit between a single file's content CID and a whole source's library CID. Persisted in
// Settings.PackageCids so the tree can keep showing it; anyone can add it as a package source to receive the package.
void IpfsModel::publishPackage(const QString & pkg)
{
    const QString Dir = PkgDirs.value(pkg);
    if (Dir.isEmpty()) { emit packagePublished(pkg, {}, QStringLiteral("package folder unknown")); return; }
    auto Cid = std::make_shared<std::string>();
    auto Err = std::make_shared<std::string>();
    AsyncWork::Run(this,
        [Dir, Cid, Err]{ *Cid = PackageCatalog::PublishMetaCid(Dir.toStdString(), Err.get()); },
        [this, pkg, Dir, Cid, Err]{
            if (Cid->empty()) { emit packagePublished(pkg, {}, QString::fromStdString(*Err)); return; }
            auto & Cfg = *Model.config();
            Cfg["Settings"]["PackageCids"][std::filesystem::path(Dir.toStdString()).filename().string()] = *Cid;
            Model.save();
            emit packagePublished(pkg, QString::fromStdString(*Cid), {});
            refresh();   // the publish pinned the package's fragment CIDs — reflect them
        });
}

void IpfsModel::forceRecheck(const QStringList & cids)
{
    if (RecheckInFlight || cids.isEmpty()) return;
    RecheckInFlight = true;
    auto Todo = std::make_shared<QStringList>(cids);
    std::thread([this, Todo, A = Alive]{
        int Failed = 0, Done = 0;
        for (const QString & cid : *Todo)
        {
            if (!A->load()) return;                       // model destroyed mid-read (these are long) → stop
            const std::string Why = IpfsWrapper::VerifyCid(cid.toStdString());
            ++Done;
            const int D = Done, T = (int)Todo->size();
            if (!Why.empty()) ++Failed;
            QMetaObject::invokeMethod(this, [this, cid, Why, D, T]{
                if (Cids.contains(cid))
                {
                    CidState & s = Cids[cid];
                    // Trust the read over any cached optimism: this is the strongest evidence we have.
                    if (Why.empty()) { s.phase = CidState::Seeded; s.error.clear(); }
                    else             { s.phase = CidState::Errored; s.error = QString::fromStdString(Why); }
                    emit cidChanged(cid);
                }
                emit recheckProgress(D, T);
            }, Qt::QueuedConnection);
        }
        const int F = Failed, C = (int)Todo->size();
        if (!A->load()) return;
        QMetaObject::invokeMethod(this, [this, F, C]{
            RecheckInFlight = false;
            emit recheckFinished(C, F);
        }, Qt::QueuedConnection);
    }).detach();
}

void IpfsModel::recheckHealth(const QStringList & cids)
{
    for (const QString & c : cids)
        if (Cids.contains(c)) { Cids[c].providers = -2; Cids[c].missing = -1; emit cidChanged(c); }
    gatherHealth();
}

void IpfsModel::unpinMany(const QStringList & cids)
{
    for (const QString & c : cids) IpfsWrapper::Unpin(c.toStdString());
    refreshNow();
}

void IpfsModel::addSource(const QString & cid, const QString & name)
{
    auto & Cfg = *Model.config();
    auto & Sources = Cfg["Settings"]["PackageSources"];
    if (!Sources.is_array()) Sources = nlohmann::ordered_json::array();
    for (const auto & S : Sources)
    {
        const std::string Have = S.is_object() ? S.value("CID", std::string())
                               : (S.is_string() ? std::string(S) : std::string());
        if (Have == cid.toStdString()) return;   // already configured
    }
    Sources.push_back({ {"NAME", name.toStdString()}, {"CID", cid.toStdString()} });
    Model.save();
    Model.syncSources();   // fetch + index it now (no-op per-CID once hydrated)
    refresh();
}

void IpfsModel::markQueued(const QString & cid)
{
    ensureLabels(cid);
    CidState & s = Cids[cid];
    s.phase = CidState::Queued; s.pct = -1.0;
    ensureSize(cid);   // fetch the byte size early (for package-progress weighting)
    emit cidChanged(cid);
}

void IpfsModel::clearQueued(const QString & cid)
{
    auto it = Cids.find(cid);
    if (it == Cids.end() || it->phase != CidState::Queued) return;
    if (!IpfsWrapper::HasLocal(cid.toStdString())) { Cids.erase(it); emit cidRemoved(cid); }
    else { it->phase = CidState::Seeded; emit cidChanged(cid); }
    refresh();
}

void IpfsModel::tick()
{
    if (LastProgress.isEmpty()) return;
    const qlonglong Now = QDateTime::currentMSecsSinceEpoch();
    constexpr qlonglong StallMs = 6000;
    for (auto it = LastProgress.constBegin(); it != LastProgress.constEnd(); ++it) {
        const QString & cid = it.key();
        if (Now - it.value() < StallMs) continue;
        auto ci = Cids.find(cid);
        if (ci == Cids.end() || ci->phase != CidState::Downloading) continue;
        // "Stalled" means a transfer went quiet MID-STREAM. Before the first byte (pct < 0) the fetch is still in
        // provider discovery — DHT walk, relay dial, holepunch, which can legitimately take minutes on a cold start —
        // and the row already says "Searching providers…"; flipping that to "Stalled" would cry wolf.
        if (ci->pct < 0) continue;
        ci->phase = CidState::Stalled; ci->speedBps = -1.0;
        emit cidChanged(cid);
    }
}

void IpfsModel::ensureSize(const QString & cid)
{
    if (Cids.value(cid).size >= 0) return;
    if (!IpfsWrapper::Available()) return;   // no node → the size stat can't run (and would outlive a short-lived model)
    std::thread([this, cid, A = Alive]{
        const long long S = IpfsWrapper::CidSize(cid.toStdString());
        if (!A->load()) return;   // model destroyed while the network stat ran → don't touch `this`
        QMetaObject::invokeMethod(this, [this, cid, S]{
            if (S >= 0 && Cids.contains(cid)) { Cids[cid].size = S; emit cidChanged(cid); }
        }, Qt::QueuedConnection);
    }).detach();
}

void IpfsModel::refresh()
{
    if (!IpfsWrapper::Available())
    {
        if (Status.available) { Status = NodeStatus{}; emit nodeStatusChanged(); }   // was up → reflect "off" once
        return;
    }
    if (RefreshInFlight) return;
    RefreshInFlight = true;

    QSet<QString> HaveSize;
    for (auto it = Cids.constBegin(); it != Cids.constEnd(); ++it)
        if (it->size >= 0) HaveSize.insert(it.key());
    // Pins whose deliverability verdict we already hold — skip the (expensive) filestore walk for these. Snapshotted
    // on the UI thread (SeedVerdict is only ever touched here + in applySnapshot, both UI-thread) and copied in.
    QSet<QString> Verified;
    for (auto it = SeedVerdict.constBegin(); it != SeedVerdict.constEnd(); ++it) Verified.insert(it.key());
    const std::string LibRoot = PackageCatalog::LibraryRootDir(*Model.config());

    std::thread([this, HaveSize, Verified, LibRoot, A = Alive]{
        NodeStatus St;
        St.available = true;
        St.daemon    = IpfsWrapper::DaemonRunning();
        St.peers     = St.daemon ? IpfsWrapper::PeerCount() : 0;
        St.repo      = QString::fromStdString(IpfsWrapper::RepoSizeHuman());
        const IpfsWrapper::BandwidthRates Bw = IpfsWrapper::Bandwidth();
        St.downBps = Bw.DownBps; St.upBps = Bw.UpBps;
        const std::vector<IpfsWrapper::PinEntry> Pins = IpfsWrapper::Pins();
        St.pinCount = (int)Pins.size();
        QSet<QString> Uploading;
        for (const auto & C : IpfsWrapper::ActiveUploads(90000)) Uploading.insert(QString::fromStdString(C));
        // LOCAL-only sizes here: pins are our own seeds, so their DAG roots are local disk reads. The
        // network-falling CidSize (bitswap+gateway, ~35s bound) must never run in this loop — a repo with
        // hundreds of orphaned filestore references made the first refresh take HOURS, wedging the status
        // pipeline behind RefreshInFlight and pinning the tab at "off" while the node was actually up.
        QHash<QString, long long> Sizes;
        for (const auto & P : Pins) {
            const QString C = QString::fromStdString(P.Cid);
            if (!HaveSize.contains(C)) { const long long S = IpfsWrapper::CidSizeLocal(P.Cid); if (S >= 0) Sizes[C] = S; }
        }
        // Deliverability: walk ONLY pins we have not verified yet (startup → all once; steady state → none; a serve
        // failure invalidates entries so they re-verify here). This is the per-pin filestore DAG-walk that used to run
        // for EVERY pin on the UI thread every 5s and froze the app — now off-thread and only when actually needed.
        QHash<QString, QString> Verdicts;
        for (const auto & P : Pins) {
            const QString C = QString::fromStdString(P.Cid);
            if (!Verified.contains(C)) Verdicts[C] = QString::fromStdString(IpfsWrapper::CidServeStatus(P.Cid));
        }
        std::error_code Ec;
        const auto Sp = std::filesystem::space(LibRoot, Ec);
        St.diskFree = Ec ? -1 : (qlonglong)Sp.available;

        if (!A->load()) return;   // model destroyed mid-refresh → don't post back to `this`
        QMetaObject::invokeMethod(this, [this, St, Pins, Sizes, Uploading, Verdicts]{
            RefreshInFlight = false;
            applySnapshot(St, Pins, Sizes, Uploading, Verdicts);
        }, Qt::QueuedConnection);
    }).detach();
}

void IpfsModel::applySnapshot(const NodeStatus & status,
                              const std::vector<IpfsWrapper::PinEntry> & pins,
                              const QHash<QString, long long> & sizes,
                              const QSet<QString> & uploading,
                              const QHash<QString, QString> & verdicts)
{
    // Fold in the deliverability verdicts computed off-thread this round (startup / newly-seen pins / post-failure
    // re-verify). From here on every displayed pin's servable/errored state is read from this cache — no UI-thread walk.
    for (auto it = verdicts.constBegin(); it != verdicts.constEnd(); ++it) SeedVerdict[it.key()] = it.value();

    // Merge freshly-gathered sizes into whatever we know.
    for (auto it = sizes.constBegin(); it != sizes.constEnd(); ++it)
        if (Cids.contains(it.key())) Cids[it.key()].size = it.value();

    // Node status strip.
    Status = status;
    long long Total = 0;
    for (const auto & P : pins) { const auto s = Cids.value(QString::fromStdString(P.Cid)).size; if (s >= 0) Total += s; }
    Status.totalSize = Total;
    emit nodeStatusChanged();

    // The set of CIDs that SHOULD be present: pins + in-flight transfers + configured-but-unfetched sources.
    QSet<QString> Desired;
    for (const auto & P : pins) Desired.insert(QString::fromStdString(P.Cid));
    for (auto it = Cids.constBegin(); it != Cids.constEnd(); ++it) {
        const CidState::Phase ph = it->phase;
        if (ph == CidState::Queued || ph == CidState::Downloading || ph == CidState::Pinning
            || ph == CidState::Stalled || ph == CidState::Errored)
            Desired.insert(it.key());
    }

    PendingSources.clear();
    {
        QSet<QString> Pinned;
        for (const auto & P : pins) Pinned.insert(QString::fromStdString(P.Cid));
        const auto & Cfg = *Model.config();
        if (Cfg.contains("Settings") && Cfg["Settings"].contains("PackageSources") && Cfg["Settings"]["PackageSources"].is_array())
            for (const auto & Src : Cfg["Settings"]["PackageSources"]) {
                const std::string C = Src.is_object() ? Src.value("CID", std::string())
                                    : Src.is_string() ? Src.get<std::string>() : std::string();
                if (C.empty()) continue;
                const QString Cid = QString::fromStdString(C);
                const bool Fetching = Cids.contains(Cid) && (Cids[Cid].phase == CidState::Downloading
                                    || Cids[Cid].phase == CidState::Queued || Cids[Cid].phase == CidState::Pinning);
                if (Pinned.contains(Cid) || Fetching) continue;
                Desired.insert(Cid);
                PendingSources.insert(Cid);
            }
    }

    // Drop entries no longer desired.
    for (const QString & cid : Cids.keys())
        if (!Desired.contains(cid)) { Cids.remove(cid); SeedVerdict.remove(cid); emit cidRemoved(cid); }

    // A peer asked for these and we could not deliver them. Strongest possible evidence: not a heuristic about
    // sizes, but an actual failed serve. This is the ON-DEMAND trigger: a failure invalidates the cached verdict so
    // the pin re-verifies (off-thread) next refresh — the reactive replacement for the old walk-everything-every-5s.
    // Applied before the pin upsert so it cannot be overwritten by it.
    bool AnyLeafFail = false;
    for (const IpfsWrapper::ServeFailure & F : IpfsWrapper::ServeFailures())
    {
        const QString cid = QString::fromStdString(F.Cid);
        if (!Cids.contains(cid)) { AnyLeafFail = true; continue; }   // a leaf block, not a displayed CID
        CidState & s = Cids[cid];
        s.phase = CidState::Errored;
        s.error = QObject::tr("a peer requested this and the bytes did not match — re-seed or re-check");
        SeedVerdict.remove(cid);             // force a re-verify next refresh to later confirm-or-clear the error
        emit cidChanged(cid);
    }
    // A failed LEAF could belong to any pin (leaves aren't displayed rows), so drop every verdict: the next refresh
    // re-verifies all pins off-thread and pins the actually-broken one. Rare (only on genuine rot) → no freeze.
    if (AnyLeafFail) SeedVerdict.clear();

    // Upsert pins as Seeded (unless mid-transfer), and pending sources as Pending.
    for (const auto & P : pins) {
        const QString cid = QString::fromStdString(P.Cid);
        CidState & s = Cids[cid];
        const bool MidTransfer = (s.phase == CidState::Downloading || s.phase == CidState::Pinning
                               || s.phase == CidState::Stalled || s.phase == CidState::Errored || s.phase == CidState::Queued);
        s.uploading = uploading.contains(cid);
        s.announced = IpfsWrapper::SeedAnnounced(P.Cid);   // "seeding" once the DHT announce is done, else "queued for seeding"
        if (!MidTransfer)
        {
            // "Seeding" has to MEAN we can hand a peer the bytes. A pin on its own proves nothing: it survives the
            // backing file being deleted, truncated or rebuilt, which is how a stale reference used to sit here
            // showing green while every request for it hung. Demote to Errored the moment a condition fails, and
            // say WHICH — the reason is the difference between "re-seed it" and "the content is gone".
            // Read the verdict computed OFF-THREAD (SeedVerdict). Absent only for a pin seen for the very first time
            // with its walk still in flight → treat as Seeded for this one tick; it gets a real verdict next refresh.
            const QString Why = SeedVerdict.value(cid, QString());
            if (Why.isEmpty()) { s.phase = CidState::Seeded; s.pct = 100.0; s.speedBps = -1.0; s.error.clear(); }
            else               { s.phase = CidState::Errored; s.error = Why; s.speedBps = -1.0; }
        }
        if (s.size < 0) ensureSize(cid);
    }
    for (const QString & cid : PendingSources) {
        CidState & s = Cids[cid];
        s.phase = CidState::Pending; s.pct = -1.0; s.size = -1;
    }

    // Label every CID in ONE pass now that all pins/pending are in the map: BuildCidLabels scans the whole catalog,
    // so calling it per-CID (the old ensureLabels-in-loop) was O(pins × nodes) — a multi-second main-thread freeze
    // once the pinset grew to hundreds. rebuildLabels builds the map once and applies it to every entry: O(nodes + cids).
    rebuildLabels();

    emit modelReset();
    gatherHealth();
}

void IpfsModel::gatherHealth()
{
    if (HealthInFlight) return;
    auto Todo = std::make_shared<QStringList>();
    for (auto it = Cids.constBegin(); it != Cids.constEnd(); ++it)
        if (it->providers == -2 && !PendingSources.contains(it.key())) *Todo << it.key();
    if (Todo->isEmpty()) return;
    HealthInFlight = true;

    const int Workers = std::min<int>(5, Todo->size());
    auto Next      = std::make_shared<std::atomic<int>>(0);
    auto Remaining = std::make_shared<std::atomic<int>>(Workers);
    for (int w = 0; w < Workers; ++w)
        std::thread([this, Todo, Next, Remaining, A = Alive]{
            for (;;) {
                if (!A->load()) return;   // model destroyed mid-scan → stop (each ProviderCount can block for seconds)
                const int i = Next->fetch_add(1);
                if (i >= Todo->size()) break;
                const QString cid = Todo->at(i);
                const std::string C = cid.toStdString();
                const int M = IpfsWrapper::CidMissing(C) ? 1 : 0;
                const int N = (M == 1) ? -1 : IpfsWrapper::ProviderCount(C);
                if (!A->load()) return;
                // Broken ref → repair (single-flight). Post to the model's thread so healOrphansIfAny touches
                // AppModel main-thread state (KnownUnhealable) on the right thread, and only while the model is alive.
                const bool Broken = (M == 1);
                QMetaObject::invokeMethod(this, [this, cid, N, M, Broken]{
                    if (Cids.contains(cid)) { Cids[cid].providers = N; Cids[cid].missing = M; emit cidChanged(cid); }
                    if (Broken) Model.healOrphansIfAny();
                }, Qt::QueuedConnection);
            }
            if (Remaining->fetch_sub(1) == 1 && A->load())
                QMetaObject::invokeMethod(this, [this]{ HealthInFlight = false; }, Qt::QueuedConnection);
        }).detach();
}

void IpfsModel::seedFolder(const QString & dir)
{
    std::thread([this, dir, A = Alive]{
        int Mismatched = 0;
        const int Seeded = PackageCatalog::SeedDirectory(dir.toStdString(),
            [this, A](int done, int total, const std::string &){
                if (A->load())
                    QMetaObject::invokeMethod(this, [this, done, total]{ emit seedProgress(done, total); }, Qt::QueuedConnection);
            }, &Mismatched);
        if (!A->load()) return;   // model destroyed during the (potentially long) seed → don't post back
        QMetaObject::invokeMethod(this, [this, Seeded, Mismatched]{
            emit seedFinished(Seeded, Mismatched);
            refresh();
        }, Qt::QueuedConnection);
    }).detach();
}
