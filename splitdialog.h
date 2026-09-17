#ifndef SPLITDIALOG_H
#define SPLITDIALOG_H

#include <QDialog>
#include <QDateTime>
#include "types.h"

class QLabel;
class QSlider;
class QRadioButton;

// kMinimumSplitDurationSeconds lives in types.h (D9 review, finding 1 round
// 3): shared with HistoryEditSession::splitRow()/splitRejection() so the two
// rules cannot drift apart.

/**
 * SplitDialog — modal dialog for splitting a single TimeDuration into two.
 *
 * Takes the start and end time of an existing segment. The user drags a slider
 * to choose a split point, and picks the type assignment for each half
 * (Activity→Pause or Pause→Activity). No dependency on HistoryDialog internals.
 */
class SplitDialog : public QDialog
{
    Q_OBJECT

public:
    SplitDialog(const QDateTime& start, const QDateTime& end, QWidget* parent = nullptr);

    QDateTime getSplitTime() const;
    DurationType getFirstSegmentType() const;
    DurationType getSecondSegmentType() const;

    void setFirstSegmentType(DurationType type);
    void setSecondSegmentType(DurationType type);

private slots:
    void updateSplitLabel(int value);

private:
    QDateTime start_;
    QDateTime end_;
    QSlider* slider_;
    QLabel* splitTimeLabel_;

    QRadioButton* activityPauseOption_;
    QRadioButton* pauseActivityOption_;

#ifndef QT_NO_DEBUG
public:
    /// Debug-build probe: exposes the split-point slider for white-box tests.
    QSlider* slider_dbg() const { return slider_; }
#endif
};

#endif // SPLITDIALOG_H
