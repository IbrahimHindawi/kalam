#ifndef KALAM_EDITOR_QT_ENGINE_VIEWPORT_WIDGET_H
#define KALAM_EDITOR_QT_ENGINE_VIEWPORT_WIDGET_H

#include "editor_runtime_host.h"

#include <QPoint>
#include <QTimer>
#include <QWidget>

namespace Kalam::Editor
{

class Engine_Viewport_Widget final : public QWidget
{
    Q_OBJECT

public:
    explicit Engine_Viewport_Widget(
        Editor_Runtime_Host* host,
        QWidget* parent = nullptr);

    QPaintEngine* paintEngine() const override;
    QSize physical_size() const;

public slots:
    void cancel_active_gizmo();

signals:
    void viewportInteraction(Kalam::Editor::Viewport_Interaction_Result result);
    void viewportError(QString error);
    void returnToEditorRequested();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;

private:
    Editor_Runtime_Host* host_ = nullptr;
    QTimer resize_timer_;
    QPoint last_pointer_physical_;
    bool has_pointer_position_ = false;
    bool gizmo_dragging_ = false;
    bool viewport_initialized_ = false;

    QPoint to_physical(const QPointF& logical) const;
    void forward_interaction(
        Game_Viewport_Interaction_Kind kind,
        const QPoint& position,
        const QPoint& delta);
    void release_capture_if_idle(Qt::MouseButtons buttons);

    static Game_Key translate_key(const QKeyEvent& event);
    static Game_Mouse_Button translate_mouse_button(Qt::MouseButton button);
};

} // namespace Kalam::Editor

#endif
