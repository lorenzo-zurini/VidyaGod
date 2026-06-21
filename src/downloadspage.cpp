#include "downloadspage.h"
#include "appmodel.h"          // AppModel — config + save
#include "ipfswrapper.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QSpinBox>

#include <nlohmann/json.hpp>

DownloadsPage::DownloadsPage(AppModel &model, QWidget *parent)
    : QWidget(parent), Model(model)
{
    QVBoxLayout * pl = new QVBoxLayout(this);
    pl->setContentsMargins(12,12,12,12);
    QFormLayout * form = new QFormLayout();
    pl->addLayout(form);

    // Max simultaneous downloads — Settings.MaxConcurrentDownloads. Caps how many files fetch at once across ALL
    // packages, so one slow/stalled file no longer holds up the rest of the queue.
    QSpinBox * maxDl = new QSpinBox(this);
    maxDl->setRange(1, 32);
    maxDl->setValue(IpfsWrapper::MaxConcurrentDownloads());
    {
        auto & S = (*Model.config())["Settings"];
        if (S.contains("MaxConcurrentDownloads") && S["MaxConcurrentDownloads"].is_number_integer())
            maxDl->setValue(int(S["MaxConcurrentDownloads"]));
    }
    connect(maxDl, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v){
        IpfsWrapper::SetMaxConcurrentDownloads(v);
        (*Model.config())["Settings"]["MaxConcurrentDownloads"] = v;
        Model.save();
    });
    form->addRow("Max simultaneous downloads:", maxDl);

    QLabel * note = new QLabel("Files download in parallel up to this limit. A stalled download keeps retrying in the "
                               "background without blocking the others; raise this to fetch more at once.", this);
    note->setWordWrap(true);
    note->setStyleSheet("color:#8f98a0;font-size:9pt;");
    pl->addWidget(note);
    pl->addStretch(1);
}
