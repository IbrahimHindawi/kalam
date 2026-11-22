#include "json_views.hpp"
#include "embedded.h"
#include "neovim_grid_widget.h"
#include "neovim_rpc_client.h"

#include <QCheckBox>
#include <QComboBox>
#include <QApplication>
#include <QSignalBlocker>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QRegularExpression>
#include <QSet>
#include <QShortcut>
#include <QSlider>
#include <QStyle>
#include <QScrollBar>
#include <QTreeWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

using namespace Kalam::Editor;

namespace {
constexpr double exposureMin = 0.01;
constexpr double exposureMax = 32.;

class FormTree final : public QTreeWidget {
public:
    FormTree(QWidget *parent, std::function<void()> command)
        : QTreeWidget(parent), command_(std::move(command)) {}

protected:
    void keyPressEvent(QKeyEvent *event) override {
        if (event->text() == ":") {
            command_();
            event->accept();
            return;
        }
        QTreeWidget::keyPressEvent(event);
    }

private:
    std::function<void()> command_;
};

struct Node {
    QString name;
    QJsonValue value;
    qsizetype key, start, end;
    std::vector<Node> children;
};

std::optional<QJsonValue> parseValue(const QByteArray &bytes) {
    const auto doc = QJsonDocument::fromJson("[" + bytes + "]");
    if (!doc.isArray() || doc.array().size() != 1)
        return std::nullopt;
    return doc.array().at(0);
}

QByteArray encode(const QJsonValue &value) {
    const auto array = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return array.mid(1, array.size() - 2);
}

// QJsonDocument validates the syntax first. This pass records UTF-8 spans in
// the original buffer, including key positions, so the commit can stay local.
class Spans {
public:
    explicit Spans(const QByteArray &text) : text_(text) {}
    std::optional<Node> parse() {
        const auto value = parseValue(text_);
        if (!value)
            return std::nullopt;
        auto root = read(QStringLiteral("$"), *value, 0, 0);
        space();
        return pos_ == text_.size() ? root : std::nullopt;
    }
private:
    const QByteArray &text_;
    qsizetype pos_ = 0;
    void space() {
        while (pos_ < text_.size() && QByteArrayView(" \t\n\r").contains(text_[pos_]))
            ++pos_;
    }
    void string() {
        ++pos_;
        while (text_[pos_] != '"') {
            if (text_[pos_] == '\\')
                ++pos_;
            ++pos_;
        }
        ++pos_;
    }
    std::optional<Node> read(QString name, QJsonValue value, qsizetype key, int depth) {
        if (depth > 64)
            return std::nullopt;
        space();
        Node node{std::move(name), value, key, pos_, 0, {}};
        const auto token = text_[pos_];
        if (token == '{' || token == '[') {
            const char tail = token == '{' ? '}' : ']';
            const auto object = value.toObject();
            const auto array = value.toArray();
            QSet<QString> keys;
            ++pos_;
            space();
            while (text_[pos_] != tail) {
                const auto childKey = pos_;
                QString label;
                QJsonValue child;
                if (token == '{') {
                    if (text_[pos_] != '"')
                        return std::nullopt;
                    string();
                    const auto parsed = parseValue(text_.mid(childKey, pos_ - childKey));
                    if (!parsed || !parsed->isString() || keys.contains(parsed->toString()))
                        return std::nullopt;
                    label = parsed->toString();
                    keys.insert(label);
                    child = object.value(label);
                    space();
                    if (text_[pos_++] != ':')
                        return std::nullopt;
                } else {
                    const auto index = qsizetype(node.children.size());
                    label = QString("[%1]").arg(index);
                    child = array.at(index);
                }
                auto next = read(label, child, childKey, depth + 1);
                if (!next)
                    return std::nullopt;
                node.children.push_back(std::move(*next));
                space();
                if (text_[pos_] == ',') {
                    ++pos_;
                    space();
                }
            }
            ++pos_;
        } else if (token == '"') {
            string();
        } else {
            while (pos_ < text_.size() && !QByteArrayView(" \t\r\n,]}").contains(text_[pos_]))
                ++pos_;
        }
        node.end = pos_;
        return node;
    }
};

enum class Kind { Json, String, Number, Boolean, Vector, Matrix, Color };

bool numbers(const QJsonArray &array) {
    return std::all_of(array.begin(), array.end(), [](const auto &v) { return v.isDouble(); });
}

Kind kind(const Node &node) {
    if (node.value.isString()) return Kind::String;
    if (node.value.isBool()) return Kind::Boolean;
    if (node.value.isDouble()) return Kind::Number;
    if (!node.value.isArray()) return Kind::Json;
    const auto array = node.value.toArray();
    const auto name = node.name.toLower();
    if (array.size() >= 2 && array.size() <= 4 &&
        std::all_of(array.begin(), array.end(), [](const auto &v) { return v.isArray(); })) {
        const auto width = array[0].toArray().size();
        if (width >= 2 && width <= 4 && std::all_of(array.begin(), array.end(), [width](const auto &v) {
            return v.toArray().size() == width && numbers(v.toArray());
        }))
            return Kind::Matrix;
    }
    if (!numbers(array)) return Kind::Json;
    if (name.contains("matrix") && (array.size() == 9 || array.size() == 16))
        return Kind::Matrix;
    if ((name == "color" || name.endsWith("_color") || name.endsWith("_colour")) &&
        (array.size() == 3 || array.size() == 4) &&
        std::all_of(array.begin(), array.end(), [](const auto &v) {
            return v.toDouble() >= 0 && v.toDouble() <= 1;
        }))
        return Kind::Color;
    return array.size() >= 2 && array.size() <= 4 ? Kind::Vector : Kind::Json;
}

const Node *at(const Node &node, qsizetype offset) {
    if (offset < node.key || offset >= node.end) return nullptr;
    const auto type = kind(node);
    if (type == Kind::Color || type == Kind::Vector || type == Kind::Matrix)
        return &node;
    for (const auto &child : node.children)
        if (const auto *match = at(child, offset))
            return match;
    return &node;
}

const Node *boundNode(const Node &root, const QString &binding) {
    if (binding.isEmpty()) return &root;
    if (!binding.startsWith('/')) return nullptr;
    const Node *node = &root;
    for (auto part : binding.mid(1).split('/')) {
        part.replace("~1", "/").replace("~0", "~");
        if (node->value.isArray()) part = QString("[%1]").arg(part);
        const auto match = std::find_if(node->children.begin(), node->children.end(),
            [&part](const Node &child) { return child.name == part; });
        if (match == node->children.end()) return nullptr;
        node = &*match;
    }
    return node;
}

const Node *atStart(const Node &node, qsizetype offset) {
    if (node.start == offset) return &node;
    for (const auto &child : node.children)
        if (offset >= child.start && offset < child.end)
            return atStart(child, offset);
    return nullptr;
}

using Apply = std::function<void(const Node &, QJsonValue, QDialog *, QLabel *)>;

void inlineError(QWidget *box, const QString &message) {
    box->setToolTip(message);
    box->setAccessibleDescription(message);
    for (auto *line : box->findChildren<QLineEdit *>()) {
        line->setProperty("invalid", !message.isEmpty());
        line->style()->unpolish(line);
        line->style()->polish(line);
    }
}

using ValueAction = std::function<void(const QJsonValue &)>;

void populateInline(QWidget *box, const Node &node, const QJsonObject &control,
                    const ValueAction &apply, const ValueAction &preview,
                    const std::function<void()> &cancel, bool exposure) {
    const auto type = kind(node);
    auto *layout = new QGridLayout(box);
    layout->setContentsMargins(2, 1, 2, 1);
    layout->setSpacing(3);
    const auto choices = control.value("choices").toArray();
    if (!choices.isEmpty()) {
        auto *combo = new QComboBox;
        const auto original = encode(node.value);
        for (const auto &choice : choices)
            if (choice.type() == node.value.type())
                combo->addItem(choice.isString() ? choice.toString() : QString::fromUtf8(encode(choice)),
                               encode(choice));
        if (combo->findData(original) < 0) combo->insertItem(0, QString::fromUtf8(original), original);
        combo->setCurrentIndex(combo->findData(original));
        combo->setAccessibleName(node.name);
        layout->addWidget(combo, 0, 0);
        QObject::connect(combo, &QComboBox::activated, box, [combo, apply] {
            if (const auto value = parseValue(combo->currentData().toByteArray())) apply(*value);
        });
        return;
    }
    const bool compound = type == Kind::Color || type == Kind::Vector || type == Kind::Matrix;
    const auto array = node.value.toArray();
    const bool nested = compound && !array.isEmpty() && array[0].isArray();
    const int columns = nested ? int(array[0].toArray().size()) :
        type == Kind::Matrix ? (array.size() == 9 ? 3 : 4) : compound ? int(array.size()) : 1;
    QJsonArray values;
    if (nested) {
        for (const auto &row : array)
            for (const auto &value : row.toArray()) values.append(value);
    } else if (compound) {
        values = array;
    } else {
        values.append(node.value);
    }
    std::vector<QLineEdit *> entries;
    QStringList initial;
    for (int i = 0; i < values.size(); ++i) {
        const auto value = values[i];
        const auto text = type == Kind::String ? value.toString() : QString::fromUtf8(encode(value));
        auto *line = new QLineEdit(text);
        line->setMinimumWidth(compound ? 32 : 48);
        const auto label = QString(type == Kind::Color ? "RGBA" : "XYZW").mid(i, 1);
        line->setAccessibleName(type == Kind::Matrix
            ? QString("%1, row %2 column %3").arg(node.name).arg(i / columns + 1).arg(i % columns + 1)
            : compound ? node.name + " " + label : node.name);
        const bool labeled = type == Kind::Color || type == Kind::Vector;
        if (labeled) {
            auto *axis = new QLabel(label);
            axis->setForegroundRole(QPalette::PlaceholderText);
            layout->addWidget(axis, 0, i);
        }
        layout->addWidget(line, i / columns + int(labeled), i % columns);
        entries.push_back(line);
        initial.push_back(text);
    }
    const auto read = [entries, type, compound, nested, columns]() -> std::optional<QJsonValue> {
        if (type == Kind::String) return QJsonValue(entries[0]->text());
        QJsonArray values, row;
        for (auto *line : entries) {
            const auto value = parseValue(line->text().toUtf8());
            if (!value || ((compound || type == Kind::Number) && !value->isDouble())) return {};
            if (!compound) return value;
            if (!nested) values.append(*value);
            else {
                row.append(*value);
                if (row.size() == columns) { values.append(row); row = {}; }
            }
        }
        return QJsonValue(values);
    };
    const auto submit = [box, read, apply] {
        if (const auto value = read()) apply(*value);
        else inlineError(box, "Enter a valid value.");
    };
    for (auto *entry : entries) {
        QObject::connect(entry, &QLineEdit::returnPressed, box, submit);
        QObject::connect(entry, &QLineEdit::textEdited, box, [read, preview] {
            if (preview)
                if (const auto value = read()) preview(*value);
        });
        auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), entry);
        escape->setContext(Qt::WidgetShortcut);
        QObject::connect(escape, &QShortcut::activated, box, [box, entries, initial, cancel] {
            if (cancel) cancel();
            for (int i = 0; i < initial.size(); ++i) entries[i]->setText(initial[i]);
            inlineError(box, {});
        });
    }
    const auto min = control.value("min").toDouble(exposureMin);
    const auto max = control.value("max").toDouble(exposureMax);
    const auto step = control.value("step").toDouble(0.01);
    const bool bounded = control.value("min").isDouble() && control.value("max").isDouble() &&
        std::isfinite(max - min) && max > min && step > 0;
    if (type == Kind::Number && (exposure || bounded)) {
        auto *line = entries.front();
        auto *slider = new QSlider(Qt::Horizontal);
        const double lower = bounded ? min : exposureMin;
        const double upper = bounded ? max : exposureMax;
        const int ticks = bounded ? int(std::clamp(std::ceil((max - min) / step), 1., 10000.)) : 3199;
        slider->setRange(0, ticks);
        const auto position = [=](double value) {
            return qRound((std::clamp(value, lower, upper) - lower) * ticks / (upper - lower));
        };
        slider->setValue(position(node.value.toDouble()));
        layout->addWidget(slider, 0, 1);
        layout->setColumnStretch(1, 2);
        QObject::connect(slider, &QSlider::valueChanged, line, [=](int thumb) {
            const double value = lower + (upper - lower) * thumb / ticks;
            line->setText(QString::number(value, 'g', 12));
            if (preview) preview(QJsonValue(value));
            if (!slider->isSliderDown()) submit();
        });
        QObject::connect(slider, &QSlider::sliderReleased, box, submit);
        QObject::connect(line, &QLineEdit::textChanged, box, [slider, read, position] {
            if (const auto value = read()) {
                const QSignalBlocker block(slider);
                slider->setValue(position(value->toDouble()));
            }
        });
    }
}

