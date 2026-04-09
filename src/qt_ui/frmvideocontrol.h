#ifndef FRMVIDEOPANEL_H
#define FRMVIDEOPANEL_H

#include <QWidget>
#include <QDebug>
#include <QBuffer>
#include <QMap>
#include "udpthread.h"
#include "clientto3399.h"
#include "warningwidget.h"
#include "settingpage.h"
#include <QKeyEvent>
#include "warningwidgetlist.h"
#include "workThread.h"
namespace Ui {
class frmVideoControl;
}

class frmVideoControl : public QWidget
{
    Q_OBJECT

public:
    explicit frmVideoControl(QWidget *parent = 0);
    ~frmVideoControl();
    void initToolsControl();
    void DeviceSizeWarning();
    int calcCount(QString typestr);
    void detachFile(QString typePath);
    bool DelDir(const QString &path);
    void initForm();
    void setWidgetEnable(bool isEnable);

public:
    static QByteArray Image_To_Base64(QString ImgPath) {
        QImage image(ImgPath);
        QByteArray ba;
        QBuffer buf(&ba);
        image.save(&buf,"PNG",20);
        QByteArray hexed = ba.toBase64();
        buf.close();
        return hexed;
    }
    static QPixmap Base64_To_Image(QByteArray bytearray) {
        QByteArray Ret_bytearray = QByteArray::fromBase64(bytearray);
        QBuffer buffer(&Ret_bytearray);
        buffer.open(QIODevice::WriteOnly);
        QPixmap imageresult;
        imageresult.loadFromData(Ret_bytearray);
        return imageresult;
    }
public slots:
    void onNewMachine(QString sn,QString ip);
    void onOpenXsl();
    void onDisconnectOldIndexShow(int index);
    void onDisconnectOldShow(QString typestr,int indexstr);
    void onChangeCmbIndex(QString typestr,int indexstr);    
    void onConnectNewShow(int counttypestr,int indexstr);
    void onShowErrorPic(QString snstr,uchar* ch,int width,int height);
    void onPoll();
    void initChannel();
    void initConfig();
    void saveConfig();
    void onInitShow();

protected:
    void keyPressEvent(QKeyEvent *evt);

protected:
    QPoint move_point;                                    //移动的距离
    bool mouse_press;                                    //鼠标按下
    void mousePressEvent(QMouseEvent *qevent);            //鼠标按下事件
    void mouseReleaseEvent(QMouseEvent *qevent);         //鼠标释放事件
    void mouseMoveEvent(QMouseEvent *qevent);             //鼠标移动事件

signals:
    void  sig_saveTime(QString timestr);
    void  sig_continueTime(QString timestr);

private slots:
    void on_pushButton_clicked();
    void on_SettingBtn_clicked();
    void onNextBtnPress();
    void onPreBtnPress();
    void onCloseWarningWidget();

    void on_indexCmb_currentIndexChanged(int index);
    void on_searchBtn_clicked();

    void on_fullBtn_clicked();

    void on_maxBtn_clicked();

    void on_minBtn_clicked();

    void on_closeBtn_clicked();

    void on_normalBtn_clicked();

    void on_styleBtn_clicked();

    void loadStyle(const QString &qssFile);

    void on_videoConfigBtn_clicked();

private:
    Ui::frmVideoControl *ui;
    udpThread* m_udpThread;
    WarningWidget* warningWidget;
    SettingPage* settingPage;
    QTabWidget*  listWarningWidgets;
    WarningWidgetList* warningwidgetlist;
    QMap<QString,QString> snipMenuMap;
    QList<frmwidget *> widgets;
    QMap<QString,WarningWidget *> tmpWidgetMap;
    MsgBox *warningmsgBox;

    bool showStyle = true;
    bool firstConfig = true;

    QMap<QString,QString> snMap;
    QMap<QString,QString> descipMap;
    QMap<QString,QString> ipMap;
    QMap<QString,QString> descMap;
    QMap<QString,clientTo3399 *>        clientTo3399Map;

    int deviceCountTime = 60 * 5;
    int warningwidgetCount = 0;

    bool isPoll = false;
    int pollCountTime = 3;
    int pollTime = 3;
    int delayNumsToDelte = 365;

    QStringList       warningSnList;

    workThread* workthread;
    workThread* workthread1;
    workThread* workthread2;
    workThread* workthread3;

};

#endif // FRMVIDEOPANEL_H
