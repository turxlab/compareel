#pragma once
#include "Common.h"
#include <QDialog>
#include <functional>
#include <vector>

class QCheckBox;
class QDoubleSpinBox;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTreeWidget;

class ScanSourcesDialog final : public QDialog {
public:
    static constexpr int SaveOnly = 2;
    ScanSourcesDialog(const std::vector<std::wstring>& folders, const ScanCriteria& options, QWidget* parent = nullptr);
    bool applyTo(std::vector<std::wstring>& folders, ScanCriteria& options, QWidget* messageParent) const;

private:
    void addIncluded(const QString& rawPath, const ScanCriteria& options);
    QTreeWidget* included_ = nullptr;
    QCheckBox* recursive_ = nullptr;
    QListWidget* excluded_ = nullptr;
    QCheckBox* crossFolders_ = nullptr;
};

class ScanRulesDialog final : public QDialog {
public:
    ScanRulesDialog(const ScanCriteria& options, bool customProfileActive, QWidget* parent = nullptr);
    bool applyTo(ScanCriteria& options, QWidget* messageParent) const;

private:
    QDoubleSpinBox* minimum_ = nullptr;
    QDoubleSpinBox* maximum_ = nullptr;
    QLineEdit* extensions_ = nullptr;
    QLineEdit* excludedExtensions_ = nullptr;
    QSpinBox* workers_ = nullptr;
    QDoubleSpinBox* threshold_ = nullptr;
    QDoubleSpinBox* tolerance_ = nullptr;
    QCheckBox* transforms_ = nullptr;
    QCheckBox* size_ = nullptr;
    QCheckBox* durationCheck_ = nullptr;
    QCheckBox* resolution_ = nullptr;
    QCheckBox* first_ = nullptr;
    QCheckBox* fileNameCheck_ = nullptr;
    QCheckBox* extensionCheck_ = nullptr;
};

class OperationHistoryDialog final : public QDialog {
public:
    OperationHistoryDialog(const QString& cachePath, std::function<void()> onHistoryCleared, QWidget* parent = nullptr);
};
