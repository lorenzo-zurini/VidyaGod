#ifndef PENEWTABWIDGET_H
#define PENEWTABWIDGET_H

#include <QWidget>

namespace Ui {
class PENewTabWidget;
}

class PENewTabWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PENewTabWidget(QWidget *parent = nullptr);
    ~PENewTabWidget();

private:
    Ui::PENewTabWidget *ui;
};

#endif // PENEWTABWIDGET_H
