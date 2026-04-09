#pragma execution_character_set("utf-8")

#include "frmvideocontrol.h"
#include "ui_frmvideocontrol.h"
#include "frmwidget.h"
#include <QKeyEvent>

frmVideoControl::frmVideoControl(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::frmVideoControl)
{
    ui->setupUi(this);
    initForm();
    initChannel();
    onInitShow();
}

frmVideoControl::~frmVideoControl()
{
    delete ui;
}

void frmVideoControl::initForm()
{
    // 设置窗口属性
    this->setWindowFlags(Qt::FramelessWindowHint);
    this->setAttribute(Qt::WA_TranslucentBackground);
}

void frmVideoControl::setWidgetEnable(bool isEnable)
{
    this->setEnabled(isEnable);
}

void frmVideoControl::initChannel()
{
    // 初始化视频通道
    // 创建 frmwidget 实例
    for (int i = 0; i < 16; i++) {
        frmwidget *w = new frmwidget(this, QString::number(i));
        widgets.append(w);
    }
}

void frmVideoControl::onInitShow()
{
    // 初始显示
}

void frmVideoControl::keyPressEvent(QKeyEvent *evt)
{
    // ESC 退出全屏
    if (evt->key() == Qt::Key_Escape) {
        showNormal();
    }
    QWidget::keyPressEvent(evt);
}

void frmVideoControl::on_fullBtn_clicked()
{
    showFullScreen();
}

void frmVideoControl::on_maxBtn_clicked()
{
    showMaximized();
}

void frmVideoControl::on_minBtn_clicked()
{
    showMinimized();
}

void frmVideoControl::on_closeBtn_clicked()
{
    close();
}

void frmVideoControl::on_normalBtn_clicked()
{
    showNormal();
}
