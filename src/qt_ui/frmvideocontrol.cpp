#pragma execution_character_set("utf-8")
#include "frmvideocontrol.h"
#include "ui_frmvideocontrol.h"
//#include "frmmain.h"
#include "quiwidget.h"
#include "controlconfig.h"
#include "widget.h"
#include <QStackedWidget>
#include <QSettings>
#include <QFileInfo>
#include <QDir>
#include <QThread>

#define GB (1024 * 1024 * 1024)
#define MB (1024 * 1024)
#define KB (1024)

#include "mpprknnvideo.h"

frmVideoControl::frmVideoControl(QWidget *parent) : QWidget(parent), ui(new Ui::frmVideoControl)
{
    ui->setupUi(this);
    this->setWindowFlags(Qt::FramelessWindowHint|Qt::WindowSystemMenuHint\
                         |Qt::WindowMinimizeButtonHint\
                         |Qt::WindowMaximizeButtonHint);
    ui->stackedWidget->setCurrentIndex(0);

    ui->textLabel->setText("监测系统 V2.17");

    //ui->statusWidget->setVisible(false);
    ui->preBtn->setVisible(false);
    ui->nextBtn->setVisible(false);
    ui->indexCmb->setVisible(false);
    ui->snLabel->setVisible(false);
    ui->searchBtn->setVisible(false);
    ui->serachEdit->setVisible(false);
    ui->SettingBtn->setVisible(false);
    ui->pushButton->setVisible(false);
    ui->videoConfigBtn->setVisible(false);

    this->setWidgetEnable(false);
#if 1
    frmwidget* tmpFrmWidget = new frmwidget();
    QGridLayout* gridlayout = ui->videoWidget->findChild<QGridLayout *>("gridLayout");
    gridlayout->addWidget(tmpFrmWidget,0,0);
    //QString rtsp_addr = "rtsp://192.168.0.123:8554/stream";
    QString rtsp_addr = "rtsp://admin:QSDBNT@192.168.0.136:554/h264/ch1/main/av_stream";
    int video_type = 264;
    int corenum = 0;
    workthread = new workThread(this,tmpFrmWidget,rtsp_addr,video_type,corenum);
    qDebug()<<"frmvideocontrol thread in run is  "<<QThread::currentThread();
    workthread->start();
#endif

    QThread::msleep(300);

#if 0
    frmwidget* tmpFrmWidget1 = new frmwidget();
    QGridLayout* gridlayout1 = ui->videoWidget->findChild<QGridLayout *>("gridLayout");
    gridlayout1->addWidget(tmpFrmWidget1,0,1);
    QString rtsp_addr1 = "rtsp://192.168.0.123:8554/stream";
//    QString rtsp_addr1 = "rtsp://admin:QSDBNT@192.168.0.136:554/h264/ch1/main/av_stream";
//    QString rtsp_addr1 = "rtsp://admin:li147369@weisungtech.com:20012/h265/ch1/sub/av_stream";
    int video_type1 = 264;
    int corenum1 = 1;
    workthread1 = new workThread(this,tmpFrmWidget1,rtsp_addr1,video_type1,corenum1);
    workthread1->start();
 #endif

    QThread::msleep(300);

#if 0
    frmwidget* tmpFrmWidget2 = new frmwidget();
    QGridLayout* gridlayout2 = ui->videoWidget->findChild<QGridLayout *>("gridLayout");
    gridlayout2->addWidget(tmpFrmWidget2,1,0);
    QString rtsp_addr2 = "rtsp://192.168.0.196:8554/stream";
    //QString rtsp_addr2 = "rtsp://admin:li147369@weisungtech.com:20012/h265/ch1/main/av_stream";
    int video_type2 = 264;
    int corenum2 = 2;
    workthread2 = new workThread(this,tmpFrmWidget2,rtsp_addr2,video_type2,corenum2);
    //qDebug()<<"frmvideocontrol thread in run is  "<<QThread::currentThread();
    workthread2->start();
#endif

    QThread::msleep(300);

#if 0
    frmwidget* tmpFrmWidget3 = new frmwidget();
    QGridLayout* gridlayout3 = ui->videoWidget->findChild<QGridLayout *>("gridLayout");
    gridlayout3->addWidget(tmpFrmWidget3,1,1);
    QString rtsp_addr3 = "rtsp://admin:li147369@weisungtech.com:20025/h265/ch1/main/av_stream";
    int video_type3 = 264;
    int corenum3 = 3;
    workthread3 = new workThread(this,tmpFrmWidget3,rtsp_addr3,video_type3,corenum3);
    //qDebug()<<"frmvideocontrol thread in run is  "<<QThread::currentThread();
    workthread3->start();
#endif



//    ControlConfig::ConfigFile = QString("%1/%2.ini").arg(QUIHelper::appPath()).arg(QUIHelper::appName());
//    ControlConfig::readConfig();
//    this->initConfig();

//    ControlConfig::readConfigExcelData();

//    ControlConfig::currentStyle == 1 ?  loadStyle(":/qss/blacksoft.css"): loadStyle(":/qss/lightblue.css");

    this->initChannel();
    ui->pushButton->hide();
}

