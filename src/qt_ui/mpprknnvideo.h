#ifndef MPPRKNNVIDEO_H
#define MPPRKNNVIDEO_H

#include <QByteArray>
#include <QString>
#include <QThread>
#include <QImage>
#include "mpp_decoder.h"



class MppRknnVideo : public QThread
{
     Q_OBJECT
public:
    MppRknnVideo(ShowImageCallBack _call_Back_ShowInQt=0,void* _user=0,QString rtsp_dddr="",int videotype = 264,int core_num = 0);

public:
//    void setCallBack(ShowImageCallBack _pSHowImage);
//    void ImageCallBackInner(int type,uchar* framebuff,void *pUser);

public:
//    ShowImageCallBack m_pShowImage_CallBack=0;

//    void*  m_pUser;
    uchar* m_framebuff;

};

#endif // MPPRKNNVIDEO_H
