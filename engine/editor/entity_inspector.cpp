#include "entity_inspector.h"
#include "embedded.h"

#include <QComboBox>
#include <QColorDialog>
#include <cmath>
#include <algorithm>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {
enum Mode { Inherit, Override, Remove };

double toSrgb(double linear) {
    return linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

double toLinear(double srgb) {
    return srgb <= 0.04045 ? srgb / 12.92 : std::pow((srgb + 0.055) / 1.055, 2.4);
}
}

EntityInspector::EntityInspector(QJsonObject document, const QJsonArray &descriptors,
                                 const QJsonObject &referenceMaterial, Commit commit, QWidget *parent)
    : QDialog(parent), document_(std::move(document)),
      entities_(document_.value("entities").toArray()) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle("Scene entities");
    resize(550, 610);
    auto *layout = new QVBoxLayout(this);
    auto *bar = new QHBoxLayout;
    entitiesCombo_ = new QComboBox;
    entitiesCombo_->setObjectName("entity-picker");
    auto *add = new QPushButton("Add entity");
    auto *remove = new QPushButton("Remove entity");
    bar->addWidget(entitiesCombo_, 1);
    bar->addWidget(add);
    bar->addWidget(remove);
    layout->addLayout(bar);
    status_ = new QLabel;
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto *form = new QFormLayout;
    archetypeCombo_ = new QComboBox;
    archetypeCombo_->addItem("None", QString{});
    const auto archetypes = document_.value("archetypes").toObject();
    for (const auto &name : archetypes.keys())
        archetypeCombo_->addItem(name, name);
    form->addRow("Archetype", archetypeCombo_);
    connect(archetypeCombo_, &QComboBox::currentIndexChanged, this, [this](int choice) {
        if (index_ < 0 || !stash()) {
            const QSignalBlocker blocker(archetypeCombo_);
            const auto prior = index_ < 0 ? QString{} : entities_.at(index_).toObject().value("archetype").toString();
            archetypeCombo_->setCurrentIndex(archetypeCombo_->findData(prior));
            return;
        }
        auto entity = entities_.at(index_).toObject();
        const auto name = archetypeCombo_->itemData(choice).toString();
        if (name.isEmpty())
            entity.remove("archetype");
        else
            entity.insert("archetype", name);
        entities_.replace(index_, entity);
        loadEntity();
    });
    for (const auto &item : descriptors) {
        const auto schema = item.toObject();
        auto *mode = new QComboBox;
        mode->addItems({"Inherit", "Local override", "Remove"});
        auto *value = new QPlainTextEdit;
        value->setTabChangesFocus(true);
        value->setMaximumHeight(96);
        auto *container = new QVBoxLayout;
        container->addWidget(mode);
        container->addWidget(value);
        form->addRow(schema.value("label").toString(), container);
        const auto key = schema.value("key").toString();
        mode->setObjectName("component-" + key);
        const auto fallback = schema.value("default").toObject();
        if (key == "render") {
            auto *color = new QPushButton("Base color...");
            container->addWidget(color);
            connect(color, &QPushButton::clicked, this, [this, mode, value, fallback, referenceMaterial] {
                if (index_ < 0)
                    return;
                auto render = mode->currentIndex() == Override
                    ? QJsonDocument::fromJson(value->toPlainText().toUtf8()).object()
                    : inherited("render");
                if (render.isEmpty() && mode->currentIndex() == Override) {
                    showError("Render must be a valid component object first.");
                    return;
                }
                if (render.isEmpty())
                    render = fallback;
                const auto source = render.value("material");
                auto material = source.isString() ? referenceMaterial : source.toObject();
                const auto rgba = material.value("base_color").toArray();
                if (rgba.size() != 4 || std::any_of(rgba.begin(), rgba.end(), [](const auto &c) {
                    return !c.isDouble() || c.toDouble() < 0.0 || c.toDouble() > 1.0;
                })) {
                    showError("Base color must contain four values in 0..1.");
                    return;
                }
                auto *picker = new QColorDialog(QColor::fromRgbF(
                    toSrgb(rgba[0].toDouble()), toSrgb(rgba[1].toDouble()),
                    toSrgb(rgba[2].toDouble()), rgba[3].toDouble()), this);
                picker->setOptions(QColorDialog::ShowAlphaChannel | QColorDialog::DontUseNativeDialog);
                picker->setAttribute(Qt::WA_DeleteOnClose);
                connect(picker, &QColorDialog::colorSelected, value, [mode, value, render, material](const QColor &c) mutable {
                    mode->setCurrentIndex(Override);
                    material.insert("base_color", QJsonArray{
                        toLinear(c.redF()), toLinear(c.greenF()), toLinear(c.blueF()), c.alphaF()});
                    render.insert("material", material);
                    value->setPlainText(QString::fromUtf8(QJsonDocument(render).toJson()));
                });
                picker->open();
            });
        }
        rows_.push_back({key, fallback, mode, value});
        connect(mode, &QComboBox::currentIndexChanged, this, [this, key, value, fallback](int state) {
            value->setReadOnly(state != Override);
            if (state == Override && value->toPlainText().trimmed().isEmpty()) {
                const auto base = inherited(key);
                value->setPlainText(QString::fromUtf8(QJsonDocument(base.isEmpty() ? fallback : base).toJson()));
            }
        });
    }
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    buttons->button(QDialogButtonBox::Apply)->setObjectName("apply-entity-edit");
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this, commit] {
        if (!stash())
            return;
        document_.insert("entities", entities_);
        commit(QJsonDocument(document_).toJson(), this);
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    for (const auto &entity : entities_)
        entitiesCombo_->addItem(entity.toObject().value("id").toString());
    connect(entitiesCombo_, &QComboBox::currentIndexChanged, this, &EntityInspector::selectEntity);
    connect(add, &QPushButton::clicked, this, [this] {
        if (!stash())
            return;
        bool accepted = false;
        const QString id = QInputDialog::getText(this, "Add entity", "Entity ID",
                                                 QLineEdit::Normal, {}, &accepted);
        if (!accepted)
            return;
        if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,64}$")).match(id).hasMatch()
            || entitiesCombo_->findText(id) >= 0 || entities_.size() >= 512) {
            showError("Use a unique entity ID containing letters, numbers, hyphens or underscores.");
            return;
        }
        entities_.append(QJsonObject{{"id", id}, {"components", QJsonObject{}}});
        entitiesCombo_->addItem(id);
        entitiesCombo_->setCurrentIndex(entitiesCombo_->count() - 1);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (index_ < 0)
            return;
        const int removed = index_;
        index_ = -1;
        entities_.removeAt(removed);
        const QSignalBlocker blocker(entitiesCombo_);
        entitiesCombo_->removeItem(removed);
        index_ = entitiesCombo_->currentIndex();
        loadEntity();
    });
    connect(this, &QDialog::finished, this, [](int) { kalam_select_entity(""); });
    index_ = entitiesCombo_->currentIndex();
    loadEntity();
}