QDialog *editor(const Node &node, QWidget *parent, Apply apply, Apply preview, const QJsonObject &control) {
    const auto type = kind(node);
    if (type == Kind::Color) {
        const auto a = node.value.toArray();
        const bool linear = node.name == "base_color";
        const auto display = [linear](double v) {
            if (!linear) return v;
            return v <= 0.0031308 ? 12.92*v : 1.055*std::pow(v, 1./2.4)-0.055;
        };
        const double alpha = a.size() == 4 ? a[3].toDouble() : 1;
        auto *dialog = new QColorDialog(QColor::fromRgbF(display(a[0].toDouble()),
            display(a[1].toDouble()), display(a[2].toDouble()), alpha), parent);
        dialog->setOption(QColorDialog::DontUseNativeDialog);
        dialog->setOption(QColorDialog::ShowAlphaChannel, a.size() == 4);
        QObject::connect(dialog, &QColorDialog::colorSelected, dialog, [node, a, linear, apply](QColor color) {
            const auto decode = [linear](double v) {
                if (!linear) return v;
                return v <= 0.04045 ? v/12.92 : std::pow((v+0.055)/1.055, 2.4);
            };
            QJsonArray values{decode(color.redF()), decode(color.greenF()), decode(color.blueF())};
            if (a.size() == 4) values.append(color.alphaF());
            apply(node, values, nullptr, nullptr);
        });
        return dialog;
    }

    auto *dialog = new QDialog(parent);
    dialog->setObjectName("kalam-field-editor");
    dialog->setWindowTitle(node.name == "$" ? "JSON document" : node.name);
    auto *layout = new QVBoxLayout(dialog);
    auto *error = new QLabel;
    std::function<std::optional<QJsonValue>()> read;

    const auto choices = control.value("choices").toArray();
    if (!choices.isEmpty() && (type == Kind::String || type == Kind::Number || type == Kind::Boolean)) {
        auto *combo = new QComboBox;
        for (const auto &choice : choices) {
            if (choice.type() != node.value.type()) continue;
            combo->addItem(choice.isString() ? choice.toString() : QString::fromUtf8(encode(choice)),
                           encode(choice));
        }
        const auto original = encode(node.value);
        if (combo->findData(original) < 0)
            combo->insertItem(0, QString::fromUtf8(original), original);
        combo->setCurrentIndex(combo->findData(original));
        layout->addWidget(combo);
        read = [combo] { return parseValue(combo->currentData().toByteArray()); };
    } else if (type == Kind::Boolean) {
        auto *check = new QCheckBox("Enabled");
        check->setChecked(node.value.toBool());
        layout->addWidget(check);
        read = [check] { return QJsonValue(check->isChecked()); };
    } else if (type == Kind::String || type == Kind::Number) {
        auto *line = new QLineEdit(type == Kind::String ? node.value.toString() : QString::fromUtf8(encode(node.value)));
        layout->addWidget(line);
        line->selectAll();
        read = [line, type]() -> std::optional<QJsonValue> {
            if (type == Kind::String) return QJsonValue(line->text());
            const auto value = parseValue(line->text().toUtf8());
            return value && value->isDouble() ? value : std::nullopt;
        };
        const double declaredMin = control.value("min").toDouble();
        const double declaredMax = control.value("max").toDouble();
        const double step = control.value("step").toDouble(0.01);
        const bool bounded = control.value("min").isDouble() && control.value("max").isDouble() &&
            std::isfinite(declaredMax - declaredMin) && declaredMax > declaredMin && step > 0;
        if (type == Kind::Number && (preview || bounded)) {
            const double min = bounded ? declaredMin : exposureMin;
            const double max = bounded ? declaredMax : exposureMax;
            auto *slider = new QSlider(Qt::Horizontal);
            slider->setObjectName("kalam-exposure-slider");
            const int ticks = bounded ? int(std::clamp(std::ceil((max - min) / step), 1., 10000.)) : 3199;
            const int firstTick = bounded ? 0 : 1;
            const auto position = [=](double number) {
                return firstTick + qRound((std::clamp(number, min, max) - min) / (max - min) * ticks);
            };
            slider->setRange(firstTick, firstTick + ticks);
            slider->setValue(position(node.value.toDouble()));
            layout->insertWidget(0, slider);
            QObject::connect(slider, &QSlider::valueChanged, line, [=](int thumb) {
                const auto value = min + (double(thumb - firstTick) / ticks) * (max - min);
                line->setText(QString::number(value, 'g', 12));
            });
            QObject::connect(line, &QLineEdit::textChanged, dialog, [=] {
                if (const auto value = read()) {
                    const QSignalBlocker block(slider);
                    slider->setValue(position(value->toDouble()));
                    if (preview) preview(node, *value, dialog, error);
                } else {
                    error->setText("Enter a valid number.");
                }
            });
        }
    } else if (type == Kind::Vector || type == Kind::Matrix) {
        const auto data = node.value.toArray();
        const bool nested = data[0].isArray();
        const int columns = nested ? int(data[0].toArray().size()) : type == Kind::Vector ? int(data.size())
            : data.size() == 9 ? 3 : 4;
        QJsonArray flat;
        if (nested) {
            for (const auto &row : data)
                for (const auto &v : row.toArray()) flat.append(v);
        } else {
            flat = data;
        }
        auto *grid = new QGridLayout;
        std::vector<QLineEdit *> entries;
        if (type == Kind::Vector) {
            for (int i = 0; i < columns; ++i)
                grid->addWidget(new QLabel(QString("XYZW").mid(i, 1)), 0, i);
        }
        for (int i = 0; i < flat.size(); ++i) {
            auto *line = new QLineEdit(QString::fromUtf8(encode(flat[i])));
            line->setAccessibleName(type == Kind::Vector ? QString(" %1").arg(QString("XYZW")[i]).trimmed()
                : QString("Row %1, column %2").arg(i/columns+1).arg(i%columns+1));
            grid->addWidget(line, i/columns + (type == Kind::Vector), i%columns);
            entries.push_back(line);
        }
        layout->addLayout(grid);
        read = [entries, columns, nested]() -> std::optional<QJsonValue> {
            QJsonArray out, row;
            for (auto *line : entries) {
                const auto v = parseValue(line->text().toUtf8());
                if (!v || !v->isDouble()) return std::nullopt;
                if (nested) {
                    row.append(*v);
                    if (row.size() == columns) { out.append(row); row = {}; }
                } else out.append(*v);
            }
            return QJsonValue(out);
        };
    } else {
        auto *text = new QPlainTextEdit(QString::fromUtf8(encode(node.value)));
        layout->addWidget(text);
        read = [text] { return parseValue(text->toPlainText().toUtf8()); };
    }

    error->setWordWrap(true);
    layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [dialog, read, error, node, apply] {
        if (const auto value = read()) apply(node, *value, dialog, error);
        else error->setText("Enter a valid value.");
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return dialog;
}

JsonViews::Document readDocument(const QVariantList &args) {
    return {args[0].toLongLong(), args[1].toLongLong(), args[2].toLongLong(),
            args[3].toString(), args[4].toString().toUtf8(), {}};
}
}

