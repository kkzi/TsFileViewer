#pragma once

#include <QWidget>

// Semi-transparent overlay shown over the whole window during long
// operations (file open, page turns, parameter switches, export).
// Static: scrim + centered "Loading..." text on a soft backing panel;
// no animation.
class LoadingOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit LoadingOverlay(QWidget* host)
        : QWidget(host), host_(host)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, false);
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
        update();
    }

    void end()
    {
        hide();
    }

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
};
