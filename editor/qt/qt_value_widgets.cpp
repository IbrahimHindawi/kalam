#include "qt_value_widgets.h"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>

namespace Kalam::Editor
{

namespace
{

struct Json_Cursor_Token
{
    qsizetype start = 0;
    qsizetype length = 0;
};

bool is_json_space(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

bool is_number_byte(char value)
{
    return (value >= '0' && value <= '9') ||
        value == '-' || value == '+' || value == '.' ||
        value == 'e' || value == 'E';
}

QVector<Json_Cursor_Token> json_cursor_tokens(const QByteArray& json)
{
    QVector<Json_Cursor_Token> result;
    qsizetype index = 0;
    while (index < json.size())
    {
        const char value = json.at(index);
        if (value == '"')
        {
            const qsizetype start = index++;
            while (index < json.size())
            {
                if (json.at(index) == '\\' && index + 1 < json.size())
                {
                    index += 2;
                    continue;
                }
                if (json.at(index++) == '"')
                    break;
            }
            qsizetype after = index;
            while (after < json.size() && is_json_space(json.at(after)))
                ++after;
            if (after >= json.size() || json.at(after) != ':')
                result.push_back({start, index - start});
            continue;
        }
        if ((value >= '0' && value <= '9') || value == '-')
        {
            const qsizetype start = index++;
            while (index < json.size() && is_number_byte(json.at(index)))
                ++index;
            result.push_back({start, index - start});
            continue;
        }
        qsizetype literal_length = 0;
        if (json.mid(index, 4) == QByteArrayLiteral("true") ||
            json.mid(index, 4) == QByteArrayLiteral("null"))
        {
            literal_length = 4;
        }
        else if (json.mid(index, 5) == QByteArrayLiteral("false"))
        {
            literal_length = 5;
        }
        if (literal_length > 0)
        {
            result.push_back({index, literal_length});
            index += literal_length;
            continue;
        }
        ++index;
    }
    return result;
}

QString cursor_fragment_html(const QString& fragment)
{
    QString result = fragment.toHtmlEscaped();
    result.replace(QLatin1Char(' '), QStringLiteral("&nbsp;"));
    result.replace(QLatin1Char('\t'), QStringLiteral("&nbsp;&nbsp;&nbsp;&nbsp;"));
    return result;
}

} // namespace

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
    const QList<QWidget*> controls = findChildren<QWidget*>();
    for (QWidget* control : controls)
    {
        control->setFocusPolicy(policy);
    }
}

void Author_Value_Widget::set_projected_cursor(
    bool active,
    const QByteArray& value_json,
    qint64 byte_column,
    const QByteArray& mode)
{
    clear_projected_cursor();
    if (!active)
        return;

    QVector<Json_Cursor_Token> tokens = json_cursor_tokens(value_json);
    if (tokens.isEmpty())
        tokens.push_back({0, value_json.size()});

    qsizetype token_index = 0;
    qint64 best_distance = -1;
    for (qsizetype index = 0; index < tokens.size(); ++index)
    {
        const Json_Cursor_Token& candidate = tokens.at(index);
        const qint64 start = candidate.start;
        const qint64 end = candidate.start + candidate.length;
        qint64 distance = 0;
        if (byte_column < start)
            distance = start - byte_column;
        else if (byte_column > end)
            distance = byte_column - end;
        if (best_distance < 0 || distance < best_distance)
        {
            token_index = index;
            best_distance = distance;
        }
    }

    QWidget* control = nullptr;
    int component = 0;
    if (!numeric_spins_.isEmpty())
    {
        component = static_cast<int>(qBound<qsizetype>(
            0, token_index, numeric_spins_.size() - 1));
        control = numeric_spins_.at(component);
    }
    else if (integer_spin_ != nullptr)
    {
        control = integer_spin_;
    }
    else if (line_edit_ != nullptr)
    {
        control = line_edit_;
    }
    else if (checkbox_ != nullptr)
    {
        control = checkbox_;
    }
    if (control == nullptr)
        return;

    const Json_Cursor_Token& token = tokens.at(token_index);
    const QByteArray token_utf8 = value_json.mid(token.start, token.length);
    const qint64 relative_byte_64 = qBound<qint64>(
        0,
        byte_column - token.start,
        static_cast<qint64>(token_utf8.size()));
    const qsizetype relative_byte = static_cast<qsizetype>(relative_byte_64);
    const QString token_text = QString::fromUtf8(token_utf8);
    const qsizetype character_index = QString::fromUtf8(
        token_utf8.constData(),
        relative_byte).size();
    const QString before = cursor_fragment_html(
        token_text.left(character_index));
    const QString cursor = cursor_fragment_html(
        character_index < token_text.size()
            ? token_text.mid(character_index, 1)
            : QStringLiteral(" "));
    const QString after = cursor_fragment_html(
        token_text.mid(character_index +
            (character_index < token_text.size() ? 1 : 0)));

    QWidget* target = control;
    if (auto* spin = qobject_cast<QAbstractSpinBox*>(control))
    {
        if (QLineEdit* spin_line_edit = spin->findChild<QLineEdit*>())
            target = spin_line_edit;
    }
    if (projected_cursor_ == nullptr)
    {
        projected_cursor_ = new QLabel(target);
        projected_cursor_->setObjectName(
            QStringLiteral("kalamProjectedCursor"));
        projected_cursor_->setTextFormat(Qt::RichText);
        projected_cursor_->setTextInteractionFlags(Qt::NoTextInteraction);
        projected_cursor_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        projected_cursor_->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    else
    {
        projected_cursor_->setParent(target);
    }

    projected_cursor_control_ = control;
    projected_cursor_target_ = target;
    projected_cursor_target_->installEventFilter(this);
    projected_cursor_control_->setProperty("kalamProjectedCursor", true);
    projected_cursor_->setProperty(
        "kalamInsertCursor",
        mode.startsWith('i'));
    setProperty("kalamCursorComponent", component);
    setProperty("kalamCursorToken", token_utf8);

    projected_cursor_->setFont(target->font());
    projected_cursor_->setPalette(target->palette());
    const QString background =
        target->palette().color(QPalette::Highlight).name();
    const QString foreground =
        target->palette().color(QPalette::HighlightedText).name();
    projected_cursor_->setText(QStringLiteral(
        "<pre style='margin:0'>%1<span style='background:%2;color:%3'>"
        "%4</span>%5</pre>")
        .arg(before, background, foreground, cursor, after));

    projected_cursor_control_->style()->unpolish(projected_cursor_control_);
    projected_cursor_control_->style()->polish(projected_cursor_control_);
    projected_cursor_control_->update();
    position_projected_cursor();
    projected_cursor_->show();
    projected_cursor_->raise();
}

bool Author_Value_Widget::has_projected_cursor() const
{
    return projected_cursor_ != nullptr && !projected_cursor_->isHidden();
}

bool Author_Value_Widget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == projected_cursor_target_ &&
        (event->type() == QEvent::Resize ||
         event->type() == QEvent::Show ||
         event->type() == QEvent::LayoutRequest ||
         event->type() == QEvent::StyleChange))
    {
        position_projected_cursor();
    }
    return QWidget::eventFilter(watched, event);
}

void Author_Value_Widget::clear_projected_cursor()
{
    if (projected_cursor_ != nullptr)
        projected_cursor_->hide();
    if (projected_cursor_target_ != nullptr)
        projected_cursor_target_->removeEventFilter(this);
    if (projected_cursor_control_ != nullptr)
    {
        projected_cursor_control_->setProperty("kalamProjectedCursor", false);
        projected_cursor_control_->style()->unpolish(projected_cursor_control_);
        projected_cursor_control_->style()->polish(projected_cursor_control_);
        projected_cursor_control_->update();
    }
    projected_cursor_control_ = nullptr;
    projected_cursor_target_ = nullptr;
    setProperty("kalamCursorComponent", -1);
    setProperty("kalamCursorToken", QByteArray());
}

void Author_Value_Widget::position_projected_cursor()
{
    if (projected_cursor_ == nullptr || projected_cursor_target_ == nullptr)
        return;
    QRect area = projected_cursor_target_->rect();
    if (projected_cursor_control_ == checkbox_)
    {
        const int left = qMin(20, qMax(0, area.width() - 1));
        area.setLeft(left);
    }
    else
    {
        area.adjust(2, 1, -2, -1);
    }
    projected_cursor_->setGeometry(area);
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
