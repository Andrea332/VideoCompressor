#include "spinner.h"

#include <QPainter>
#include <QVariantAnimation>

Spinner::Spinner(int size, QWidget *parent)
    : QWidget(parent), turn_(new QVariantAnimation(this))
{
    setFixedSize(size, size);
    turn_->setStartValue(0.0);
    turn_->setEndValue(360.0);
    turn_->setDuration(900);
    turn_->setLoopCount(-1);
    connect(turn_, &QVariantAnimation::valueChanged, this, qOverload<>(&QWidget::update));
    hide();
}

void Spinner::start()
{
    if (turn_->state() != QAbstractAnimation::Running)
        turn_->start();
    show();
}

void Spinner::stop()
{
    turn_->stop();
    hide();
}

bool Spinner::isSpinning() const
{
    return turn_->state() == QAbstractAnimation::Running;
}

void Spinner::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal pen = width() / 7.0;
    const QRectF circle = QRectF(rect()).adjusted(pen / 2, pen / 2, -pen / 2, -pen / 2);
    QColor track = palette().color(QPalette::WindowText);
    track.setAlphaF(0.15f);
    p.setPen(QPen(track, pen));
    p.drawEllipse(circle);
    p.setPen(QPen(palette().color(QPalette::Highlight), pen, Qt::SolidLine, Qt::RoundCap));
    // angles in 1/16 of a degree, counterclockwise: a negative start turns it clockwise
    const int start = -int(turn_->currentValue().toDouble() * 16);
    p.drawArc(circle, start, 100 * 16);
}