frmVideoControl::~frmVideoControl()
{
    if(!firstConfig)
    {
       // m_udpThread->stopthread();
      //  m_udpThread->wait();
    }
    workthread->stopthread();
    workthread->wait();

//    workthread1->stopthread();
//    workthread1->wait();

//    workthread2->stopthread();
//    workthread2->wait();

//    workthread3->stopthread();
//    workthread3->wait();

    delete ui;
}

void frmVideoControl::mousePressEvent(QMouseEvent *qevent)
{
    if(qevent->button() == Qt::LeftButton)
    {
        mouse_press = true;
        move_point = qevent->pos();;
    }
}
void frmVideoControl::mouseMoveEvent(QMouseEvent *qevent)
{
    if(mouse_press)
    {
        QPoint move_pos = qevent->globalPos();
        this->move(move_pos - move_point);
    }
}

void frmVideoControl::mouseReleaseEvent(QMouseEvent *qevent)
{
    mouse_press = false;
}

void frmVideoControl::setWidgetEnable(bool isEnable)
{
    ui->fullBtn->setEnabled(isEnable);
    ui->maxBtn->setEnabled(isEnable);
    ui->normalBtn->setEnabled(isEnable);
    ui->minBtn->setEnabled(isEnable);
    ui->styleBtn->setEnabled(isEnable);

    ui->videoWidget->setEnabled(isEnable);
    ui->preBtn->setEnabled(isEnable);
    ui->indexCmb->setEnabled(isEnable);
    ui->nextBtn->setEnabled(isEnable);
    ui->searchBtn->setEnabled(isEnable);
    ui->serachEdit->setEnabled(isEnable);
    ui->SettingBtn->setEnabled(isEnable);
    ui->pushButton->setEnabled(isEnable);
}

void frmVideoControl::initChannel()
{
    //int channelcount = ControlConfig::snQStringList.size();
    int channelcount = 4;
    if(channelcount > 0){
        firstConfig = false;
        initForm();
        this->setWidgetEnable(true);

        for(int i = 0 ; i < channelcount ; i++)
        {
//            QString id = QString::number(i+1);
//            QString snstr = ControlConfig::snQStringList.value(i);
//            QString descstr = ControlConfig::descQStringList.value(i);
//            clientTo3399* clientto3399 = new clientTo3399(this,id,snstr,"",descstr);
//            clientTo3399Map.insert(id,clientto3399);
//            snMap.insert(snstr,id);
//            descMap.insert(descstr,id);
//            descipMap.insert(descstr,"");//初始化descipmap
        }
        //cow init
        QString id = "1";
        QString snstr = "001";
        QString descstr = "camera1";
        clientTo3399* clientto3399 = new clientTo3399(this,id,snstr,"",descstr);
        clientTo3399Map.insert(id,clientto3399);
        snMap.insert(snstr,id);
        descMap.insert(descstr,id);
        descipMap.insert(descstr,"");//初始化descipmap

        id = "2";
        snstr = "002";
        descstr = "camera2";
        clientto3399 = new clientTo3399(this,id,snstr,"",descstr);
        clientTo3399Map.insert(id,clientto3399);
        snMap.insert(snstr,id);
        descMap.insert(descstr,id);
        descipMap.insert(descstr,"");//初始化descipmap

        id = "3";
        snstr = "003";
        descstr = "camera3";
        clientto3399 = new clientTo3399(this,id,snstr,"",descstr);
        clientTo3399Map.insert(id,clientto3399);
        snMap.insert(snstr,id);
        descMap.insert(descstr,id);
        descipMap.insert(descstr,"");//初始化descipmap

        id = "4";
        snstr = "004";
        descstr = "camera4";
        clientto3399 = new clientTo3399(this,id,snstr,"",descstr);
        clientTo3399Map.insert(id,clientto3399);
        snMap.insert(snstr,id);
        descMap.insert(descstr,id);
        descipMap.insert(descstr,"");//初始化descipmap
    }   

}

void frmVideoControl::onInitShow()
{
    onConnectNewShow(calcCount(ControlConfig::currentVideoType),ui->indexCmb->currentIndex());//
}

void frmVideoControl::initConfig()
{


    int Index = ControlConfig::currentChannelIndex;
    QString Type = ControlConfig::currentVideoType;
    //onChangeCmbIndex(Type,Index);
    int caclcount = calcCount(Type);
    int count = (videochannelcount/caclcount) + ((0 == videochannelcount%caclcount)?0:1);
    ui->indexCmb->clear();
    for(int i = 0; i < count ; i++)
        ui->indexCmb->addItem(QString::number(i+1));

    int indexcmb= ControlConfig::indexCmbNumber;
    ui->indexCmb->setCurrentIndex(indexcmb);
    connect(ui->indexCmb, SIGNAL(currentIndexChanged(int)), this, SLOT(saveConfig()));
}

void frmVideoControl::saveConfig()
{
    ControlConfig::indexCmbNumber = ui->indexCmb->currentIndex();
    ControlConfig::writeConfig();
}

