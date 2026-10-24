#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <vector>

namespace dvs::ui {

// Owns the video review tasks shown as tabs in one window. Unlike the still-image host, every
// task shares the single decode/render runtime: a task is a pure data record (source group in
// slot order, reference identity, opaque state snapshot), so switching tabs rebuilds the
// session while the host keeps the records, identity matching, open routing and neighbour
// selection. The host never decodes and never interprets snapshot content: the asynchronous
// open is driven by the caller through openRequested/commitOpen/failOpen, and the restore
// protocol marks tasks restoring (docs/plans/video-task-tabs-design.md §4).
class VideoTaskHost final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY tabsChanged)
    Q_PROPERTY(int activeIndex READ activeIndex NOTIFY activeChanged)
    Q_PROPERTY(QVariantList tabs READ tabs NOTIFY tabsChanged)

public:
    explicit VideoTaskHost(QObject* parent = nullptr);
    ~VideoTaskHost() override;

    VideoTaskHost(const VideoTaskHost&) = delete;
    VideoTaskHost& operator=(const VideoTaskHost&) = delete;
    VideoTaskHost(VideoTaskHost&&) = delete;
    VideoTaskHost& operator=(VideoTaskHost&&) = delete;

    [[nodiscard]] int count() const noexcept;
    [[nodiscard]] int activeIndex() const noexcept;
    [[nodiscard]] QVariantList tabs() const;

    // Open routing (design §2.2). A group that matches an existing task — identity multiset
    // plus reference identity, order-independent (decision e) — focuses that task and keeps
    // its record and state snapshot untouched; while the task is still loading or restoring
    // the activation is deferred to the commit/restore completion. Without a match a loading
    // record is created after the tab-limit check and openRequested is emitted; the caller
    // then drives the actual open through the shell and reports back. Invalid requests and a
    // reached tab limit answer with openFailed and -1.
    Q_INVOKABLE int openTask(const QVariantList& urls, int referenceIndex, const QString& purpose);

    // Session-fact writeback. commitOpen stores the frozen identities (path|size|mtime per
    // slot; recorded only, never used for matching), finishes the loading record, makes the
    // task active and emits taskOpened; on an already loaded record it only refreshes the
    // frozen identities (group evolution writeback). failOpen removes a still-loading record;
    // a loaded record stays untouched, because a failed reopen of an existing task must not
    // destroy it. Both report openFailed.
    Q_INVOKABLE bool commitOpen(int index, const QStringList& frozenIdentities);
    Q_INVOKABLE bool failOpen(int index, const QString& error);

    // Group evolution of the active task (decision f): the record is updated in place and the
    // state snapshot is kept. appendSourceToActiveTask refuses a fourth source and needs a
    // committed group, so a loading task cannot grow. removeSourceFromActiveTask matches a
    // slot by URL identity (case-insensitive) or frozen identity (exact), refuses the last
    // source (closing is closeAt/closeActive territory) and re-points a removed reference slot
    // at slot 0; a two-source task shrinks into the single-video shape on the same tab.
    Q_INVOKABLE bool appendSourceToActiveTask(const QUrl& url);
    Q_INVOKABLE bool removeSourceFromActiveTask(const QString& identity);

    // Task-record evolution (design §5 rows 5-8, §6, decision f). replaceActiveTaskGroup
    // rewrites the ACTIVE record's group in place: an in-place disc switch (row 5/8)
    // resets the state snapshot - the new material has no positions to restore - while a
    // group evolution (one source removed, §11-f) keeps it. The task keeps its tab; the
    // caller refreshes the frozen identities afterwards via commitOpen.
    Q_INVOKABLE bool
    replaceActiveTaskGroup(const QVariantList& urls, int referenceIndex, bool resetState);
    // setTaskReference updates a record's reference identity after a successful
    // ChangeReference terminal, so later openTask matching stays honest.
    Q_INVOKABLE bool setTaskReference(int index, int referenceIndex);
    // Session-fact query for adoption paths (issue restore, sidebar): the index of the
    // task whose group and reference match the given facts, or -1. Pure query - unlike
    // openTask it never creates or activates anything.
    Q_INVOKABLE int indexOfTask(const QVariantList& urls, int referenceIndex) const;

    // Opaque per-task state snapshot: stored and returned verbatim, never interpreted. QML
    // collects it when a tab is left and replays it when the task is restored (design §3).
    Q_INVOKABLE void updateTaskState(int index, const QVariantMap& state);
    Q_INVOKABLE QVariantMap taskState(int index) const;

    // Deterministic URL-list equality for the QML identity gates (design §4.6): compares
    // QVariant(QUrl) and QVariant(QString) elements through QVariant conversion in C++.
    // The JS String() of a QVariant-wrapped QUrl is not stable across engine states, and
    // an identity gate must not depend on it.
    Q_INVOKABLE bool urlsMatch(const QVariantList& left, const QVariantList& right) const;

    // Marks a task restoring while its session is being rebuilt (design §4): the tab shows
    // the restore title and cycle() skips it. Completing a restore activates a task whose
    // activation was deferred meanwhile.
    Q_INVOKABLE bool setTaskRestoring(int index, bool restoring);

    Q_INVOKABLE bool activate(int index);
    // Closing is refused for a still-loading task (design §6); closing the active task
    // selects the right neighbour, or the left one at the end.
    Q_INVOKABLE bool closeAt(int index);
    Q_INVOKABLE bool closeActive();
    // Cycles through the tabs that are neither loading nor restoring (design §8).
    Q_INVOKABLE void cycle(int delta);

    // Future extension point (decision h): cloneTask(index) stays unimplemented in the first
    // version; this comment marks where the API goes.

signals:
    void tabsChanged();
    void activeChanged();
    // A new loading record exists; the caller must start the actual open (shell queue) for
    // the recorded request and eventually answer with commitOpen/failOpen.
    void
    openRequested(int index, const QVariantList& urls, int referenceIndex, const QString& purpose);
    // The open committed and the task became active.
    void taskOpened(int index);
    void openFailed(const QString& error);
    void allTasksClosed();

private:
    struct Task {
        QList<QUrl> urls;
        QStringList urlIdentities;
        QString referenceIdentity;
        int referenceIndex = 0;
        QStringList frozenIdentities;
        QVariantMap state;
        bool loading = false;
        bool loaded = false;
        bool restoring = false;
        bool activateWhenLoaded = false;
    };

    [[nodiscard]] int indexOfMatchingTask(const QStringList& identities,
                                          const QString& referenceIdentity) const;
    void setActiveIndex(int index);
    void removeTask(int index, bool notify);
    [[nodiscard]] QString titleFor(const Task& task) const;
    [[nodiscard]] QString detailFor(const Task& task) const;

    std::vector<Task> tasks_;
    int activeIndex_ = -1;
};

} // namespace dvs::ui
