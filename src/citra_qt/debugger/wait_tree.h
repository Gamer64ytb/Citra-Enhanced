// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <span>
#include <QAbstractItemModel>
#include <QDockWidget>
#include <QTreeView>
#include <boost/container/flat_set.hpp>
#include "core/core.h"

class EmuThread;

namespace Kernel {
class KSynchronizationObject;
class KEvent;
class KMutex;
class KSemaphore;
class KThread;
class KTimer;
} // namespace Kernel

namespace Core {
class System;
}

class WaitTreeThread;

class WaitTreeItem : public QObject {
    Q_OBJECT
public:
    ~WaitTreeItem() override;

    virtual bool IsExpandable() const;
    virtual std::vector<std::unique_ptr<WaitTreeItem>> GetChildren() const;
    virtual QString GetText() const = 0;
    virtual QColor GetColor() const;

    void Expand();
    WaitTreeItem* Parent() const;
    std::span<const std::unique_ptr<WaitTreeItem>> Children() const;
    std::size_t Row() const;
    static std::vector<std::unique_ptr<WaitTreeThread>> MakeThreadItemList(Core::System& system);

private:
    std::size_t row;
    bool expanded = false;
    WaitTreeItem* parent = nullptr;
    std::vector<std::unique_ptr<WaitTreeItem>> children;
};

class WaitTreeText : public WaitTreeItem {
    Q_OBJECT
public:
    explicit WaitTreeText(QString text);
    ~WaitTreeText() override;

    QString GetText() const override;

private:
    QString text;
};

class WaitTreeExpandableItem : public WaitTreeItem {
    Q_OBJECT
public:
    bool IsExpandable() const override;
};

class WaitTreeWaitObject : public WaitTreeExpandableItem {
    Q_OBJECT
public:
    explicit WaitTreeWaitObject(const Kernel::KSynchronizationObject& object);
    static std::unique_ptr<WaitTreeWaitObject> make(const Kernel::KSynchronizationObject& object);
    QString GetText() const override;
    std::vector<std::unique_ptr<WaitTreeItem>> GetChildren() const override;

protected:
    const Kernel::KSynchronizationObject& object;

    static QString GetResetTypeQString(Kernel::ResetType reset_type);
};

class WaitTreeObjectList : public WaitTreeExpandableItem {
    Q_OBJECT
public:
    WaitTreeObjectList(const std::vector<Kernel::KSynchronizationObject*>& list, bool wait_all);
    QString GetText() const override;
    std::vector<std::unique_ptr<WaitTreeItem>> GetChildren() const override;

private:
    const std::vector<Kernel::KSynchronizationObject*>& object_list;
    bool wait_all;
};

class WaitTreeThread : public WaitTreeWaitObject {
    Q_OBJECT
public:
    explicit WaitTreeThread(const Kernel::KThread& thread);
    QString GetText() const override;
    QColor GetColor() const override;
    std::vector<std::unique_ptr<WaitTreeItem>> GetChildren() const override;
};

class WaitTreeEvent : public WaitTreeWaitObject {
    Q_OBJECT
public:
    explicit WaitTreeEvent(const Kernel::KEvent& object);
    std::vector<std::unique_ptr<WaitTreeItem>> GetChildren() const override;
};

class WaitTreeMutex : public WaitTreeWaitObject {
    Q_OBJECT
public:
    explicit WaitTreeMutex(const Kernel::KMutex& object);
    std::vector<std::unique_ptr<WaitTreeItem>> GetChildren() const override;
};

class WaitTreeSemaphore : public WaitTreeWaitObject {
    Q_OBJECT
public:
    explicit WaitTreeSemaphore(const Kernel::KSemaphore& object);
    std::vector<std::unique_ptr<WaitTreeItem>> GetChildren() const override;
};

class WaitTreeTimer : public WaitTreeWaitObject {
    Q_OBJECT
public:
    explicit WaitTreeTimer(const Kernel::KTimer& object);
    std::vector<std::unique_ptr<WaitTreeItem>> GetChildren() const override;
};

class WaitTreeMutexList : public WaitTreeExpandableItem {
    Q_OBJECT
public:
    explicit WaitTreeMutexList(const boost::container::flat_set<Kernel::KMutex*>& list);

    QString GetText() const override;
    std::vector<std::unique_ptr<WaitTreeItem>> GetChildren() const override;

private:
    const boost::container::flat_set<Kernel::KMutex*>& mutex_list;
};

class WaitTreeThreadList : public WaitTreeExpandableItem {
    Q_OBJECT
public:
    explicit WaitTreeThreadList(const std::vector<Kernel::KThread*>& list);
    QString GetText() const override;
    std::vector<std::unique_ptr<WaitTreeItem>> GetChildren() const override;

private:
    const std::vector<Kernel::KThread*>& thread_list;
};

class WaitTreeModel : public QAbstractItemModel {
    Q_OBJECT

public:
    explicit WaitTreeModel(QObject* parent = nullptr);

    QVariant data(const QModelIndex& index, int role) const override;
    QModelIndex index(int row, int column, const QModelIndex& parent) const override;
    QModelIndex parent(const QModelIndex& index) const override;
    int rowCount(const QModelIndex& parent) const override;
    int columnCount(const QModelIndex& parent) const override;

    void ClearItems();
    void InitItems(Core::System& system);

private:
    std::vector<std::unique_ptr<WaitTreeThread>> thread_items;
};

class WaitTreeWidget : public QDockWidget {
    Q_OBJECT

public:
    explicit WaitTreeWidget(Core::System& system, QWidget* parent = nullptr);

public slots:
    void OnDebugModeEntered();
    void OnDebugModeLeft();

    void OnEmulationStarting(EmuThread* emu_thread);
    void OnEmulationStopping();

private:
    QTreeView* view;
    WaitTreeModel* model;
    Core::System& system;
};
