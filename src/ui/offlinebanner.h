#ifndef OFFLINEBANNER_H
#define OFFLINEBANNER_H

#include <QFrame>

class AppModel;

// The bar a tab that needs the network shows at its top while networking is off: what that means here, and the button
// that turns it on. It follows the model by itself (shown while AppModel::networkingEnabled() is false), so a tab only
// places it. Its look is Theme's (QFrame#OfflineBanner).
class OfflineBanner : public QFrame
{
    Q_OBJECT
public:
    OfflineBanner(AppModel &Model, const QString &Text, QWidget *Parent);
};

#endif // OFFLINEBANNER_H