JsonViews::JsonViews(Neovim_Grid_Widget *grid, Neovim_Rpc_Client *rpc, Validate validate, Preview preview, QString scenePath,
                     bool useNeovimPaneKeys)
    : grid_(grid), rpc_(rpc), validate_(std::move(validate)), preview_(std::move(preview)), scenePath_(std::move(scenePath)),
      useNeovimPaneKeys_(useNeovimPaneKeys) {}

JsonViews::~JsonViews() { qDeleteAll(panes_); }

void JsonViews::rememberForm(qint64 window, QWidget *pane) {
    auto *tree = qobject_cast<QTreeWidget *>(pane);
    if (!tree) return;
    QStringList selection;
    for (auto *item = tree->currentItem(); item; item = item->parent())
        selection.prepend(item->text(0));
    formViews_.insert(window, {documents_.value(window).buffer, selection, tree->verticalScrollBar()->value()});
}

bool JsonViews::notify(const QByteArray &name, const QVariantList &args) {
    if (name == "kalam.cmdline" && args.size() == 1) {
        cmdline_ = args[0].toBool();
        layout();
    } else if (name == "kalam.field" && args.size() == 6) {
        field(readDocument(args), args[5].toLongLong());
    } else if (name == "kalam.tui_edit" && args.size() == 8) {
        const auto doc = readDocument(args);
        const auto root = Spans(doc.text).parse();
        const auto binding = args[5].toString();
        const auto *node = root ? boundNode(*root, binding) : nullptr;
        const auto value = parseValue(args[6].toString().toUtf8());
        const bool apply = args[7].toBool();
        if (!node || !value) {
            if (apply) tuiApplied(doc.window, "Field is missing or the edited value is invalid JSON.");
        } else if (binding == "/exposure" && QFileInfo(doc.path) == QFileInfo(scenePath_) && value->isDouble()) {
            editExposure(doc, node->start, node->end, value->toDouble(), apply, nullptr, nullptr, true);
        } else if (apply) {
            commit(doc, node->start, node->end, encode(*value), nullptr, nullptr, {},
                   [this, window=doc.window] { cancelPreview(window); tuiApplied(window); },
                   [this, window=doc.window](const QString &message) {
                       cancelPreview(window);
                       tuiApplied(window, message);
                   });
        } else {
            previewValue(doc, node->start, node->end, encode(*value));
        }
    } else if (name == "kalam.cancel_preview" && args.size() == 1) {
        cancelPreview(args[0].toLongLong());
    } else if (name == "kalam.buffer_revision" && args.size() == 3) {
        sourceBuffer_ = args[0].toLongLong();
        sourceRevision_ = args[1].toLongLong();
        if (appliedBuffer_ == sourceBuffer_ && args[2].toDouble() > 0) {
            appliedExposure_ = args[2].toFloat();
            if (!draftWindow_) kalam_embed_exposure(appliedExposure_);
        }
        if (draftBuffer_ == sourceBuffer_ && draftRevision_ < sourceRevision_)
            cancelPreview(draftWindow_);
    } else if (name == "kalam.forms" && args.size() == 2) {
        forms_ = args[0].toBool();
        QHash<qint64, Document> visible;
        qint64 restoreFocus = 0;
        for (const auto &item : args[1].toList()) {
            const auto values = item.toList();
            if (values.size() != 5 && values.size() != 6) continue;
            auto doc = readDocument(values);
            if (values.size() == 6) doc.panel = values[5].toString().toUtf8();
            const auto previous = documents_.constFind(doc.window);
            if (previous != documents_.cend() && previous->buffer == doc.buffer
                && previous->revision == doc.revision && previous->path == doc.path
                && previous->panel == doc.panel) {
                doc = *previous;
            } else {
                auto *pane = panes_.take(doc.window);
                if (pane && (pane->hasFocus() || pane->isAncestorOf(QApplication::focusWidget())))
                    restoreFocus = doc.window;
                rememberForm(doc.window, pane);
                if (pane) cancelPreview(doc.window);
                delete pane;
            }
            visible.insert(doc.window, std::move(doc));
        }
        for (const auto window : panes_.keys()) {
            if (visible.contains(window) && forms_) continue;
            auto *pane = panes_.take(window);
            rememberForm(window, pane);
            cancelPreview(window);
            delete pane;
        }
        documents_ = std::move(visible);
        layout();
        if (auto *pane = panes_.value(restoreFocus)) pane->setFocus();
    } else if (name == "kalam.form_closed" && args.size() == 1) {
        const auto window = args[0].toLongLong();
        cancelPreview(window);
        delete panes_.take(window);
        documents_.remove(window);
        formViews_.remove(window);
    } else if (name == "kalam.form_focus" && args.size() == 1) {
        if (auto *pane = panes_.value(args[0].toLongLong())) pane->setFocus();
    } else {
        return false;
    }
    return true;
}

