#pragma execution_character_set("utf-8")

#include "videocontrol.h"
#include "qevent.h"
#include "qmenu.h"
#include "qlayout.h"
#include "qlabel.h"
#include "qwidget.h"
#include "qtimer.h"
#include "qdebug.h"
#include "quiwidget.h"
#include "controlconfig.h"
#define BROADCASTPORT 65000

VideoControl::VideoControl(QWidget *parent) : QWidget(parent)
{
    this->initControl();
    this->initForm();
    //this->initMenu();
    this->show_video_all();
    //delayToShow();
}

void VideoControl::delayToShow()
{
    QTimer::singleShot(3000, this, SLOT(play_video_all()));
}

bool VideoControl::eventFilter(QObject *watched, QEvent *event)
{
 #if 0
    if (event->type() == QEvent::MouseButtonDblClick) {
//        QLabel *widget = (QLabel *) watched;
//        if (!videoMax) {
//            videoMax = true;
//            hide_video_all();
//            gridLayout->addWidget(widget, 0, 0);
//            widget->setVisible(true);
//        } else {
//            videoMax = false;
//            show_video_all();
//        }

//        widget->setFocus();
    } else if (event->type() == QEvent::MouseButtonPress) {
        QMouseEvent *mouseEvent = (QMouseEvent *)event;
        frmwidget *frmWidget = (frmwidget*) watched;

        false_focus_all();
        frmWidget->setWidgetFcous(true);
        videoIndex = frmWidget->objectName().toInt();

        if (mouseEvent->button() == Qt::RightButton) {
            videoMenu->exec(QCursor::pos());
        }
//        else if(mouseEvent->button() == Qt::LeftButton){

//        }
    }

    return QWidget::eventFilter(watched, event);
#endif
}

QSize VideoControl::sizeHint() const
{
    return QSize(800, 600);
}

QSize VideoControl::minimumSizeHint() const
{
    return QSize(80, 60);
}

void VideoControl::initControl()
{
    gridLayout = new QGridLayout;
    gridLayout->setSpacing(1);
    gridLayout->setContentsMargins(0, 0, 0, 0);
    gridLayout->setObjectName("gridLayout");
    this->setLayout(gridLayout);
}

void VideoControl::initForm()
{
    //设置样式表
    QStringList qss;
    qss.append("QFrame{border:2px solid #000000;}");
    qss.append("QLabel{font:75 25px;color:#F0F0F0;border:2px solid #AAAAAA;background:#303030;}");
    qss.append("QLabel:focus{border:2px solid #00BB9E;background:#555555;}");
    this->setStyleSheet(qss.join(""));

    ControlConfig::ConfigFile = QString("%1/%2.ini").arg(QUIHelper::appPath()).arg(QUIHelper::appName());
    ControlConfig::readConfig();

    videoMax = false;
    videoCount = videochannelcount;
//    currentCmbIndex = ControlConfig::indexCmbNumber;
//    videoIndex = ControlConfig::currentChannelIndex;
//    videoType = ControlConfig::currentVideoType;
    currentCmbIndex = 0;
    videoIndex = 0;
    videoType = "1_4";


    for (int i = 0; i < videoCount; i++) {
        frmwidget *widget = new frmwidget(nullptr,QString("通道%1").arg(i+1));
        widget->setObjectName(QString("%1").arg(i));
        widget->installEventFilter(this);
        widget->setFocusPolicy(Qt::StrongFocus);

        widgets.append(widget);
        //添加空白站位的widgets
        frmwidget *blankwidget = new frmwidget;
        blankWidgets.append(blankwidget);
    }

    m_udpSocket = new QUdpSocket(this);

//    if(!m_udpSocket->bind(BROADCASTPORT)){

//        qDebug() << "bind failed! The assert will be triggred!";
//        Q_ASSERT(!"bind faile!");
//    }
    connect(m_udpSocket, SIGNAL(readyRead()), this, SLOT(readPendingDatagrams()));
}