void frmVideoControl::initForm()
{
    //this->initToolsControl();
//    m_udpThread = new udpThread(this);
//    connect(m_udpThread, SIGNAL(sig_newMachine(QString,QString)), this, SLOT(onNewMachine(QString,QString)));

//    connect(ui->nextBtn, SIGNAL(clicked()), this, SLOT(onNextBtnPress()));
//    connect(ui->preBtn, SIGNAL(clicked()), this, SLOT(onPreBtnPress()));

    settingPage = new SettingPage();
    settingPage->setWindowTitle("设置");
    connect(settingPage,&SettingPage::sig_deleteDays,this, [=](int days){
        delayNumsToDelte =days;
    });
    connect(settingPage,&SettingPage::sig_polltime,this, [=](QString polltime){
        pollTime = polltime.toInt();
    });
    connect(settingPage,&SettingPage::sig_saveTime,this, [=](QString timestr){
        emit sig_saveTime(timestr);
    });
    connect(settingPage,&SettingPage::sig_continueTime,this, [=](QString timestr){
        emit sig_continueTime(timestr);
    });
    connect(settingPage,&SettingPage::sig_upload,this, [=](){//打开升级助手
        ControlConfig::writeUploadData(&descipMap);
        QString EXEName_Dst = "uploader.exe";
        QString CMD = QDir::currentPath();
        CMD = CMD + "/"+ EXEName_Dst;
           //执行CMD命令
            /*************非阻塞式调用***********/
            QProcess process(this);
            process.startDetached(CMD);
    });
    settingPage->firstConfig();

    connect(ui->videoWidget,SIGNAL(sig_videoType(QString,int)),this,SLOT(onChangeCmbIndex(QString,int)));
    connect(ui->videoWidget,SIGNAL(sig_disconnectOldIndexShow(int)),this,SLOT(onDisconnectOldIndexShow(int)));
    connect(ui->videoWidget,SIGNAL(sig_connect(int,int)),this,SLOT(onConnectNewShow(int,int)));//h264 show widget
    connect(ui->videoWidget,SIGNAL(sig_play()),this,SLOT(onInitShow()));
    connect(ui->videoWidget,&VideoControl::sig_poll,this, [=](bool ipoll){
        isPoll = ipoll;
        pollCountTime = ipoll ? pollTime : 0;
    });


    warningwidgetlist = new WarningWidgetList();
    warningwidgetlist->setWindowFlags(Qt::FramelessWindowHint |Qt::WindowStaysOnTopHint);
    warningwidgetlist->setWindowTitle("报警窗口");
    warningmsgBox=new MsgBox(nullptr,2,tr("  提示  "),tr("确定关闭所有报警界面？"),tr("确定"),tr("取消"));
    connect(warningwidgetlist,&WarningWidgetList::sig_showCloseMsgBox, warningmsgBox, &MsgBox::show);


    QTimer* timer = new QTimer(this);
    connect(timer,&QTimer::timeout, this, [=](){
        //时间更新
        ui->timeLabel->setText(DATETIME);

        //磁盘报警
//        deviceCountTime--;
//        if(0 == deviceCountTime)
//        {
//            DeviceSizeWarning();
//            deviceCountTime = 60 * 5;
//        }

//        if(isPoll)//处理轮询
//        {
//            pollCountTime--;
//            if(0 == pollCountTime)
//            {
//                onPoll();
//                pollCountTime = pollTime;
//            }
//        }

        //if(QDateTime::currentDateTime().toString("HHmmss") == "220000")//自动删除图片  每天固定的时间才删除 晚上10.00
        if(QDateTime::currentDateTime().toString("ss") == "00")//自动删除图片  每天整点时间删除
        {
//            detachFile("/picture/");
//            detachFile("/errorPicture/");
//            detachFile("/video/");
            detachFile(ControlConfig::normalPath);
            detachFile(ControlConfig::ngPath);
            detachFile(ControlConfig::videoPath);
            QString runPath = QCoreApplication::applicationDirPath();
            detachFile(runPath + "/Logo/");
        }
    });
    timer->start(1000);
    qDebug()<<"ui->indexCmb:"<<ui->indexCmb->currentIndex();
}

void frmVideoControl::onPoll()
{
    int count = ui->indexCmb->count();
    int index = ui->indexCmb->currentIndex();
    if(index + 1 != count)
        ui->nextBtn->clicked();
    else
        ui->indexCmb->setCurrentIndex(0);
}