void JsonViews::layout() {
    if (!forms_) return;
    if (cmdline_) {
        for (auto *pane : panes_) pane->hide();
        return;
    }
    for (const auto &doc : documents_) {
        auto *pane = panes_.value(doc.window);
        const QRect cells = grid_->model().window_screen_rect(doc.window);
        if (!cells.isValid() || cells.width() < 4 || cells.height() < 2 ||
            grid_->model().window_is_floating(doc.window)) {
            if (pane) pane->hide();
            continue;
        }
        if (!pane) {
            pane = makePane(doc);
            panes_.insert(doc.window, pane);
        }
        const QSize cell = grid_->cell_size();
        pane->setGeometry(cells.x()*cell.width(), cells.y()*cell.height(),
                          cells.width()*cell.width(), cells.height()*cell.height());
        pane->show();
        pane->raise();
    }
}

QWidget *JsonViews::makePane(const Document &doc) {
    auto *tree = new FormTree(grid_, [this, window = doc.window] {
        grid_->setFocus();
        rpc_->notify("nvim_set_current_win", {window});
        rpc_->notify("nvim_input", {":"});
    });
    tree->setFont(QFontDatabase::systemFont(QFontDatabase::GeneralFont));
    tree->setHeaderLabels({QFileInfo(doc.path).fileName(), "Value"});
    tree->header()->setSectionResizeMode(QHeaderView::Stretch);
    tree->setObjectName("kalam-json-form");
    tree->setAlternatingRowColors(true);
    tree->setIndentation(16);
    tree->setAllColumnsShowFocus(true);
    tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), tree);
    escape->setContext(Qt::WidgetShortcut);
    QObject::connect(escape, &QShortcut::activated, tree, [this] { grid_->setFocus(); });
    if (useNeovimPaneKeys_) {
        auto *switchPane = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_W), tree);
        switchPane->setContext(Qt::WidgetWithChildrenShortcut);
        QObject::connect(switchPane, &QShortcut::activated, tree, [this,doc] {
            cancelPreview(doc.window);
            grid_->setFocus();
            rpc_->notify("nvim_set_current_win", {doc.window});
            rpc_->notify("nvim_input", {"<C-w>"});
        });
    }
    auto parsed = Spans(doc.text).parse();
    if (!parsed) {
        new QTreeWidgetItem(tree, {"Invalid JSON", "Use :KalamMode text to edit"});
        return tree;
    }
    const auto root = std::make_shared<Node>(std::move(*parsed));
    const auto add = [&](auto &self, const Node &node, QTreeWidgetItem *parent) -> void {
        const auto type = kind(node);
        const bool group = type == Kind::Json && !node.children.empty();
        auto summary = group ? QString("%1 %2").arg(node.children.size()).arg(node.value.isArray() ? "items" : "fields")
            : type == Kind::String ? node.value.toString() : QString::fromUtf8(encode(node.value)).left(160);
        if (type == Kind::Vector) {
            QStringList components;
            const auto vector = node.value.toArray();
            for (qsizetype i = 0; i < vector.size(); ++i)
                components << QString("%1 %2").arg(QStringLiteral("XYZW")[i])
                    .arg(vector[i].toDouble(), 0, 'g', 6);
            summary = components.join("   ");
        }
        auto *row = new QTreeWidgetItem(QStringList{node.name, summary});
        if (group) {
            QFont section = tree->font();
            section.setWeight(QFont::DemiBold);
            row->setFont(0, section);
            row->setBackground(0, tree->palette().window());
            row->setBackground(1, tree->palette().window());
            row->setForeground(1, tree->palette().placeholderText());
        }
        if (type == Kind::Color) {
            const auto channels = node.value.toArray();
            const auto display = [&node](double value) {
                if (node.name != "base_color") return value;
                return value <= 0.0031308 ? 12.92 * value : 1.055 * std::pow(value, 1. / 2.4) - 0.055;
            };
            QPixmap swatch(20, 12);
            swatch.fill(QColor::fromRgbF(display(channels[0].toDouble()), display(channels[1].toDouble()),
                                        display(channels[2].toDouble())));
            row->setIcon(0, QIcon(swatch));
        }
        row->setData(0, Qt::UserRole, qlonglong(node.start));
        if (type == Kind::Boolean) {
            row->setText(1, QString());
            row->setCheckState(1, node.value.toBool() ? Qt::Checked : Qt::Unchecked);
        }
        if (parent) parent->addChild(row); else tree->addTopLevelItem(row);
        if (group)
            for (const auto &child : node.children) self(self, child, row);
    };
    if (!doc.panel.isEmpty()) {
        const auto panel = QJsonDocument::fromJson(doc.panel).object();
        tree->setHeaderLabels({panel.value("title").toString(), "Value"});
        for (const auto &entry : panel.value("controls").toArray()) {
            const auto control = entry.toObject();
            const auto *node = boundNode(*root, control.value("bind").toString());
            const auto label = control.value("label").toString();
            if (node) {
                add(add, *node, nullptr);
                auto *row = tree->topLevelItem(tree->topLevelItemCount() - 1);
                row->setText(0, label);
                if (control.contains("choices") || (control.contains("min") && control.contains("max"))) {
                    row->setData(0, Qt::UserRole + 2, QJsonDocument(control).toJson(QJsonDocument::Compact));
                    if (control.contains("choices") && node->value.isBool()) {
                        row->setData(1, Qt::CheckStateRole, QVariant{});
                        row->setText(1, QString::fromUtf8(encode(node->value)));
                    }
                }
            } else {
                auto *row = new QTreeWidgetItem(tree, {label, "Unresolved binding"});
                row->setDisabled(true);
            }
        }
    } else if (root->children.empty() || kind(*root) != Kind::Json) add(add, *root, nullptr);
    else for (const auto &child : root->children) add(add, child, nullptr);
    tree->expandToDepth(0);
    const auto previous = formViews_.constFind(doc.window);
    auto *selected = tree->topLevelItem(0);
    if (previous != formViews_.cend() && previous->buffer == doc.buffer) {
        auto *parent = tree->invisibleRootItem();
        for (const auto &label : previous->selection) {
            QTreeWidgetItem *match = nullptr;
            for (int i = 0; i < parent->childCount(); ++i)
                if (parent->child(i)->text(0) == label) {
                    match = parent->child(i);
                    break;
                }
            if (!match) break;
            parent->setExpanded(true);
            selected = parent = match;
        }
        const int scroll = previous->scroll;
        QTimer::singleShot(0, tree, [tree, scroll] { tree->verticalScrollBar()->setValue(scroll); });
    }
    if (selected) tree->setCurrentItem(selected);
    const auto install = [this, doc, tree, root](auto &self, QTreeWidgetItem *item) -> void {
        if (item->childCount()) {
            if (item->isExpanded())
                for (int i = 0; i < item->childCount(); ++i) self(self, item->child(i));
            return;
        }
        if (item->isDisabled() || item->data(1, Qt::CheckStateRole).isValid() || tree->itemWidget(item, 1))
            return;
        const auto *node = atStart(*root, item->data(0, Qt::UserRole).toLongLong());
        if (!node) return;
        const bool exposure = node == boundNode(*root, "/exposure") && node->value.isDouble() &&
            QFileInfo(doc.path) == QFileInfo(scenePath_);
        const auto control = QJsonDocument::fromJson(item->data(0, Qt::UserRole + 2).toByteArray()).object();
        auto *box = new QWidget(tree);
        box->setObjectName("kalam-inline-value");
        box->setAutoFillBackground(true);
        const auto apply = [this, doc, tree, item, box, node, root, exposure](const QJsonValue &value) {
            if (!box->isEnabled()) return;
            inlineError(box, {});
            if (value == node->value) {
                cancelPreview(doc.window);
                return;
            }
            tree->setCurrentItem(item);
            box->setEnabled(false);
            commit(doc, node->start, node->end, encode(value), nullptr, nullptr, {},
                   [this, guard=QPointer<QWidget>(box), doc, value, exposure] {
                if (exposure) acceptExposure(doc, value.toDouble());
                else cancelPreview(doc.window);
                rpc_->notify("nvim_exec_autocmds", {"User", QVariantMap{{"pattern", "KalamFormsRefresh"}}});
                if (guard) guard->setEnabled(true);
            }, [this, guard=QPointer<QWidget>(box), window=doc.window, exposure](const QString &error) {
                cancelPreview(window);
                if (guard) {
                    guard->setEnabled(true);
                    inlineError(guard, error);
                }
            });
        };
        const auto preview = [this, doc, start=node->start, end=node->end, exposure](const QJsonValue &value) {
            if (exposure)
                editExposure(doc, start, end, value.toDouble(), false);
            else
                previewValue(doc, start, end, encode(value));
        };
        populateInline(box, *node, control, apply, preview, [this, doc] {
            cancelPreview(doc.window);
        }, exposure);
        item->setText(1, {});
        tree->setItemWidget(item, 1, box);
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) install(install, tree->topLevelItem(i));
    QObject::connect(tree, &QTreeWidget::itemExpanded, tree, [install](QTreeWidgetItem *item) {
        install(install, item);
    });
    QObject::connect(tree, &QTreeWidget::itemCollapsed, tree, [tree](QTreeWidgetItem *item) {
        const QPersistentModelIndex branch(tree->indexFromItem(item));
        // Wait for the collapse to finish before changing the view's index widgets.
        QTimer::singleShot(0, tree, [tree, branch] {
            auto *collapsed = branch.isValid() ? tree->itemFromIndex(branch) : nullptr;
            if (!collapsed || collapsed->isExpanded()) return;
            const auto release = [tree](auto &self, QTreeWidgetItem *row) -> void {
                for (int i = 0; i < row->childCount(); ++i) self(self, row->child(i));
                auto *box = tree->itemWidget(row, 1);
                if (!box || !box->isEnabled() || !box->toolTip().isEmpty()) return;
                const auto entries = box->findChildren<QLineEdit *>();
                if (std::any_of(entries.cbegin(), entries.cend(),
                                [](const QLineEdit *line) { return line->isModified(); })) return;
                tree->removeItemWidget(row, 1);
            };
            release(release, collapsed);
        });
    });
    QObject::connect(tree, &QTreeWidget::itemChanged, tree, [this,doc,tree,root](QTreeWidgetItem *item, int column) {
        if (column != 1 || !item->data(1, Qt::CheckStateRole).isValid()) return;
        const auto *node = atStart(*root, item->data(0, Qt::UserRole).toLongLong());
        if (!node || !node->value.isBool()) return;
        const auto restore = [tree=QPointer<QTreeWidget>(tree), item, old=node->value] {
            if (!tree) return;
            const QSignalBlocker block(tree);
            item->setCheckState(1, old.toBool() ? Qt::Checked : Qt::Unchecked);
        };
        const QJsonValue next(item->checkState(1) == Qt::Checked);
        if (next != node->value) {
            commit(doc, node->start, node->end, encode(next), nullptr, nullptr, restore, [this] {
                rpc_->notify("nvim_exec_autocmds", {"User", QVariantMap{{"pattern", "KalamFormsRefresh"}}});
            }, [pane=QPointer<QTreeWidget>(tree), item](const QString &message) {
                if (pane) item->setToolTip(1, message);
            });
        }
    });
    return tree;
}

