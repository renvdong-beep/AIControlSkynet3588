#include "frmwidget.h"
#include "ui_frmwidget.h"
#include "quiwidget.h"
#include "controlconfig.h"

#define USE_LABE_SHOW_PICTURE 0

void showMFrameInQt (int type,uchar* framebuff,void *pUser,int width, int height){
    if(pUser)
    {
        frmwidget*pUserQt=(frmwidget*)pUser;
        pUserQt->process_FrameToSHowInQt(type,framebuff,width,height);
    }
}
void frmwidget::process_FrameToSHowInQt(int type,uchar* framebuff,int width,int height)
{
    //do you other process.
    //qDebug() << "process_FrameToSHowInQt:"<<framebuff;
    updateImage(framebuff,width,height);
}
frmwidget::frmwidget(QWidget *parent,QString objectnameID ) :
    QWidget(parent),
    ui(new Ui::frmwidget)
{
    ui->setupUi(this);
    m_objectID = objectnameID;

    //设置样式表
    QStringList qss;
    qss.append("QLabel{font:75 25px;color:#F0F0F0;border:0px solid #AAAAAA;background:#303030;}");
    qss.append("QWidget:focus{border:0px solid #00BB9E;background:#555555;}");
    this->setStyleSheet(qss.join(""));

    ui->label->setAlignment(Qt::AlignCenter);
    ui->label->setText(m_objectID);
    ui->label->setScaledContents(true);

    image = QImage();

#ifdef USE_LABE_SHOW_PICTURE
    //ui->widget->setVisible(true);
    setWidgetViisble(true);
#endif

    ui->stopBtn_2->hide();
    ui->closeCurentWarningBtn->hide();
    ui->pauseBtn_2->hide();


    ui->startBtn_2->hide();
    ui->resetBtn_2->hide();
    ui->leftBtn_2->hide();
    ui->rightBtn_2->hide();

    ui->btnWidget->setVisible(false);
    ui->btnWidget->hide();
}

frmwidget::~frmwidget()
{
    delete ui;
}

void frmwidget::setWidgetViisble(bool isVisible)
{
    ui->widget->setVisible(isVisible);
    ui->picWidget->setVisible(!isVisible);
    ui->btnWidget->setVisible(!isVisible);
}

void frmwidget::paintEvent(QPaintEvent *)
{
    if (image.isNull()) {
        return;
    }

    QPainter painter(this);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);

    painter.drawImage(ui->picWidget->rect(), image);

    painter.setPen(QColor(Qt::white));
    QRectF rectID(this->width()/20, this->height()/20, this->width()/5, this->height()/10);
    painter.drawText(ui->picWidget->rect(), Qt::AlignLeft|Qt::AlignTop,m_descStr);
    painter.drawText(ui->picWidget->rect(), Qt::AlignLeft|Qt::AlignBottom,m_ipStr);
    painter.drawText(ui->picWidget->rect(), Qt::AlignHCenter|Qt::AlignBottom,m_snStr);
    painter.setPen(QColor(Qt::blue));
    painter.drawText(ui->picWidget->rect(), Qt::AlignRight|Qt::AlignBottom,m_objectID);

    if(isFocus){
        painter.setPen(QPen(QColor(Qt::green), 4));
        painter.drawRect(this->rect());
    }
    if(isRed)
    {
        painter.setPen(QPen(QColor(Qt::red), 4));
        painter.drawRect(this->rect());
    }
    if(isSaveVideo){
        painter.setPen(QPen(QColor(Qt::red), 4));
        QFont font;
        font.setPixelSize(30);
        painter.setFont(font);
        painter.drawText(ui->picWidget->rect(), Qt::AlignHCenter|Qt::AlignTop,"当前通道正在录制视频...");
    }
}

void frmwidget::setRedRect(bool isTure)
{
    isRed = isTure;
}

void frmwidget::setIdStr(QString idstr)
{
    m_objectID = idstr;
}

void frmwidget::setDescStr(QString descstr)
{
    m_descStr = descstr;
}