void frmVideoControl::onNewMachine(QString sn,QString ip)
{
    if(widgets.isEmpty())
        widgets = ui->videoWidget->getFrmWidgets();

//    qDebug()<<"onNewMachine:"<<sn;

    if(!snMap.contains(sn))
    {
        return;
    }




    QString idstr = snMap.value(sn);
    clientTo3399* clientto3399 = clientTo3399Map.value(idstr);
    descipMap[clientto3399->getDescStr()]=ip;
    frmwidget* tmpFrmWidget = widgets.at(idstr.toInt()-1);    

    if(ip != clientto3399->getIpStr())
    {
        clientto3399->setIpStr(ip);
        connect(clientto3399, SIGNAL(sig_disconnect()), tmpFrmWidget, SLOT(onDisconnectShow()));
        //connect(clientto3399, SIGNAL(sendImage(uchar*,int ,int )), tmpFrmWidget, SLOT(updateImage(uchar*,int ,int)));
        connect(clientto3399, SIGNAL(sig_errorPic(QString,uchar*,int ,int)), this, SLOT(onShowErrorPic(QString,uchar*,int ,int)));
        connect(this, &frmVideoControl::sig_saveTime,clientto3399, [=](QString timestr){
            clientto3399->setSaveVideoTime(timestr);
        });
        connect(this, &frmVideoControl::sig_continueTime,clientto3399, [=](QString timestr){
            clientto3399->setContinueVideoTime(timestr);
        });
        connect(ui->videoWidget, &VideoControl::sig_savevideo,this, [=](bool isavevideo,int index){
             clientTo3399* clientto3399 = clientTo3399Map.value(QString::number(index+1));
             if(clientto3399)
             {
                 clientto3399->setSaveVideoStatus(isavevideo);
                 if(index+1 == idstr.toInt())
                    tmpFrmWidget->setSaveVideoTip(isavevideo);
             }
             ui->devicesizeLabel->setText((isavevideo && clientto3399)?("通道"+QString::number(index+1)+"正在录制视频....."):"");
        });
        connect(clientto3399, &clientTo3399::sig_saveVideoEnd,ui->videoWidget, [=](){
            ui->videoWidget->save_current_video();
            ui->devicesizeLabel->setText("");
        });
        connect(clientto3399, &clientTo3399::sig_showEndMsgBox,this, [=](){
            MsgBox *msgbox=new MsgBox(nullptr,4,tr("  提示  "),tr(" 录制结束!"),tr(""),tr("确定"));
            qDebug()<<"录制结束:frmvideocontrol";
            msgbox->exec();
        });

        QPushButton* startbtn = tmpFrmWidget->findChild<QPushButton *>("startBtn_2");
        QPushButton* stopbtn = tmpFrmWidget->findChild<QPushButton *>("stopBtn_2");
        QPushButton* pausebtn = tmpFrmWidget->findChild<QPushButton *>("pauseBtn_2");
        QPushButton* resetbtn = tmpFrmWidget->findChild<QPushButton *>("resetBtn_2");
        QPushButton* leftbtn = tmpFrmWidget->findChild<QPushButton *>("leftBtn_2");
        QPushButton* rightbtn = tmpFrmWidget->findChild<QPushButton *>("rightBtn_2");
        connect(pausebtn, SIGNAL(clicked()), clientto3399, SIGNAL(sig_pauseBtn()));
        connect(resetbtn, SIGNAL(clicked()), clientto3399, SIGNAL(sig_resetBtn()));

        connect(startbtn, &QPushButton::clicked, this, [=](){
            bool btnstatus = tmpFrmWidget->getStartStop();
            if(!btnstatus)
            {
                startbtn->setText("停止");
                tmpFrmWidget->setStartStop(true);
                clientto3399->sig_startBtn();
            }
            else
            {
                startbtn->setText("启动");
                tmpFrmWidget->setStartStop(false);
                clientto3399->sig_stopBtn();
            }

            //处理ng画面的按钮
            //qDebug()<<"tmpWidgetMap.keys() :" << tmpWidgetMap.keys();
            for(int i = 0; i < tmpWidgetMap.keys().count();i++)
            {
                if(tmpWidgetMap.keys().value(i).contains(sn))
                {
                    WarningWidget* warningWidget = tmpWidgetMap.value(tmpWidgetMap.keys().value(i));
                    QPushButton* startbtn = warningWidget->findChild<QPushButton *>("startBtn");
                    startbtn->setText(!btnstatus ? "停止" : "启动");
                    frmwidget* FrmWidget = warningWidget->findChild<frmwidget*>("errorVideoWidget");
                    FrmWidget->setStartStop(!btnstatus);
                }
            }


        });
        connect(leftbtn, &QPushButton::clicked, this, [=](){
            bool btnstatus = tmpFrmWidget->getLeftStop();
            if(!btnstatus)
            {
                leftbtn->setText("向左停止");
                tmpFrmWidget->setLeftStop(true);
                clientto3399->sig_leftBtn();
            }
            else
            {
                leftbtn->setText("向左");
                tmpFrmWidget->setLeftStop(false);
                clientto3399->sig_leftstopBtn();
            }
            //处理ng画面的按钮
            for(int i = 0; i < tmpWidgetMap.keys().count();i++)
            {
                if(tmpWidgetMap.keys().value(i).contains(sn))
                {
                    WarningWidget* warningWidget = tmpWidgetMap.value(tmpWidgetMap.keys().value(i));
                    QPushButton* leftBtn = warningWidget->findChild<QPushButton *>("leftBtn");
                    leftBtn->setText(!btnstatus ? "向左停止" : "向左");
                    frmwidget* FrmWidget = warningWidget->findChild<frmwidget*>("errorVideoWidget");
                    FrmWidget->setLeftStop(!btnstatus);
                }
            }
        });
        connect(rightbtn, &QPushButton::clicked, this, [=](){
            bool btnstatus = tmpFrmWidget->getRightStop();
            if(!btnstatus)
            {
                rightbtn->setText("向右停止");
                tmpFrmWidget->setRightStop(true);
                clientto3399->sig_rightBtn();
            }
            else
            {
                rightbtn->setText("向右");
                tmpFrmWidget->setRightStop(false);
                clientto3399->sig_rightstopBtn();
            }
            //处理ng画面的按钮
            for(int i = 0; i < tmpWidgetMap.keys().count();i++)
            {
                if(tmpWidgetMap.keys().value(i).contains(sn))
                {
                    WarningWidget* warningWidget = tmpWidgetMap.value(tmpWidgetMap.keys().value(i));
                    QPushButton* rightBtn = warningWidget->findChild<QPushButton *>("rightBtn");
                    rightBtn->setText(!btnstatus ? "向右停止" : "向右");
                    frmwidget* FrmWidget = warningWidget->findChild<frmwidget*>("errorVideoWidget");
                    FrmWidget->setRightStop(!btnstatus);
                }
            }
        });

        tmpFrmWidget->setDescStr(clientto3399->getDescStr());
        tmpFrmWidget->setIpStr(clientto3399->getIpStr());
        tmpFrmWidget->setSnStr(sn);
    }

}