QList<frmwidget *> VideoControl::getFrmWidgets()
{
    return widgets;
}

void VideoControl::readPendingDatagrams()
{
    while(m_udpSocket->hasPendingDatagrams())
    {

        QHostAddress srcAddress;
        quint16 nSrcPort;
        QByteArray datagram;
        datagram.resize(m_udpSocket->pendingDatagramSize());
        m_udpSocket->readDatagram(datagram.data(), datagram.size(), &srcAddress, &nSrcPort);;
        QString firstData = srcAddress.toString() + " " + QString::number(nSrcPort) + " :";
        qDebug()<<"firstData is:" <<firstData;


    }
}

void VideoControl::initMenu()
{
    videoMenu = new QMenu(this);
    videoMenu->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Preferred);

    actionPoll = new QAction("启动轮询视频", videoMenu);
    connect(actionPoll, SIGNAL(triggered(bool)), this, SLOT(poll()));

    videoMenu->addAction(actionPoll);
    videoMenu->addSeparator();



     videoMenu->addAction("切换到4画面", this, SLOT(show_video_4()));

    videoMenu->addAction("切换到9画面", this, SLOT(show_video_9()));


    videoMenu->addAction("切换到16画面", this, SLOT(show_video_16()));
    videoMenu->addSeparator();

    videoMenu->addAction("截图当前视频", this, SLOT(snapshot_video_one()));
    videoMenu->addSeparator();

    actionSaveVideo = new QAction("录制当前视频", videoMenu);
    connect(actionSaveVideo, SIGNAL(triggered(bool)), this, SLOT(save_current_video()));
    videoMenu->addAction(actionSaveVideo);
}

void VideoControl::save_current_video()
{
    if (actionSaveVideo->text() == "录制当前视频") {
        isSaveVideo = true;
        actionSaveVideo->setText("停止录制视频");
    } else {
        isSaveVideo = false;
        actionSaveVideo->setText("录制当前视频");
    }
    //执行录制处理
    emit sig_savevideo(isSaveVideo,this->videoIndex);    
}

void VideoControl::full()
{
    if (actionFull->text() == "切换全屏模式") {
        emit fullScreen(true);
        actionFull->setText("切换正常模式");
    } else {
        emit fullScreen(false);
        actionFull->setText("切换全屏模式");
    }

    //执行全屏处理
}

void VideoControl::poll()
{
    if (actionPoll->text() == "启动轮询视频") {
        ispoll = true;
        actionPoll->setText("停止轮询视频");
    } else {
        ispoll = false;
        actionPoll->setText("启动轮询视频");
    }

    //执行轮询处理
    emit sig_poll(ispoll);
}

void VideoControl::play_video_all()
{
    emit sig_play();
}

void VideoControl::snapshot_video_one()
{
    //emit sig_snapPicture();
    if(this->videoIndex < widgets.size())
    {
        frmwidget* widget = widgets.at(this->videoIndex);
        widget->savePicImage(true);
    }
}

void VideoControl::snapshot_video_all()
{

}

void VideoControl::show_video_all()
{
    if (videoType == "1_4") {
        change_video_4(0);
    } else if (videoType == "5_8") {
        change_video_4(4);
    } else if (videoType == "9_12") {
        change_video_4(8);
    } else if (videoType == "13_16") {
        change_video_4(12);
    } else if (videoType == "1_9") {
        change_video_9(0);
    } else if (videoType == "8_16") {
        change_video_9(7);
    } else if (videoType == "1_16") {
        change_video_16(0);
    } else if (videoType == "1_25") {
        change_video_25(0);
    } else if (videoType == "1_36") {
        change_video_36(0);
    } else if (videoType == "1_64") {
        change_video_64(0);
    }
}
int VideoControl::calcCount(QString typestr)
{
    int caclcount = 1;
    if (typestr == "1_4")
        caclcount = 4;
    else if(typestr == "1_9")
        caclcount = 9;
    else if(typestr == "1_16")
        caclcount = 16;
    else if(typestr == "1_25")
        caclcount = 25;
    else if(typestr == "1_36")
        caclcount = 36;
    else if(typestr == "1_64")
        caclcount = 64;
    return caclcount;
}

