#include "engine_viewport_widget.h"

#include <QFocusEvent>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEngine>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWheelEvent>

namespace Kalam::Editor
{

Engine_Viewport_Widget::Engine_Viewport_Widget(
    Editor_Runtime_Host* host,
    QWidget* parent)
    : QWidget(parent), host_(host)
{
    Q_ASSERT(host != nullptr);
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    resize_timer_.setSingleShot(true);
    resize_timer_.setInterval(35);
    connect(
        &resize_timer_,
        &QTimer::timeout,
        this,
        [this]()
        {
            host_->resize_viewport(physical_size());
        });
}

QPaintEngine* Engine_Viewport_Widget::paintEngine() const
{
    return nullptr;
}

QSize Engine_Viewport_Widget::physical_size() const
{
    const qreal scale = devicePixelRatioF();
    return QSize(
        qRound(static_cast<qreal>(width()) * scale),
        qRound(static_cast<qreal>(height()) * scale));
}

void Engine_Viewport_Widget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (!viewport_initialized_)
    {
        QString error;
        const bool debug_layer = qEnvironmentVariableIntValue(
            "KALAM_D3D11_DEBUG") != 0;
        viewport_initialized_ = host_->initialize_viewport(
            reinterpret_cast<void*>(static_cast<quintptr>(winId())),
            physical_size(),
            debug_layer,
            &error);
        if (!viewport_initialized_)
        {
            emit viewportError(error);
            return;
        }
        host_->start_frames();
    }
    host_->set_viewport_visible(true);
}

void Engine_Viewport_Widget::hideEvent(QHideEvent* event)
{
    cancel_active_gizmo();
    host_->set_viewport_visible(false);
    QWidget::hideEvent(event);
}

void Engine_Viewport_Widget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (viewport_initialized_)
    {
        resize_timer_.start();
    }
}

void Engine_Viewport_Widget::focusInEvent(QFocusEvent* event)
{
    host_->viewport_focus_gained();
    QWidget::focusInEvent(event);
}

void Engine_Viewport_Widget::focusOutEvent(QFocusEvent* event)
{
    cancel_active_gizmo();
    host_->viewport_focus_lost();
    if (mouseGrabber() == this)
    {
        releaseMouse();
    }
    has_pointer_position_ = false;
    QWidget::focusOutEvent(event);
}

void Engine_Viewport_Widget::mousePressEvent(QMouseEvent* event)
{
    setFocus(Qt::MouseFocusReason);
    const QPoint position = to_physical(event->position());
    const QPoint delta = has_pointer_position_
        ? position - last_pointer_physical_
        : QPoint();
    last_pointer_physical_ = position;
    has_pointer_position_ = true;
    host_->set_pointer_position(position);
    host_->add_pointer_delta(delta);

    const Game_Mouse_Button button = translate_mouse_button(event->button());
    if (button < Game_Mouse_Button_Count)
    {
        host_->set_mouse_button(button, true);
    }

    if (event->button() == Qt::LeftButton)
    {
        grabMouse();
        Viewport_Interaction_Result begin_result;
        QString error;
        if (host_->viewport_interaction(
                Game_Viewport_Interaction_Gizmo_Begin,
                position,
                {},
                physical_size(),
                &begin_result,
                &error) &&
            (begin_result.flags & Game_Viewport_Result_Gizmo_Active) != 0)
        {
            gizmo_dragging_ = true;
            emit viewportInteraction(begin_result);
        }
        else
        {
            forward_interaction(
                Game_Viewport_Interaction_Select,
                position,
                {});
        }
    }
    event->accept();
}

void Engine_Viewport_Widget::mouseReleaseEvent(QMouseEvent* event)
{
    const QPoint position = to_physical(event->position());
    const QPoint delta = has_pointer_position_
        ? position - last_pointer_physical_
        : QPoint();
    last_pointer_physical_ = position;
    has_pointer_position_ = true;
    host_->set_pointer_position(position);
    host_->add_pointer_delta(delta);

    const Game_Mouse_Button button = translate_mouse_button(event->button());
    if (button < Game_Mouse_Button_Count)
    {
        host_->set_mouse_button(button, false);
    }

    if (event->button() == Qt::LeftButton && gizmo_dragging_)
    {
        forward_interaction(
            Game_Viewport_Interaction_Gizmo_Commit,
            position,
            delta);
        gizmo_dragging_ = false;
    }
    release_capture_if_idle(event->buttons());
    event->accept();
}

void Engine_Viewport_Widget::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint position = to_physical(event->position());
    const QPoint delta = has_pointer_position_
        ? position - last_pointer_physical_
        : QPoint();
    last_pointer_physical_ = position;
    has_pointer_position_ = true;
    host_->set_pointer_position(position);
    host_->add_pointer_delta(delta);
    if (gizmo_dragging_)
    {
        forward_interaction(
            Game_Viewport_Interaction_Gizmo_Update,
            position,
            delta);
    }
    event->accept();
}

void Engine_Viewport_Widget::wheelEvent(QWheelEvent* event)
{
    host_->add_wheel_delta(
        static_cast<float>(event->angleDelta().y()) / 120.0f);
    event->accept();
}