void JsonViews::field(const Document &document, qsizetype offset, bool exact, const QByteArray &controlSpec) {
    if (dialog_) {
        dialog_->raise();
        return;
    }
    const auto root = Spans(document.text).parse();
    if (!root) {
        QMessageBox::warning(grid_, "JSON field", "Enter valid JSON before opening a field.");
        return;
    }
    const auto *node = exact ? atStart(*root, offset) : at(*root, offset);
    if (!node) return;
    const bool exposure = node->name == "exposure" && node->value.isDouble() &&
        QFileInfo(document.path) == QFileInfo(scenePath_) &&
        std::any_of(root->children.begin(), root->children.end(), [node](const Node &child) {
            return &child == node;
        });
    auto edit = [this, document, exposure](bool apply, const Node &target,
                                         QJsonValue value, QDialog *dialog, QLabel *error) {
        if (exposure) editExposure(document, target.start, target.end, value.toDouble(), apply, dialog, error);
        else commit(document, target.start, target.end, encode(value), dialog, error);
    };
    const auto action = [edit](bool apply) -> Apply {
        return [edit, apply](const Node &target, QJsonValue value, QDialog *dialog, QLabel *error) {
            edit(apply, target, value, dialog, error);
        };
    };
    const QPointer<QWidget> previousFocus = QApplication::focusWidget();
    dialog_ = editor(*node, grid_->window(), action(true), exposure ? action(false) : Apply{},
                     QJsonDocument::fromJson(controlSpec).object());
    dialog_->setFont(QFontDatabase::systemFont(QFontDatabase::GeneralFont));
    QObject::connect(dialog_, &QDialog::finished, grid_, [this, document, previousFocus] {
        cancelPreview(document.window);
        if (previousFocus && previousFocus->isVisible()) previousFocus->setFocus();
        else grid_->setFocus();
    });
    dialog_->setAttribute(Qt::WA_DeleteOnClose);
    dialog_->open();
}