void frmVideoControl::initToolsControl()
{

    AppConfig::Intervals << "1" << "10" << "20" << "50" << "100" << "200" << "300" << "500" << "1000" << "1500" << "2000" << "3000" << "5000" << "10000";
    AppConfig::ConfigFile = QString("%1/%2.ini").arg(QUIHelper::appPath()).arg(QUIHelper::appName());
    AppConfig::readConfig();
    AppConfig::readSendData();
    AppConfig::readDeviceData();


}

void frmVideoControl::on_SettingBtn_clicked()
{    
    settingPage->show();
}
void frmVideoControl::onOpenXsl()
{
#if 0
    QString str = QFileDialog::getOpenFileName(this,QStringLiteral("选择Excel文件"),"",tr("Exel file(*.xls *.xlsx)"));
    if(str.isEmpty())
    {
        //delete excel;
        return;
    }

    QAxObject *excel = new QAxObject(this);//建立excel操作对象
    excel->setControl("Excel.Application");//连接Excel控件
    excel->setProperty("Visible", false);//显示窗体看效果,选择ture将会看到excel表格被打开
    excel->setProperty("DisplayAlerts", true);//显示警告看效果
    QAxObject *workbooks = excel->querySubObject("WorkBooks");//获取工作簿(excel文件)集合

    workbooks->dynamicCall("Open(const QString&)", str);//打开刚才选定的excel
    QAxObject *workbook = excel->querySubObject("ActiveWorkBook");
    QAxObject *worksheet = workbook->querySubObject("WorkSheets(int)",1);
    QAxObject *usedRange = worksheet->querySubObject("UsedRange");//获取表格中的数据范围

    QVariant var = usedRange->dynamicCall("Value");//将所有的数据读取刀QVariant容器中保存
    QList<QList<QVariant>> excel_list;//用于将QVariant转换为Qlist的二维数组
    QVariantList varRows=var.toList();
    if(varRows.isEmpty())
     {
         return;
     }
    const int row_count = varRows.size();
    QVariantList rowData;
    for(int i=0;i<row_count;++i)
    {
        rowData = varRows[i].toList();
        excel_list.push_back(rowData);
    }//转换完毕

    if(0)
    {
        m_udpThread->stopthread();
        QMap<QString,clientTo3399 *>::iterator itid = clientTo3399Map.begin();
        while(itid != clientTo3399Map.end())
        {
            delete itid.value();
            itid++;
        }
    }

    ControlConfig::snQStringList.clear();
    ControlConfig::descQStringList.clear();
    int channelcount = excel_list.size() > videochannelcount ? videochannelcount : excel_list.size();
    for(int i = 0; i < channelcount;i++)
    {
        QList<QVariant> rowData = excel_list.value(i);
        if(rowData.size()<2)//小于两列为无效文件
            continue;

        QString id =QString::number(i+1);
        QString snstr = rowData.value(0).toString();
        QString descstr = rowData.value(1).toString();
        QString ipstr = "";

        ControlConfig::snQStringList.append(snstr);
        ControlConfig::descQStringList.append(descstr);
    }
    ControlConfig::writeConfigExcelData();

    workbook->dynamicCall( "Close(Boolean)", false );
    excel->dynamicCall( "Quit(void)" );
    delete excel;
    MsgBox *msgbox=new MsgBox(nullptr,4,tr("  提示  "),tr(" 读取成功，请重启软件进行使用!!"),tr(""),tr("确定"));
    connect(msgbox, &MsgBox::sig_cancelClicked, this,  [=](){
        this->close();
    });
    msgbox->exec();
#endif
}

int frmVideoControl::calcCount(QString typestr)
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

void frmVideoControl::onDisconnectOldIndexShow(int index)
{
    clientTo3399Map.value(QString::number(index + 1))->sig_stopSendH264();
}

void frmVideoControl::onDisconnectOldShow(QString typestr,int indexstr)
{
    int count = 0,caclcount = 1;
    caclcount = calcCount(typestr);
    for(int i = 0 ; i < caclcount ; i++)
    {
        int currentchannelindex = i + indexstr * caclcount;
        if(currentchannelindex < videochannelcount)
            clientTo3399Map.value(QString::number(currentchannelindex + 1))->sig_stopSendH264();
    }
}

void frmVideoControl::onConnectNewShow(int counttypestr,int indexstr)// 4 0
{
    for(int i = 0 ; i < counttypestr ; i++)
    {
        int currentchannelindex = i + indexstr * counttypestr;
        if(currentchannelindex  < videochannelcount)
            clientTo3399Map.value(QString::number(currentchannelindex + 1))->sig_startSendH264();//sig to camera  ,this project not used
    }
}

