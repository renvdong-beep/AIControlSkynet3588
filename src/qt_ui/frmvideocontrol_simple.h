#ifndef FRMVIDEOCONTROL_SIMPLE_H
#define FRMVIDEOCONTROL_SIMPLE_H

#include <QWidget>
#include <QDebug>
#include <QBuffer>
#include <QMap>
#include <QKeyEvent>

namespace Ui {
class frmVideoControl;
}

/**
 * @brief 简化版视频控制面板
 * 
 * 移除了原项目中的网络通信、警告等依赖
 * 只保留核心的多路视频显示功能
 */
class frmVideoControl : public QWidget
{
    Q_OBJECT

public:
    explicit frmVideoControl(QWidget *parent = 0);
    ~frmVideoControl();
    
    void initForm();
    void setWidgetEnable(bool isEnable);

public slots:
    void initChannel();
    void onInitShow();

protected:
    void keyPressEvent(QKeyEvent *evt);

signals:
    void sig_play();

private slots:
    void on_fullBtn_clicked();
    void on_maxBtn_clicked();
    void on_minBtn_clicked();
    void on_closeBtn_clicked();
    void on_normalBtn_clicked();

private:
    Ui::frmVideoControl *ui;
    QList<class frmwidget *> widgets;
    bool showStyle = true;
};

#endif // FRMVIDEOCONTROL_SIMPLE_H