void VideoControl::onDisconnectOldShow(QString typestr,int indexstr)
{
    int count = 0,caclcount = 1;
    caclcount = calcCount(typestr);
    m_beforeIndexList.clear();
    for(int i = 0 ; i < caclcount ; i++)
    {
        int currentchannelindex = i + indexstr * caclcount;
        if(currentchannelindex < videochannelcount)
            m_beforeIndexList.append(currentchannelindex);
    }
}

void VideoControl::onConnectNewShow(int counttypestr,int indexstr)
{
    m_currentIndexList.clear();
    for(int i = 0 ; i < counttypestr ; i++)
    {
        int currentchannelindex = i + indexstr * counttypestr;
        if(currentchannelindex  < videochannelcount)
            m_currentIndexList.append(currentchannelindex);
    }
}

void VideoControl::show_video_4()
{

    QAction *action = (QAction *)sender();
    QString name = action->text();

    int index = 0;
    QString videoType;
    if (name == "通道1-通道4") {
        index = 0;
        videoType = "1_4";
    } else if (name == "通道5-通道8") {
        index = 4;
        videoType = "5_8";
    } else if (name == "通道9-通道12") {
        index = 8;
        videoType = "9_12";
    } else if (name == "通道13-通道16") {
        index = 12;
        videoType = "13_16";
    }
#if 1
    index = 0;
    videoType = "1_4";
#endif
    if (this->videoType != videoType) {
        int beforeIndex = this->currentCmbIndex;
        onDisconnectOldShow(this->videoType,beforeIndex);
        this->videoType = videoType;
        this->videoMax = false;
        emit sig_videoType(this->videoType,this->videoIndex);
        change_video_4(index);
    }
}

void VideoControl::show_video_9()
{
    QAction *action = (QAction *)sender();
    QString name = action->text();

    int index = 0;
    QString videoType;
    if (name == "通道1-通道9") {
        index = 0;
        videoType = "1_9";
    } else if (name == "通道8-通道16") {
        index = 7;
        videoType = "8_16";
    }

#if 1
    index = 0;
    videoType = "1_9";
#endif

    if (this->videoType != videoType) {
        int beforeIndex = this->currentCmbIndex;
        onDisconnectOldShow(this->videoType,beforeIndex);
        this->videoType = videoType;
        this->videoMax = false;
        emit sig_videoType(this->videoType,this->videoIndex);
        change_video_9(index);
    }
}

void VideoControl::show_video_16()
{
    int index = 0;
    QString videoType = "1_16";
    if (this->videoType != videoType) {
        int beforeIndex = this->currentCmbIndex;
        onDisconnectOldShow(this->videoType,beforeIndex);
        this->videoType = videoType;
        this->videoMax = false;
        emit sig_videoType(this->videoType,this->videoIndex);
        change_video_16(index);
    }
}

void VideoControl::show_video_25()
{
    int index = 0;
    QString videoType = "1_25";
    if (this->videoType != videoType) {
        int beforeIndex = this->currentCmbIndex;
        onDisconnectOldShow(this->videoType,beforeIndex);
        this->videoType = videoType;
        this->videoMax = false;
        emit sig_videoType(this->videoType,this->videoIndex);
        change_video_25(index);
    }
}

void VideoControl::show_video_36()
{
    int index = 0;
    QString videoType = "1_36";
    if (this->videoType != videoType) {
        int beforeIndex = this->currentCmbIndex;
        onDisconnectOldShow(this->videoType,beforeIndex);
        this->videoType = videoType;
        this->videoMax = false;
        emit sig_videoType(this->videoType,this->videoIndex);
        change_video_36(index);
    }
}