void frmVideoControl::onShowErrorPic(QString snstr,uchar* ch,int datawidth,int dataheight)
{
    QString errorPicStr = snstr + "-" +QDateTime::currentDateTime().toString("HHmmss");//用来btnlist显示


    QString idstr = snMap.value(snstr);
    clientTo3399* clientto3399 = clientTo3399Map.value(idstr);
    QString descstr = clientto3399->getDescStr();
    QString btnPicStr = descstr + "-" +QDateTime::currentDateTime().toString("HHmmss");//用来btnlist显示
    warningSnList.append(errorPicStr);

    WarningWidget* warningwidget = new WarningWidget(this);
    warningwidget->setObjectName(errorPicStr);
    frmwidget* frmWidget = warningwidget->findChild<frmwidget *>("errorVideoWidget");
    frmWidget->setIdStr("通道"+idstr);
    frmWidget->setSnStr(snstr);
    frmWidget->setIpStr(clientto3399->getIpStr());
    frmWidget->setDescStr(clientto3399->getDescStr());
    frmWidget->setRedRect(true);
    frmWidget->updateErrorImage(ch,datawidth,dataheight);
    QWidget* btnWidget = frmWidget->findChild<QWidget *>("btnWidget");
    btnWidget->hide();

    tmpWidgetMap.insert(errorPicStr,warningwidget);


    QPushButton* startbtn = warningwidget->findChild<QPushButton *>("startBtn");
    QPushButton* stopbtn = warningwidget->findChild<QPushButton *>("stopBtn");
    QPushButton* pausebtn = warningwidget->findChild<QPushButton *>("pauseBtn");
    QPushButton* resetbtn = warningwidget->findChild<QPushButton *>("resetBtn");
    QPushButton* leftbtn = warningwidget->findChild<QPushButton *>("leftBtn");
    QPushButton* rightbtn = warningwidget->findChild<QPushButton *>("rightBtn");
    QPushButton* closebtn = warningwidget->findChild<QPushButton *>("closeCurentWarningBtn");
    connect(pausebtn, SIGNAL(clicked()), clientto3399, SIGNAL(sig_pauseBtn()));
    connect(resetbtn, SIGNAL(clicked()), clientto3399, SIGNAL(sig_resetBtn()));
    connect(closebtn, SIGNAL(clicked()), this, SLOT(onCloseWarningWidget()));

    frmwidget* tmpFrmWidget = widgets.at(idstr.toInt()-1);
    QPushButton* startbtn_videowidget = tmpFrmWidget->findChild<QPushButton *>("startBtn_2");
    QPushButton* leftbtn_videowidge = tmpFrmWidget->findChild<QPushButton *>("leftBtn_2");
    QPushButton* rightbtn_videowidge = tmpFrmWidget->findChild<QPushButton *>("rightBtn_2");
    //初始同步ng的按钮状态为当前通道的状态
    bool btnstatus;
    btnstatus = tmpFrmWidget->getStartStop();
    startbtn->setText(!btnstatus ? "启动" : "停止");
    frmWidget->setStartStop(!btnstatus ? false : true);
    btnstatus = tmpFrmWidget->getLeftStop();
    leftbtn->setText(!btnstatus ? "向左" : "向左停止");
    frmWidget->setLeftStop(!btnstatus ? false : true);
    btnstatus = tmpFrmWidget->getRightStop();
    rightbtn->setText(!btnstatus ? "向右" : "向右停止");
    frmWidget->setRightStop(!btnstatus ? false : true);

    connect(startbtn, &QPushButton::clicked, this, [=](){
        bool btnstatus = frmWidget->getStartStop();
        if(!btnstatus)
        {
            startbtn_videowidget->setText("停止");
            tmpFrmWidget->setStartStop(true);
            clientto3399->sig_startBtn();
        }
        else
        {
            startbtn_videowidget->setText("启动");
            tmpFrmWidget->setStartStop(false);
            clientto3399->sig_stopBtn();
        }

        //处理ng画面的按钮
        for(int i = 0; i < tmpWidgetMap.keys().count();i++)
        {
            if(tmpWidgetMap.keys().value(i).contains(snstr))
            {
                WarningWidget* warningWidget = tmpWidgetMap.value(tmpWidgetMap.keys().value(i));
                QPushButton* startbtn = warningWidget->findChild<QPushButton *>("startBtn");
                startbtn->setText(!btnstatus ? "停止" : "启动");
                frmwidget* FrmWidget = warningWidget->findChild<frmwidget*>("errorVideoWidget");
                FrmWidget->setStartStop(!btnstatus);
            }
        }

    });
    connect(leftbtn, &QPushButton::clicked, this, [=](){
        bool btnstatus = frmWidget->getLeftStop();
        if(!btnstatus)
        {
            leftbtn_videowidge->setText("向左停止");
            tmpFrmWidget->setLeftStop(true);
            clientto3399->sig_leftBtn();
        }
        else
        {
            leftbtn_videowidge->setText("向左");
            tmpFrmWidget->setLeftStop(false);
            clientto3399->sig_leftstopBtn();
        }
        //处理ng画面的按钮
        for(int i = 0; i < tmpWidgetMap.keys().count();i++)
        {
            if(tmpWidgetMap.keys().value(i).contains(snstr))
            {
                WarningWidget* warningWidget = tmpWidgetMap.value(tmpWidgetMap.keys().value(i));
                QPushButton* leftBtn = warningWidget->findChild<QPushButton *>("leftBtn");
                leftBtn->setText(!btnstatus ? "向左停止" : "向左");
                frmwidget* FrmWidget = warningWidget->findChild<frmwidget*>("errorVideoWidget");
                FrmWidget->setLeftStop(!btnstatus);
            }
        }
    });
    connect(rightbtn, &QPushButton::clicked, this, [=](){
        bool btnstatus = frmWidget->getRightStop();
        if(!btnstatus)
        {
            rightbtn_videowidge->setText("向右停止");
            tmpFrmWidget->setRightStop(true);
            clientto3399->sig_rightBtn();
        }
        else
        {
            rightbtn_videowidge->setText("向右");
            tmpFrmWidget->setRightStop(false);
            clientto3399->sig_rightstopBtn();
        }
        //处理ng画面的按钮
        for(int i = 0; i < tmpWidgetMap.keys().count();i++)
        {
            if(tmpWidgetMap.keys().value(i).contains(snstr))
            {
                WarningWidget* warningWidget = tmpWidgetMap.value(tmpWidgetMap.keys().value(i));
                QPushButton* rightBtn = warningWidget->findChild<QPushButton *>("rightBtn");
                rightBtn->setText(!btnstatus ? "向右停止" : "向右");
                frmwidget* FrmWidget = warningWidget->findChild<frmwidget*>("errorVideoWidget");
                FrmWidget->setRightStop(!btnstatus);
            }
        }
    });



    warningwidgetlist->setFixedSize(width() *2 / 3,height() *2 / 3);
    warningwidgetlist->addNewWarningWidget(errorPicStr,btnPicStr,warningwidget);
    connect(warningmsgBox, &MsgBox::sig_okClicked, this,  [=](){
        if(0 != warningSnList.count())
            onCloseWarningWidget();
    });
    warningwidgetlist->showNormal();



}

