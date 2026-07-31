#include "qt_value_widgets.h"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>

namespace Kalam::Editor
{

Author_Value_Widget::Author_Value_Widget(
    const Field_Descriptor& field,
    const Author_Value_Owned& initial_value,
    QWidget* parent)
    : QWidget(parent), field_(field)
{
    setObjectName(QStringLiteral("authorValueWidget"));

    if (field_.kind == Author_Field_Bool)
    {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        checkbox_ = new QCheckBox(this);
        layout->addWidget(checkbox_);
        layout->addStretch();
        connect(
            checkbox_,
            &QCheckBox::toggled,
            this,
            [this]()
            {
                publish_preview();
                publish_commit();
            });
    }
    else if (field_.kind == Author_Field_I32 ||
             field_.kind == Author_Field_Enum)
    {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        integer_spin_ = new QSpinBox(this);
        integer_spin_->setRange(
            static_cast<int>(field_.minimum[0]),
            static_cast<int>(field_.maximum[0]));
        layout->addWidget(integer_spin_);
        connect(
            integer_spin_,
            qOverload<int>(&QSpinBox::valueChanged),
            this,
            [this]() { publish_preview(); });
        connect(
            integer_spin_,
            &QSpinBox::editingFinished,
            this,
            &Author_Value_Widget::publish_commit);
    }
    else if (field_.kind == Author_Field_String ||
             field_.kind == Author_Field_Asset_Reference)
    {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        line_edit_ = new QLineEdit(this);
        layout->addWidget(line_edit_);
        connect(
            line_edit_,
            &QLineEdit::textEdited,
            this,
            [this]() { publish_preview(); });
        connect(
            line_edit_,
            &QLineEdit::editingFinished,
            this,
            &Author_Value_Widget::publish_commit);
    }
    else
    {
        build_numeric_editor();
    }
    set_value(initial_value);
}

const Field_Descriptor& Author_Value_Widget::field() const
{
    return field_;
}

Author_Value_Owned Author_Value_Widget::value() const
{
    Author_Value_Owned result;
    result.kind = field_.kind;
    if (checkbox_ != nullptr)
    {
        result.boolean = checkbox_->isChecked();
    }
    else if (integer_spin_ != nullptr)
    {
        if (field_.kind == Author_Field_Enum)
        {
            result.enum_value = integer_spin_->value();
        }
        else
        {
            result.i32 = integer_spin_->value();
        }
    }
    else if (line_edit_ != nullptr)
    {
        result.string_utf8 = line_edit_->text().toUtf8();
    }
    else if (field_.kind == Author_Field_Transform)
    {
        for (int component = 0; component < 3; ++component)
        {
            result.transform.translation[component] =
                static_cast<float>(numeric_spins_.at(component)->value());
            result.transform.rotation_degrees[component] =
                static_cast<float>(numeric_spins_.at(3 + component)->value());
            result.transform.scale[component] =
                static_cast<float>(numeric_spins_.at(6 + component)->value());
        }
    }
    else
    {
        for (qsizetype index = 0; index < numeric_spins_.size(); ++index)
        {
            result.f32[index] =
                static_cast<float>(numeric_spins_.at(index)->value());
        }
    }
    return result;
}

void Author_Value_Widget::set_value(const Author_Value_Owned& new_value)
{
    updating_ = true;
    if (checkbox_ != nullptr)
    {
        checkbox_->setChecked(new_value.boolean);
        checkbox_->setText(
            new_value.boolean
                ? QStringLiteral("true") : QStringLiteral("false"));
    }
    else if (integer_spin_ != nullptr)
    {
        integer_spin_->setValue(
            field_.kind == Author_Field_Enum
                ? static_cast<int>(new_value.enum_value)
                : new_value.i32);
    }
    else if (line_edit_ != nullptr)
    {
        line_edit_->setText(QString::fromUtf8(new_value.string_utf8));
    }
    else if (field_.kind == Author_Field_Transform)
    {
        for (int component = 0; component < 3; ++component)
        {
            numeric_spins_.at(component)->setValue(
                new_value.transform.translation[component]);
            numeric_spins_.at(3 + component)->setValue(
                new_value.transform.rotation_degrees[component]);
            numeric_spins_.at(6 + component)->setValue(
                new_value.transform.scale[component]);
        }
    }
    else
    {
        for (qsizetype index = 0; index < numeric_spins_.size(); ++index)
        {
            numeric_spins_.at(index)->setValue(new_value.f32[index]);
        }
    }
    updating_ = false;
}

void Author_Value_Widget::set_keyboard_passive(bool passive)
{
    const Qt::FocusPolicy policy = passive
        ? Qt::NoFocus
        : Qt::StrongFocus;
    setFocusPolicy(policy);
    const QVector<QWidget*> controls = editable_controls();
    for (QWidget* control : controls)
        control->setFocusPolicy(policy);
    if (color_button_ != nullptr)
        color_button_->setFocusPolicy(policy);
}

QVector<QWidget*> Author_Value_Widget::editable_controls() const
{
    QVector<QWidget*> result;
    if (!numeric_spins_.isEmpty())
    {
        result.reserve(numeric_spins_.size());
        for (QDoubleSpinBox* spin : numeric_spins_)
            result.push_back(spin);
    }
    else if (integer_spin_ != nullptr)
    {
        result.push_back(integer_spin_);
    }
    else if (line_edit_ != nullptr)
    {
        result.push_back(line_edit_);
    }
    else if (checkbox_ != nullptr)
    {
        result.push_back(checkbox_);
    }
    return result;
}

int Author_Value_Widget::editable_component_count() const
{
    return static_cast<int>(editable_controls().size());
}

int Author_Value_Widget::component_for_descendant(const QObject* object) const
{
    const QWidget* widget = qobject_cast<const QWidget*>(object);
    if (widget == nullptr)
        return -1;
    const QVector<QWidget*> controls = editable_controls();
    for (qsizetype index = 0; index < controls.size(); ++index)
    {
        QWidget* control = controls.at(index);
        if (widget == control ||
            control->isAncestorOf(const_cast<QWidget*>(widget)))
        {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void Author_Value_Widget::set_modal_selection(
    bool active,
    int component,
    bool insert_mode)
{
    clear_modal_selection();
    if (!active)
        return;
    const QVector<QWidget*> controls = editable_controls();
    if (controls.isEmpty())
        return;
    component = qBound(0, component, static_cast<int>(controls.size()) - 1);
    modal_selection_control_ = controls.at(component);
    modal_selection_control_->setProperty("kalamModalSelection", true);
    modal_selection_control_->setProperty("kalamInsertMode", insert_mode);
    setProperty("kalamCursorComponent", component);
    modal_selection_control_->style()->unpolish(modal_selection_control_);
    modal_selection_control_->style()->polish(modal_selection_control_);
    modal_selection_control_->update();
}

void Author_Value_Widget::focus_component(int component)
{
    const QVector<QWidget*> controls = editable_controls();
    if (controls.isEmpty())
        return;
    component = qBound(0, component, static_cast<int>(controls.size()) - 1);
    QWidget* control = controls.at(component);
    control->setFocus(Qt::ShortcutFocusReason);
    QLineEdit* edit = qobject_cast<QLineEdit*>(control);
    if (auto* spin = qobject_cast<QAbstractSpinBox*>(control))
        edit = spin->findChild<QLineEdit*>();
    if (edit != nullptr)
    {
        edit->setFocus(Qt::ShortcutFocusReason);
        edit->deselect();
        edit->setCursorPosition(0);
    }
}

bool Author_Value_Widget::has_modal_selection() const
{
    return modal_selection_control_ != nullptr &&
        modal_selection_control_->property("kalamModalSelection").toBool();
}

void Author_Value_Widget::clear_modal_selection()
{
    if (modal_selection_control_ != nullptr)
    {
        modal_selection_control_->setProperty("kalamModalSelection", false);
        modal_selection_control_->setProperty("kalamInsertMode", false);
        modal_selection_control_->style()->unpolish(modal_selection_control_);
        modal_selection_control_->style()->polish(modal_selection_control_);
        modal_selection_control_->update();
    }
    modal_selection_control_ = nullptr;
    setProperty("kalamCursorComponent", -1);
}

void Author_Value_Widget::focus_first_control()
{
    QWidget* target = nullptr;
    if (checkbox_ != nullptr)
    {
        target = checkbox_;
    }
    else if (integer_spin_ != nullptr)
    {
        target = integer_spin_;
    }
    else if (line_edit_ != nullptr)
    {
        target = line_edit_;
    }
    else if (!numeric_spins_.isEmpty())
    {
        target = numeric_spins_.first();
    }
    else if (color_button_ != nullptr)
    {
        target = color_button_;
    }

    if (target != nullptr)
    {
        target->setFocus(Qt::TabFocusReason);
    }
}

void Author_Value_Widget::build_numeric_editor()
{
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(3);

    const int count = spin_component_count();
    for (int row = 0; row < (field_.kind == Author_Field_Transform ? 3 : 1); ++row)
    {
        auto* row_layout = new QHBoxLayout();
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->setSpacing(4);
        if (field_.kind == Author_Field_Transform)
        {
            static const char* labels[] = {"T", "R", "S"};
            auto* label = new QLabel(QString::fromLatin1(labels[row]), this);
            label->setMinimumWidth(14);
            row_layout->addWidget(label);
        }

        const int columns = field_.kind == Author_Field_Transform ? 3 : count;
        for (int column = 0; column < columns; ++column)
        {
            auto* spin = new QDoubleSpinBox(this);
            spin->setKeyboardTracking(true);
            spin->setDecimals(3);
            int descriptor_component = column;
            double minimum = field_.minimum[descriptor_component];
            double maximum = field_.maximum[descriptor_component];
            double step = field_.step[descriptor_component];

            if (field_.kind == Author_Field_Transform)
            {
                if (row == 0)
                {
                    minimum = -10000.0;
                    maximum = 10000.0;
                    step = 0.01;
                }
                else if (row == 1)
                {
                    minimum = -360000.0;
                    maximum = 360000.0;
                    step = 0.1;
                }
                else
                {
                    minimum = 0.001;
                    maximum = 1000.0;
                    step = 0.01;
                }
            }
            if (!(maximum > minimum))
            {
                minimum = -1000000.0;
                maximum = 1000000.0;
            }
            spin->setRange(minimum, maximum);
            spin->setSingleStep(step > 0.0 ? step : 0.01);
            row_layout->addWidget(spin, 1);
            numeric_spins_.push_back(spin);
            connect(
                spin,
                qOverload<double>(&QDoubleSpinBox::valueChanged),
                this,
                [this]() { publish_preview(); });
            connect(
                spin,
                &QDoubleSpinBox::editingFinished,
                this,
                &Author_Value_Widget::publish_commit);
        }
        root_layout->addLayout(row_layout);
    }

    if (field_.kind == Author_Field_Color3 ||
        field_.kind == Author_Field_Color4)
    {
        color_button_ = new QPushButton(QStringLiteral("Choose color..."), this);
        connect(
            color_button_,
            &QPushButton::clicked,
            this,
            &Author_Value_Widget::choose_color);
        root_layout->addWidget(color_button_);
    }
}

void Author_Value_Widget::choose_color()
{
    if (color_dialog_ != nullptr)
    {
        color_dialog_->raise();
        color_dialog_->activateWindow();
        return;
    }
    const Author_Value_Owned old_value = value();
    QColor initial;
    initial.setRgbF(
        old_value.f32[0],
        old_value.f32[1],
        old_value.f32[2],
        field_.kind == Author_Field_Color4 ? old_value.f32[3] : 1.0f);
    color_dialog_original_ = old_value;
    color_dialog_ = new QColorDialog(initial, this);
    color_dialog_->setObjectName(QStringLiteral("authorColorDialog"));
    color_dialog_->setWindowTitle(QString::fromUtf8(field_.name));
    color_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    color_dialog_->setOption(QColorDialog::DontUseNativeDialog, true);
    color_dialog_->setOption(
        QColorDialog::ShowAlphaChannel,
        field_.kind == Author_Field_Color4);
    connect(
        color_dialog_,
        &QColorDialog::currentColorChanged,
        this,
        [this](const QColor& selected)
        {
            Author_Value_Owned new_value = value();
            new_value.f32[0] = static_cast<float>(selected.redF());
            new_value.f32[1] = static_cast<float>(selected.greenF());
            new_value.f32[2] = static_cast<float>(selected.blueF());
            if (field_.kind == Author_Field_Color4)
            {
                new_value.f32[3] = static_cast<float>(selected.alphaF());
            }
            set_value(new_value);
            publish_preview();
        });
    connect(
        color_dialog_,
        &QColorDialog::accepted,
        this,
        &Author_Value_Widget::publish_commit);
    connect(
        color_dialog_,
        &QColorDialog::rejected,
        this,
        [this]()
        {
            set_value(color_dialog_original_);
            publish_preview();
        });
    connect(
        color_dialog_,
        &QObject::destroyed,
        this,
        [this]() { color_dialog_ = nullptr; });
    color_dialog_->open();
}

void Author_Value_Widget::publish_preview()
{
    if (!updating_)
    {
        emit previewRequested(field_.id, value());
    }
}

void Author_Value_Widget::publish_commit()
{
    if (!updating_)
    {
        emit commitRequested(field_.id, value());
    }
}

int Author_Value_Widget::spin_component_count() const
{
    switch (field_.kind)
    {
    case Author_Field_Vec2: return 2;
    case Author_Field_Vec3:
    case Author_Field_Color3:
    case Author_Field_Euler_Degrees3:
        return 3;
    case Author_Field_Vec4:
    case Author_Field_Color4:
        return 4;
    case Author_Field_Transform:
        return 9;
    default:
        return 1;
    }
}

} // namespace Kalam::Editor
