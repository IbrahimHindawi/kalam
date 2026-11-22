#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <functional>
#include <vector>

class QComboBox;
class QLabel;
class QPlainTextEdit;

class EntityInspector final : public QDialog {
public:
    using Commit = std::function<void(const QByteArray &, EntityInspector *)>;
    EntityInspector(QJsonObject document, const QJsonArray &descriptors,
                    const QJsonObject &referenceMaterial, Commit commit, QWidget *parent);
    void showError(const QString &message);

private:
    struct Row {
        QString key;
        QJsonObject fallback;
        QComboBox *mode;
        QPlainTextEdit *value;
    };

    QJsonObject document_;
    QJsonArray entities_;
    QComboBox *entitiesCombo_;
    QComboBox *archetypeCombo_;
    QLabel *status_;
    std::vector<Row> rows_;
    int index_ = -1;

    bool stash();
    void selectEntity(int next);
    void loadEntity();
    QJsonObject inherited(const QString &key) const;
};
