#ifndef MOUSEMENU_H
#define MOUSEMENU_H

#include <QMenu>

class MouseMenu : public QMenu
{
    Q_OBJECT

public:
    explicit MouseMenu(QObject *parent = nullptr);
protected:
    void paintEvent(QPaintEvent *);

private:
};

#endif // MOUSEMENU_H
