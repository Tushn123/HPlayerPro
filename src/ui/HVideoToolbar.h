#ifndef H_VIDEO_TOOLBAR_H
#define H_VIDEO_TOOLBAR_H

#include "qtheaders.h"

class HVideoToolbar : public QFrame
{
    Q_OBJECT
public:
    explicit HVideoToolbar(QWidget *parent = nullptr);

signals:
    void sigStart();
    void sigPause();
    void sigStop();
    void sldProgressClicked(int value);

public slots:

protected:
    void initUI();
    void initConnect();
    bool eventFilter(QObject *watched, QEvent *event) override;

public:
    QPushButton *btnStart;
    QPushButton *btnPause;

    QPushButton *btnPrev;
    QPushButton *btnStop;
    QPushButton *btnNext;

    QSlider     *sldProgress;
    QLabel      *lblDuration;
};

#endif // H_VIDEO_TOOLBAR_H
