#include "mousemenu.h"
#include <QStyleOption>
#include <QPainter>
MouseMenu::MouseMenu(QObject *parent)
{
    this->setAttribute(Qt::WA_TranslucentBackground,true);
    this->setWindowFlags(Qt::FramelessWindowHint | this->windowFlags());
}
void MouseMenu::paintEvent(QPaintEvent *e)
{
    QStyleOption opt;
    opt.init(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget,&opt,&p,this);
    //::paintEvent(e);
}