void JsonViews::resetPreview() {
    appliedExposure_ = 0;
    appliedBuffer_ = 0;
    draftWindow_ = draftBuffer_ = draftRevision_ = 0;
    kalam_embed_exposure(0);
}

void JsonViews::cancelPreview(qint64 window) {
    if (window && documentDraftWindow_ == window) {
        documentDraftWindow_ = 0;
        preview_({}, {});
    }
    if (!window || draftWindow_ != window) return;
    draftWindow_ = draftBuffer_ = draftRevision_ = 0;
    kalam_embed_exposure(appliedExposure_);
}

void JsonViews::previewValue(const Document &document, qsizetype start, qsizetype end,
                             const QByteArray &value) {
    auto candidate = document.text;
    candidate.replace(start, end - start, value);
    documentDraftWindow_ = document.window;
    preview_(document.path, candidate);
}

void JsonViews::tuiApplied(qint64 window, const QString &message) {
    rpc_->notify("nvim_exec_autocmds", {"User", QVariantMap{{"pattern", "KalamTuiApplied"},
        {"data", QVariantMap{{"window", window}, {"message", message}}}}});
}

void JsonViews::editExposure(const Document &doc, qsizetype start, qsizetype end,
                             double value, bool apply, QDialog *dialog, QLabel *error, bool tui) {
    const auto replacement = encode(QJsonValue(value));
    const auto complete = [this, window=doc.window, dialog=QPointer<QDialog>(dialog), tui](
                              const QString &message = {}) {
        if (dialog) dialog->setEnabled(true);
        if (tui) tuiApplied(window, message);
    };
    if (!apply) {
        QString message;
        if (doc.buffer == sourceBuffer_ && doc.revision < sourceRevision_) {
            message = "Buffer changed. Reopen the field.";
        } else if (!(value >= exposureMin && value <= exposureMax)) {
            message = "Exposure must be between 0.01 and 32.";
        } else if (draftWindow_ != doc.window || draftBuffer_ != doc.buffer || draftRevision_ != doc.revision) {
            auto candidate = doc.text;
            candidate.replace(start, end - start, replacement);
            message = validate_(doc.path, candidate);
        }
        if (error) error->setText(message);
        if (!message.isEmpty()) return;
        draftWindow_ = doc.window;
        draftBuffer_ = doc.buffer;
        draftRevision_ = doc.revision;
        kalam_embed_exposure(float(value));
        return;
    }
    if (dialog) dialog->setEnabled(false);
    commit(doc, start, end, replacement, dialog, error, {}, [this, doc, value, complete] {
        acceptExposure(doc, value);
        complete();
    }, [this, doc, complete](const QString &message) {
        cancelPreview(doc.window);
        complete(message);
    });
}

