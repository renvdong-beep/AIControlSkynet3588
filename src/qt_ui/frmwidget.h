#ifndef FRMWIDGET_H
#define FRMWIDGET_H

#include <QtGui>
#if (QT_VERSION >= QT_VERSION_CHECK(5,0,0))
#include <QtWidgets>
#endif

namespace Ui {
class frmwidget;
}

class frmwidget : public QWidget
{
    Q_OBJECT

public:
    explicit frmwidget(QWidget *parent = 0,QString objectnameID = "");
    ~frmwidget();

    void setWidgetFcous(bool isTrue);
    void setWidgetViisble(bool isVisible);
    void setIdStr(QString idstr);
    void setDescStr(QString descstr);
    void setSnStr(QString snstr);
    void setIpStr(QString ipstr);
    void setRedRect(bool isTure);    
    void setStartStop(bool istrue);
    void setLeftStop(bool istrue);
    void setRightStop(bool istrue);
    void setSaveVideoTip(bool isSave);
    bool getStartStop();
    bool getLeftStop();
    bool getRightStop();
    int YUV420ToRGB24(const unsigned char* data, int width, int height,unsigned char** pOutData);

protected:
    void paintEvent(QPaintEvent *);

public slots:
    //接收图像并绘制
    void updateImage(const QImage &image);
    void updateImage(uchar*data,int wid,int hei);
    void updateErrorImage(uchar*data,int wid,int hei);
    void savePicImage(bool isNormal);
    void onDisconnectShow();
    void process_FrameToSHowInQt(int type,uchar* framebuff,int width,int height);
private:
    Ui::frmwidget *ui;
    QImage image;
    QRect mRect ;    
    bool isFocus = false;
    bool isRed = false;
    bool isSaveVideo = false;
    QString m_objectID;
    QString m_snStr="";
    QString m_descStr="";
    QString m_ipStr="";
    uchar   *m_saveBuffer=0;
    int m_picWId=0;
    int m_picHei=0;

    bool isStartStop = false;
    bool isLeftStop  = false;
    bool isRightStop = false;


};

#endif // FRMWIDGET_H
