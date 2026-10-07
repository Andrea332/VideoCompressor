#pragma once

#include <QWidget>

class QVariantAnimation;

// A turning arc that shows that something is being worked on; hidden when stopped.
class Spinner : public QWidget
{
    Q_OBJECT

public:
    explicit Spinner(int size, QWidget *parent = nullptr);

    void start();
    void stop();
    bool isSpinning() const;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVariantAnimation *turn_;
};