void frmVideoControl::onCloseWarningWidget()
{

    QStackedWidget* stackwidget = warningwidgetlist->findChild<QStackedWidget*>("stackedWidget");
    WarningWidget* warnningwidget = static_cast<WarningWidget*>(stackwidget->currentWidget());
    QString errorPicStr = warnningwidget->objectName();
    tmpWidgetMap.remove(errorPicStr);
    warningwidgetlist->removeNewWarningWidget(errorPicStr,warnningwidget);
    delete warnningwidget;
    warnningwidget = NULL;
    warningSnList.removeOne(errorPicStr);
    if(0 == warningSnList.count())
    {
        warningwidgetlist->close();
    }
}

void frmVideoControl::on_indexCmb_currentIndexChanged(int index)
{
    ui->videoWidget->change_video_index(index);
}

void frmVideoControl::onChangeCmbIndex(QString typestr,int indexstr)//显示通道类型变了，index的内容也要随着改变
{
    int count = 0,caclcount = 1;
    int tmpindex = ui->indexCmb->currentIndex();
    int tmpcount = ui->indexCmb->count();
    caclcount = calcCount(typestr);
    count = (videochannelcount/caclcount) + ((0 == videochannelcount%caclcount)?0:1);

    while(tmpcount < count)
    {
        ui->indexCmb->addItem(QString::number(tmpcount+1));
        tmpcount++;
    }
    while(tmpcount > count)
    {
        ui->indexCmb->removeItem(tmpcount - 1);
        tmpcount--;
    }


   ui->indexCmb->setCurrentIndex(tmpindex > count - 1 ? count - 1 : tmpindex);
   ControlConfig::indexCmbNumber = ui->indexCmb->currentIndex();
   ControlConfig::writeConfig();
}

void frmVideoControl::onNextBtnPress()
{
    int tmpindex = ui->indexCmb->currentIndex();
    int count = ui->indexCmb->count();
    ui->indexCmb->setCurrentIndex( tmpindex + 1  < count ?  tmpindex + 1 : tmpindex);
}
void frmVideoControl::onPreBtnPress()
{
    int tmpindex = ui->indexCmb->currentIndex();
    ui->indexCmb->setCurrentIndex( tmpindex != 0 ?  tmpindex - 1 : 0);
}