void EntityInspector::showError(const QString &message) {
    status_->setText(message);
}

QJsonObject EntityInspector::inherited(const QString &key) const {
    if (index_ < 0)
        return {};
    const auto entity = entities_.at(index_).toObject();
    const auto name = entity.value("archetype").toString();
    return document_.value("archetypes").toObject().value(name).toObject().value(key).toObject();
}

bool EntityInspector::stash() {
    if (index_ < 0)
        return true;
    auto entity = entities_.at(index_).toObject();
    auto components = entity.value("components").toObject();
    for (const auto &row : rows_) {
        switch (row.mode->currentIndex()) {
        case Inherit: components.remove(row.key); break;
        case Remove: components.insert(row.key, QJsonValue::Null); break;
        case Override: {
            const auto json = QJsonDocument::fromJson(row.value->toPlainText().toUtf8());
            if (!json.isObject()) {
                showError(QString("%1 must be a JSON object.").arg(row.key));
                row.value->setFocus();
                return false;
            }
            components.insert(row.key, json.object());
            break;
        }
        }
    }
    if (entity.contains("components") || !components.isEmpty())
        entity.insert("components", components);
    entities_.replace(index_, entity);
    return true;
}

void EntityInspector::selectEntity(int next) {
    if (!stash()) {
        const QSignalBlocker blocker(entitiesCombo_);
        entitiesCombo_->setCurrentIndex(index_);
        return;
    }
    index_ = next;
    loadEntity();
}

void EntityInspector::loadEntity() {
    const auto entity = index_ >= 0 ? entities_.at(index_).toObject() : QJsonObject{};
    {
        const QSignalBlocker blocker(archetypeCombo_);
        archetypeCombo_->setEnabled(index_ >= 0);
        archetypeCombo_->setCurrentIndex(archetypeCombo_->findData(entity.value("archetype").toString()));
    }
    const auto id = entity.value("id").toString().toUtf8();
    kalam_select_entity(id.constData());
    const auto components = entity.value("components").toObject();
    bool transform = false, render = false;
    for (const auto &row : rows_) {
        const auto local = components.value(row.key);
        const int state = local.isUndefined() ? Inherit : local.isNull() ? Remove : Override;
        const auto value = state == Override ? local.toObject() : inherited(row.key);
        const QSignalBlocker blocker(row.mode);
        row.mode->setEnabled(index_ >= 0);
        row.mode->setCurrentIndex(state);
        row.value->setReadOnly(state != Override || index_ < 0);
        row.value->setPlainText(value.isEmpty() || state == Remove ? QString{} :
                                QString::fromUtf8(QJsonDocument(value).toJson()));
        const bool effective = state != Remove && !value.isEmpty();
        transform |= row.key == "transform" && effective;
        render |= row.key == "render" && effective;
    }
    if (index_ < 0) {
        showError("Add an entity to edit its components.");
    } else {
        const auto base = entity.value("archetype").toString("none");
        showError(QString("Archetype: %1. %2").arg(base,
            transform && render ? "Visible after reload." : "Needs transform and render to appear."));
    }
}
