#ifndef NEWSUBGAMEFRAME_H
#define NEWSUBGAMEFRAME_H

#include <QWidget>

namespace Ui {
class NewSubGameFrame;
}

class NewSubGameFrame : public QWidget
{
    Q_OBJECT

public:
    explicit NewSubGameFrame(QWidget *parent = nullptr);
    ~NewSubGameFrame();

private:
    Ui::NewSubGameFrame *ui;
};

#endif // NEWSUBGAMEFRAME_H