void frmVideoControl::DeviceSizeWarning()
{
#if 0
            QString dirName = QDir::currentPath().mid(0,3);
            //qDebug()<<"current"<<dirName;
            LPCWSTR lpcwstrDriver = (LPCWSTR)dirName.utf16();
            ULARGE_INTEGER liFreeBytesAvailable, liTotalBytes, liTotalFreeBytes;

            if (GetDiskFreeSpaceEx(lpcwstrDriver, &liFreeBytesAvailable, &liTotalBytes, &liTotalFreeBytes)) {
                QString use = QString::number((double)(liTotalBytes.QuadPart - liTotalFreeBytes.QuadPart) / GB, 'f', 1);
                use += "G";
                QString free = QString::number((double) liTotalFreeBytes.QuadPart / GB, 'f', 1);
                free += "G";
                QString all = QString::number((double) liTotalBytes.QuadPart / GB, 'f', 1);
                all += "G";
                int percent = 100 - ((double)liTotalFreeBytes.QuadPart / liTotalBytes.QuadPart) * 100;
                //qDebug()<<"use"<<use<<"free"<<free<<"all"<<all<<"percent"<<percent;

                double size = ((double) liTotalFreeBytes.QuadPart / GB);
                QPalette pe;
                pe.setColor(QPalette::WindowText, Qt::red);
                ui->devicesizeLabel->setPalette(pe);
                if( size < 2.0)
                    ui->devicesizeLabel->setText("当前磁盘容量小于2GB");
                else
                    ui->devicesizeLabel->setText("");

            }
#endif
}
//删除文件夹
bool frmVideoControl::DelDir(const QString &path)
{
    if (path.isEmpty()){
        return false;
    }
    QDir dir(path);
    if(!dir.exists()){
        return true;
    }
    dir.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot); //设置过滤
    QFileInfoList fileList = dir.entryInfoList(); // 获取所有的文件信息
    foreach (QFileInfo file, fileList){ //遍历文件信息
        qDebug()<<file.absolutePath();
        if (file.isFile()){ // 是文件，删除
            file.dir().remove(file.fileName());
        }else{ // 递归删除
            DelDir(file.absoluteFilePath());
        }
    }
    return dir.rmdir(dir.absolutePath()); // 删除文件夹
}
void frmVideoControl::detachFile(QString typePath)
{
    QString runPath = QCoreApplication::applicationDirPath();
    QString dirPath= runPath + typePath;// "/picture/"
    //qDebug()<<dirPath;
    QDir *dir = new QDir;
    dir->setPath(typePath);
    //qDebug()<<typePath;

    QDateTime today = QDateTime::currentDateTime();
    //循环遍历，在Qlist中存取文件名

    for(int i=0;i<dir->entryInfoList().length();i++)
    {
        QFileInfo mItem = dir->entryInfoList().at(i);

        QDateTime date = mItem.lastModified();
        uint filedate = date.toTime_t();
        uint todayData = today.toTime_t();
        //qDebug()<<"todayData:"<<todayData<<"  filedate: "<<filedate;
        uint ruler = todayData - filedate;
        ruler /= (60 * 60 * 24);
        //qDebug()<<"ruler:"<<ruler<<"  delayNumsToDelte: "<<delayNumsToDelte;
        if (ruler >= delayNumsToDelte)
        //if (ruler >= 1)
        {
            //qDebug()<<"mItem.filePath()"<<mItem.absoluteFilePath();
            if(mItem.isDir())
            {
                DelDir(mItem.absoluteFilePath());
                qDebug()<<"DelDir:"<<mItem.absoluteFilePath();
            }
            if(mItem.isFile())
            {
                QFile tmpFile(mItem.filePath());
                tmpFile.remove();
                qDebug()<<"DelFile:"<<mItem.filePath();
            }
        }
    }
}


void frmVideoControl::on_searchBtn_clicked()
{
    QString searchStr = ui->serachEdit->text();
    if(!descMap.contains(searchStr))
        return;
    QString idstr = descMap.value(searchStr);
    clientTo3399* clientto3399 = clientTo3399Map.value(idstr);
    frmwidget* frm = widgets.at(idstr.toInt()-1);

    int typeCount = ui->videoWidget->getTypeCount();
    int idindex = (idstr.toInt()-1)/typeCount;
    int index  = (idindex);
    qDebug()<<"on_searchBtn_clicked:idindex:"<<idindex;
    ui->indexCmb->setCurrentIndex(index);
    for(int i = 0 ; i < widgets.size() ; i++)
    {
        widgets.value(i)->setWidgetFcous(false);
    }
    frm->setWidgetFcous(true);
}


void frmVideoControl::on_fullBtn_clicked()
{
    ui->headWidget->setVisible(false);
    ui->statusWidget->setVisible(false);
    this->showFullScreen();
}

void frmVideoControl::keyPressEvent(QKeyEvent *evt)
{
    switch (evt->key())
    {
    case Qt::Key_Escape:  // 按下的为Esc键
        ui->headWidget->setVisible(true);
        ui->statusWidget->setVisible(true);
        this->showMaximized();
        break;
    }
}


void frmVideoControl::on_maxBtn_clicked()
{
    this->showMaximized();
}


void frmVideoControl::on_minBtn_clicked()
{
    this->showMinimized();
}




void frmVideoControl::on_closeBtn_clicked()
{
    this->close();
}


void frmVideoControl::on_normalBtn_clicked()
{
    this->showNormal();
}

void frmVideoControl::on_pushButton_clicked()
{
}

void frmVideoControl::loadStyle(const QString &qssFile)
{
    //加载样式表
    QString qss;
    QFile file(qssFile);
    if (file.open(QFile::ReadOnly)) {
        //用QTextStream读取样式文件不用区分文件编码 带bom也行
        QStringList list;
        QTextStream in(&file);
        while (!in.atEnd()) {
            QString line;
            in >> line;
            list << line;
        }

        file.close();
        qss = list.join("\n");
        QString paletteColor = qss.mid(20, 7);
        qApp->setPalette(QPalette(paletteColor));
        qApp->setStyleSheet(qss);
    }
}

void frmVideoControl::on_styleBtn_clicked()
{

    if (showStyle) {
        showStyle = false;
        ControlConfig::currentStyle = 2;
        ControlConfig::writeConfig();
        loadStyle(":/qss/lightblue.css");
    }
    else
    {
        showStyle = true;
        ControlConfig::currentStyle = 1;
        ControlConfig::writeConfig();
        loadStyle(":/qss/blacksoft.css");
    }

}


void frmVideoControl::on_videoConfigBtn_clicked()
{
    onOpenXsl();
}

