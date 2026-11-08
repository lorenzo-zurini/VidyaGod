#include "offlinebanner.h"
#include "appmodel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

OfflineBanner::OfflineBanner(AppModel &Model, const QString &Text, QWidget *Parent) : QFrame(Parent)
{
    setObjectName("OfflineBanner");   // its look: Theme
    setAccessibleName("Networking is off");
    auto *L = new QHBoxLayout(this);
    L->setContentsMargins(12, 6, 8, 6);
    L->addWidget(new QLabel(Text, this), 1);
    auto *On = new QPushButton("Turn on networking", this);
    connect(On, &QPushButton::clicked, this, [&Model] { Model.setNetworkingEnabled(true); });
    L->addWidget(On);
    setVisible(!Model.networkingEnabled());
    connect(&Model, &AppModel::networkingChanged, this, [this](bool Enabled) { setVisible(!Enabled); });
}