void VideoControl::show_video_64()
{
    int index = 0;
    QString videoType = "1_64";
    if (this->videoType != videoType) {
        int beforeIndex = this->currentCmbIndex;
        onDisconnectOldShow(this->videoType,beforeIndex);
        this->videoType = videoType;
        this->videoMax = false;
        emit sig_videoType(this->videoType,this->videoIndex);
        change_video_64(index);
    }
}

void VideoControl::hide_video_all()
{

    QLayoutItem* item;
    while((item = gridLayout->takeAt(0)) != 0)
    {
        if(item->widget())
        {
            gridLayout->removeWidget(item->widget());
            item->widget()->setVisible(false);
        }
    }

}

void VideoControl::false_focus_all()
{
    for (int i = 0; i < videoCount; i++) {
        widgets.at(i)->setWidgetFcous(false);
    }
}

void VideoControl::change_video(int index, int flag)
{
    int count = 0;
    int row = 0;
    int column = 0;

    //行列数一致的比如 2*2 3*4 4*4 5*5 等可以直接套用通用的公式
    //按照这个函数还可以非常容易的拓展出 10*10 16*16=256 通道界面
    for (int i = 0; i < videoCount; i++) {
        //if (i >= index) {//注销原来的index判断
        if (i >= this->currentCmbIndex*flag*flag ) {
            gridLayout->addWidget(widgets.at(i), row, column);
            widgets.at(i)->setVisible(true);

            count++;
            column++;
            if (column == flag) {
                row++;
                column = 0;
            }
        }

        if (count == (flag * flag)) {
            break;
        }
    }
    if((this->currentCmbIndex+ 1 )*flag*flag > videoCount)//处理末位显示形状不规则
    {
        for(int i = videoCount; i < (this->currentCmbIndex+ 1 )*flag*flag; i++ )
        {
            int cindex = i - videoCount;
            gridLayout->addWidget(blankWidgets.at(cindex), row, column);
            blankWidgets.at(cindex)->setVisible(true);
            count++;
            column++;
            if (column == flag) {
                row++;
                column = 0;
            }
            if (count == (flag * flag)) {
                break;
            }
        }
    }

    int caclcount = flag*flag;

    onConnectNewShow(caclcount,currentCmbIndex);
    for(int i = 0 ;i < m_beforeIndexList.size() ; i ++)
    {
        int value = m_beforeIndexList.value(i);
        if(m_currentIndexList.contains(value))
            continue;
        emit sig_disconnectOldIndexShow(value);
    }

    emit sig_connect(caclcount,currentCmbIndex);//发送starth264显示新的当前通道画面

    ControlConfig::currentVideoType = this->currentCmbIndex;
    ControlConfig::currentChannelIndex = this->videoIndex;
    ControlConfig::currentVideoType = this->videoType;
    ControlConfig::writeConfig();
}

void VideoControl::change_video_4(int index)
{
    hide_video_all();
    change_video(index, 2);
}

void VideoControl::change_video_9(int index)
{
    hide_video_all();
    change_video(index, 3);
}

void VideoControl::change_video_16(int index)
{
    hide_video_all();
    change_video(index, 4);
}

void VideoControl::change_video_25(int index)
{
    hide_video_all();
    change_video(index, 5);
}

void VideoControl::change_video_36(int index)
{
    hide_video_all();
    change_video(index, 6);
}

void VideoControl::change_video_64(int index)
{
    hide_video_all();
    change_video(index, 8);
}

void VideoControl::change_video_index(int index)
{
    int beforeIndex = this->currentCmbIndex;
    this->currentCmbIndex = index;
    onDisconnectOldShow(this->videoType,beforeIndex);
    if (this->videoType == "1_4")
        change_video_4(0);
    else if(this->videoType == "1_9")
        change_video_9(0);
    else if(this->videoType == "1_16")
        change_video_16(0);
    else if(this->videoType == "1_25")
        change_video_25(0);
    else if(this->videoType == "1_36")
        change_video_36(0);
    else if(this->videoType == "1_64")
        change_video_64(0);

}

int VideoControl::getTypeCount()
{
    return this->videoType.mid(2).toInt();
}
