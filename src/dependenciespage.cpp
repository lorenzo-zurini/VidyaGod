#include "dependenciespage.h"
#include "depcheck.h"

#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

DependenciesPage::DependenciesPage(AppModel & model, QWidget * parent)
    : QWidget(parent)
{
    (void)model;   // system-level checks — no app state needed; ctor mirrors the other Settings pages
    QVBoxLayout * pl = new QVBoxLayout(this);
    pl->setContentsMargins(12, 12, 12, 12);
    Scroll = new QScrollArea(this);
    Scroll->setWidgetResizable(true);
    Scroll->setFrameShape(QFrame::NoFrame);
    pl->addWidget(Scroll);
    rebuild();
}

void DependenciesPage::rebuild()
{
    using namespace DepCheck;
    if (!Scroll) return;
    if (Scroll->widget()) Scroll->widget()->deleteLater();

    const SystemState St = ProbeSystem();
    const Distro D = St.distro;

    QWidget * contents = new QWidget(Scroll);
    QVBoxLayout * v = new QVBoxLayout(contents);
    Scroll->setWidget(contents);

    // ---- intro + summary + re-check ----
    QLabel * intro = new QLabel(
        "Missing libraries — especially 32-bit ones — are the most common cause of Proton/Wine game crashes. "
        "A ✓ means it's installed; ✗ means it's missing; ⚠ means the 64-bit library is present but the 32-bit "
        "(multilib) one is not, which can crash 32-bit games. Install the named package to fix.", contents);
    intro->setWordWrap(true);
    intro->setStyleSheet("color:#8f98a0;font-size:9pt;");
    v->addWidget(intro);

    int Missing = 0, Partial = 0;
    for (const DepItem & I : AllDeps())
    {
        const Status S = Check(I, St);
        if (S == Status::Missing) ++Missing;
        else if (S == Status::Partial) ++Partial;
    }

    QHBoxLayout * top = new QHBoxLayout();
    QLabel * summary = new QLabel(contents);
    if (Missing == 0 && Partial == 0)
        summary->setText("<span style='color:#5fb55f;font-weight:bold;'>✓ All dependencies satisfied.</span>");
    else
        summary->setText(QString("<span style='color:#c6a15f;font-weight:bold;'>⚠ %1 missing, %2 partial (32-bit) "
                                 "— games may crash.</span>").arg(Missing).arg(Partial));
    top->addWidget(summary, 1);
    QLabel * distro = new QLabel(QString("detected: %1").arg(DistroLabel(D)), contents);
    distro->setStyleSheet("color:#8f98a0;font-size:9pt;");
    top->addWidget(distro);
    QPushButton * recheck = new QPushButton("Re-check", contents);
    connect(recheck, &QPushButton::clicked, this, [this]{ rebuild(); });
    top->addWidget(recheck);
    v->addLayout(top);

    // ---- one group box per category ----
    for (const Category Cat : { Category::Core, Category::Graphics, Category::WineLibs, Category::Audio, Category::Media })
    {
        QGroupBox * box = new QGroupBox(CategoryLabel(Cat), contents);
        QVBoxLayout * bv = new QVBoxLayout(box);
        bv->setSpacing(4);

        for (const DepItem & I : AllDeps())
        {
            if (I.Cat != Cat) continue;
            const Status S = Check(I, St);

            QHBoxLayout * row = new QHBoxLayout();
            QLabel * glyph = new QLabel(box);
            glyph->setFixedWidth(16);
            if      (S == Status::Ok)      glyph->setText("<span style='color:#5fb55f;font-weight:bold;'>✓</span>");
            else if (S == Status::Partial) glyph->setText("<span style='color:#c6a15f;font-weight:bold;'>⚠</span>");
            else                           glyph->setText("<span style='color:#c0726a;font-weight:bold;'>✗</span>");
            row->addWidget(glyph);

            QLabel * name = new QLabel(QString::fromStdString(I.Name), box);
            name->setMinimumWidth(150);
            row->addWidget(name);

            QLabel * purpose = new QLabel(QString::fromStdString(I.Purpose), box);
            purpose->setWordWrap(true);
            purpose->setStyleSheet("color:#8f98a0;font-size:9pt;");
            row->addWidget(purpose, 1);

            if (S != Status::Ok)
            {
                const std::string Hint = InstallHint(I, S, D);
                if (!Hint.empty())
                {
                    QLabel * fix = new QLabel(QString("install <code>%1</code>").arg(QString::fromStdString(Hint)), box);
                    fix->setTextFormat(Qt::RichText);
                    fix->setStyleSheet(S == Status::Partial ? "color:#c6a15f;" : "color:#c0726a;");
                    row->addWidget(fix);
                }
            }
            bv->addLayout(row);
        }
        v->addWidget(box);
    }
    v->addStretch(1);
}
