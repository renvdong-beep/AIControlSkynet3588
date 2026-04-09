#ifndef VIDEOCONTROL_H
#define VIDEOCONTROL_H


#include <QWidget>
#include <QUdpSocket>
#include "frmwidget.h"
#include "mousemenu.h"

class QMenu;
class QLabel;
class QGridLayout;

static int videochannelcount = 100;


#ifdef quc
class Q_DECL_EXPORT VideoControl : public QWidget
#else
class VideoControl : public QWidget
#endif

{
    Q_OBJECT

public:
    explicit VideoControl(QWidget *parent = 0);
    QList<frmwidget *> getFrmWidgets();
    void change_video_index(int index);
    int getTypeCount();
    void delayToShow();
    int calcCount(QString typestr);

protected:
    bool eventFilter(QObject *watched, QEvent *event);

private:
    QGridLayout *gridLayout;    //表格布局存放视频标签
    bool videoMax;              //是否最大化
    int videoCount;             //视频通道个数
    int videoIndex;             //当前视频通道
    int currentCmbIndex;             //当前视频通道
    QString videoType;          //当前画面类型
    QMenu *videoMenu;           //右键菜单
    QAction *actionFull;        //全屏动作
    QAction *actionPoll;        //轮询动作
    QList<frmwidget *> widgets;    //视频标签集合
    QList<frmwidget *> blankWidgets;    //视频标签集合
    QUdpSocket *m_udpSocket;

    bool  ispoll = false;                //是否轮训模式

    QAction *actionSaveVideo;           //录制视频标签
    bool  isSaveVideo = false;           //是否录制视频

    QList<int>  m_beforeIndexList;
    QList<int>  m_currentIndexList;

public:
    QSize sizeHint()            const;
    QSize minimumSizeHint()     const;

public slots:
    void save_current_video();
    void onDisconnectOldShow(QString typestr,int indexstr);
    void onConnectNewShow(int counttypestr,int indexstr);
    
    // 画面布局切换（公开）
    void show_video_all();
    void show_video_4();
    void show_video_9();
    void show_video_16();
    void show_video_25();
    void show_video_36();
    void show_video_64();
protected slots:
    void readPendingDatagrams();

private slots:
    void initControl();
    void initForm();
    void initMenu();
    void full();
    void poll();


private slots:
    void play_video_all();
    void snapshot_video_one();
    void snapshot_video_all();

    void hide_video_all();
    void false_focus_all();
    void change_video(int index, int flag);
    void change_video_4(int index);
    void change_video_9(int index);
    void change_video_16(int index);
    void change_video_25(int index);
    void change_video_36(int index);
    void change_video_64(int index);



signals:
    void fullScreen(bool full);
    void sig_videoType(QString type,int index);
    void sig_snapPicture();

    void sig_disconnectOldIndexShow(int index);
    void sig_disconnect(QString type,int index);
    void sig_connect(int counttype,int index);
    void sig_poll(bool ispoll);
    void sig_savevideo(bool isavevideo,int index);

    void sig_play();
};

#endif // VIDEOCONTROL_H