void frmwidget::setSnStr(QString snstr)
{
    m_snStr = snstr;
}

void frmwidget::setIpStr(QString ipstr)
{
    m_ipStr = ipstr;
}

void frmwidget::setSaveVideoTip(bool isSave)
{
    isSaveVideo = isSave;
}

void frmwidget::setWidgetFcous(bool isTrue)
{
    isFocus = isTrue;
}

void frmwidget::savePicImage(bool isNormal)
{

    QString picturePath = isNormal ? ControlConfig::normalPath : ControlConfig::ngPath;
    QString dirPath= picturePath + QDateTime::currentDateTime().toString("yyyyMMdd") + "/";
    QDir *folder = new QDir;
    folder->setPath(dirPath);
    if(!folder->exists())
        folder->mkdir(dirPath);
    dirPath = dirPath + "/" + m_descStr + "/";
    folder->setPath(dirPath);
    if(!folder->exists())
        folder->mkdir(dirPath);

    QImage img(this->image.width(),20,QImage::Format_ARGB32);
    img.fill(Qt::white);
    QPainter pimg(&img);
    pimg.setPen(QColor(Qt::black));
    pimg.drawText(0,12,m_objectID);
    pimg.drawText(0.3 * img.width(),12,m_snStr);
    pimg.drawText(0.7 * img.width(),12,m_descStr);

    QImage completeImg(this->image.width(),this->image.height() + 20,QImage::Format_ARGB32);
    //QPainter p(&this->image);
    QPainter p(&completeImg);
    //绘制两幅小图到QPixmap上
    p.drawPixmap(0,0, this->image.width(), this->image.height(),QPixmap::fromImage(this->image));
    p.drawPixmap(0,this->image.height(), this->rect().width(), 20,QPixmap::fromImage(img));

    QString picPathName = dirPath + QDateTime::currentDateTime().toString("yyyy-MM-dd-HH-mm-ss") + "_"+ m_snStr + "_" + m_descStr + ".jpg";
    bool isSaveOK = completeImg.save(picPathName,"JPG",100);
    isSaveOK ? qDebug() << "================" + picPathName + " Save OK!" : qDebug() <<"================" + picPathName + " Save faile!";
}

void frmwidget::updateImage(uchar*data,int wid,int hei)
{

    QImage img = QImage(data,wid,hei,QImage::Format_RGB888);
    //QImage img = QImage(data,wid,hei,QImage::Format_Grayscale8);
    this->image = img.copy();
#if 0
    delete data;
    data=nullptr;
#endif
    if(!this->image.isNull())
        this->setWidgetViisble(false);
    update();
}

void frmwidget::updateErrorImage(uchar*data,int wid,int hei)
{

    this->image = QImage(data,wid,hei,QImage::Format_RGB888).copy();//要用.copy，IMAGE 会使用一个参数 data

    this->savePicImage(false);//保存nv12图片
#if 0
    delete data;
    data=nullptr;
#endif
    if(!this->image.isNull())
        this->setWidgetViisble(false);
    update();
}

void frmwidget::onDisconnectShow()
{
    this->setWidgetViisble(true);
    ui->label->setStyleSheet("QLabel{color:red;}");
    ui->label->setText(m_objectID + "断开");
}



void frmwidget::updateImage(const QImage &image)
{
 #if USE_LABE_SHOW_PICTURE
   ui->label->setPixmap(QPixmap::fromImage(image));
#else

     this->image = image.copy();
     this->update();
 #endif


}

void frmwidget::setStartStop(bool istrue)
{
    isStartStop = istrue;
}
void frmwidget::setLeftStop(bool istrue)
{
    isLeftStop = istrue;
}
void frmwidget::setRightStop(bool istrue)
{
    isRightStop = istrue;
}

bool frmwidget::getStartStop()
{
    return isStartStop ;
}
bool frmwidget::getLeftStop()
{
    return isLeftStop ;
}
bool frmwidget::getRightStop()
{
    return isRightStop ;
}