void JsonViews::acceptExposure(const Document &doc, double value) {
    appliedExposure_ = float(value);
    appliedBuffer_ = doc.buffer;
    cancelPreview(doc.window);
    if (!draftWindow_) kalam_embed_exposure(appliedExposure_);
}

void JsonViews::commit(const Document &document, qsizetype start, qsizetype end,
                       const QByteArray &replacement, QDialog *dialog, QLabel *error,
                       std::function<void()> rejected, std::function<void()> accepted,
                       std::function<void(const QString &)> report) {
    auto text = document.text;
    text.replace(start, end - start, replacement);
    const auto validation = validate_(document.path, text);
    if (!validation.isEmpty()) {
        if (rejected) rejected();
        if (report) report(validation);
        if (error) error->setText(validation);
        else if (!report) QMessageBox::warning(grid_, "JSON field", validation);
        return;
    }
    const auto patch = QStringLiteral(R"lua(
      local buf, tick, first, last, value = ...
      if not vim.api.nvim_buf_is_loaded(buf) or vim.api.nvim_buf_get_changedtick(buf) ~= tick then
        error("Buffer changed. Reopen the field.")
      end
      local function pos(offset)
        local row = 0
        for _, line in ipairs(vim.api.nvim_buf_get_lines(buf, 0, -1, false)) do
          if offset <= #line then return row, offset end
          row, offset = row + 1, offset - #line - 1
        end
        error("Invalid JSON field location.")
      end
      local start_row, start_col = pos(first)
      local end_row, end_col = pos(last)
      vim.api.nvim_buf_set_text(buf, start_row, start_col, end_row, end_col, vim.split(value, "\n", {plain=true}))
    )lua");
    auto *request = rpc_->call("nvim_exec_lua", {patch, QVariantList{document.buffer,
        document.revision, qlonglong(start), qlonglong(end), QString::fromUtf8(replacement)}});
    if (!request) {
        if (rejected) rejected();
        if (report) report("Neovim is unavailable.");
        return;
    }
    QObject::connect(request, &Neovim_Rpc_Request::finished, grid_,
        [dialog=QPointer<QDialog>(dialog), accepted=std::move(accepted)] {
        if (accepted) accepted();
        if (dialog) dialog->accept();
    });
    QObject::connect(request, &Neovim_Rpc_Request::failed, grid_, [this,error=QPointer<QLabel>(error),
        rejected=std::move(rejected), report=std::move(report)](
        quint32, const QVariant &, const QString &message) {
        if (rejected) rejected();
        if (report) report(message);
        if (error) error->setText(message);
        else if (!report) QMessageBox::warning(grid_, "JSON field", message);
    });
}