void Engine_Viewport_Widget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape)
    {
        if (gizmo_dragging_)
        {
            cancel_active_gizmo();
        }
        else
        {
            emit returnToEditorRequested();
        }
        event->accept();
        return;
    }
    const Game_Key key = translate_key(*event);
    if (key != Game_Key_Unknown)
    {
        host_->set_key(key, true);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void Engine_Viewport_Widget::keyReleaseEvent(QKeyEvent* event)
{
    const Game_Key key = translate_key(*event);
    if (key != Game_Key_Unknown)
    {
        host_->set_key(key, false);
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

QPoint Engine_Viewport_Widget::to_physical(const QPointF& logical) const
{
    const qreal scale = devicePixelRatioF();
    return QPoint(
        qRound(logical.x() * scale),
        qRound(logical.y() * scale));
}

void Engine_Viewport_Widget::forward_interaction(
    Game_Viewport_Interaction_Kind kind,
    const QPoint& position,
    const QPoint& delta)
{
    Viewport_Interaction_Result result;
    QString error;
    if (!host_->viewport_interaction(
            kind,
            position,
            delta,
            physical_size(),
            &result,
            &error))
    {
        if (!error.contains(QStringLiteral("unavailable")))
        {
            emit viewportError(error);
        }
        return;
    }
    emit viewportInteraction(result);
}

void Engine_Viewport_Widget::cancel_active_gizmo()
{
    if (!gizmo_dragging_)
    {
        return;
    }
    forward_interaction(
        Game_Viewport_Interaction_Gizmo_Cancel,
        last_pointer_physical_,
        {});
    gizmo_dragging_ = false;
}

void Engine_Viewport_Widget::release_capture_if_idle(
    Qt::MouseButtons buttons)
{
    if (buttons == Qt::NoButton && mouseGrabber() == this)
    {
        releaseMouse();
    }
}

Game_Key Engine_Viewport_Widget::translate_key(const QKeyEvent& event)
{
    const int key = event.key();
    const bool keypad = event.modifiers().testFlag(Qt::KeypadModifier);
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
    {
        return static_cast<Game_Key>(Game_Key_A + key - Qt::Key_A);
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
    {
        return static_cast<Game_Key>(
            (keypad ? Game_Key_Keypad_0 : Game_Key_Digit_0) +
            key - Qt::Key_0);
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
    {
        return static_cast<Game_Key>(Game_Key_F1 + key - Qt::Key_F1);
    }

    switch (key)
    {
    case Qt::Key_Escape: return Game_Key_Escape;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return keypad ? Game_Key_Keypad_Enter : Game_Key_Enter;
    case Qt::Key_Tab: return Game_Key_Tab;
    case Qt::Key_Backspace: return Game_Key_Backspace;
    case Qt::Key_Space: return Game_Key_Space;
    case Qt::Key_Left: return Game_Key_Left;
    case Qt::Key_Right: return Game_Key_Right;
    case Qt::Key_Up: return Game_Key_Up;
    case Qt::Key_Down: return Game_Key_Down;
    case Qt::Key_Insert: return Game_Key_Insert;
    case Qt::Key_Delete: return Game_Key_Delete;
    case Qt::Key_Home: return Game_Key_Home;
    case Qt::Key_End: return Game_Key_End;
    case Qt::Key_PageUp: return Game_Key_Page_Up;
    case Qt::Key_PageDown: return Game_Key_Page_Down;
    case Qt::Key_Shift: return Game_Key_Left_Shift;
    case Qt::Key_Control: return Game_Key_Left_Control;
    case Qt::Key_Alt: return Game_Key_Left_Alt;
    case Qt::Key_Meta: return Game_Key_Left_Super;
    case Qt::Key_CapsLock: return Game_Key_Caps_Lock;
    case Qt::Key_NumLock: return Game_Key_Num_Lock;
    case Qt::Key_ScrollLock: return Game_Key_Scroll_Lock;
    case Qt::Key_Print: return Game_Key_Print_Screen;
    case Qt::Key_Pause: return Game_Key_Pause;
    case Qt::Key_Apostrophe: return Game_Key_Apostrophe;
    case Qt::Key_Comma: return Game_Key_Comma;
    case Qt::Key_Minus:
        return keypad ? Game_Key_Keypad_Subtract : Game_Key_Minus;
    case Qt::Key_Period:
        return keypad ? Game_Key_Keypad_Decimal : Game_Key_Period;
    case Qt::Key_Slash:
        return keypad ? Game_Key_Keypad_Divide : Game_Key_Slash;
    case Qt::Key_Semicolon: return Game_Key_Semicolon;
    case Qt::Key_Equal:
        return keypad ? Game_Key_Keypad_Equal : Game_Key_Equal;
    case Qt::Key_BracketLeft: return Game_Key_Left_Bracket;
    case Qt::Key_Backslash: return Game_Key_Backslash;
    case Qt::Key_BracketRight: return Game_Key_Right_Bracket;
    case Qt::Key_QuoteLeft: return Game_Key_Grave_Accent;
    case Qt::Key_Asterisk: return Game_Key_Keypad_Multiply;
    case Qt::Key_Plus: return Game_Key_Keypad_Add;
    default: return Game_Key_Unknown;
    }
}

Game_Mouse_Button Engine_Viewport_Widget::translate_mouse_button(
    Qt::MouseButton button)
{
    switch (button)
    {
    case Qt::LeftButton: return Game_Mouse_Button_Left;
    case Qt::RightButton: return Game_Mouse_Button_Right;
    case Qt::MiddleButton: return Game_Mouse_Button_Middle;
    case Qt::BackButton: return Game_Mouse_Button_Auxiliary_1;
    case Qt::ForwardButton: return Game_Mouse_Button_Auxiliary_2;
    case Qt::ExtraButton3: return Game_Mouse_Button_Auxiliary_3;
    case Qt::ExtraButton4: return Game_Mouse_Button_Auxiliary_4;
    case Qt::ExtraButton5: return Game_Mouse_Button_Auxiliary_5;
    default:
        return static_cast<Game_Mouse_Button>(Game_Mouse_Button_Count);
    }
}

} // namespace Kalam::Editor
