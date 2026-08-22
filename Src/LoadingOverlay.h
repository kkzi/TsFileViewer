#pragma once

#include <QVariantAnimation>
#include <QWidget>

// Semi-transparent overlay shown over the whole window during long
// operations (file open, page turns, parameter switches). Clicks pass
// through to nothing (modal feel) while visible.
class LoadingOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit LoadingOverlay(QWidget* host)
        : QWidget(host), host_(host)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, false);
        spin_.setStartValue(0);
        spin_.setEndValue(360);
        spin_.setDuration(900);
        spin_.setLoopCount(-1);
        connect(&spin_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v)
        {
            angle_ = v.toInt();
            update();
        });
        hide();
    }

    void begin()
    {
        if (host_ == nullptr)
        {
            return;
        }
        setGeometry(host_->rect());
        raise();
        show();
        spin_.start();
    }

    void end()
    {
        spin_.stop();
        hide();
    }

    bool active() const { return isVisible(); }

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override
    {
        // Keep covering the host as the window resizes while visible.
        if (host_ != nullptr)
        {
            setGeometry(host_->rect());
        }
    }

private:
    QWidget* host_;
    QVariantAnimation spin_;
    int angle_ = 0;
};
