/**
 * ContentWidget - View + light controller for the main timer UI.
 *
 * Owns the UI elements (buttons, labels) and handles user interactions:
 * button presses write settings, launch HistoryDialog, and build tooltips.
 * TimeTracker state changes propagate here via one transitioned() signal
 * connection, reduced by onTransitioned() to update the visual state.
 * updateTimes() is called by the main loop (MainWin::onTick) to refresh the
 * time display labels by polling TimeTracker.
 */

#include "contentwidget.h"
#include <QDateTime>
#include <QtDebug>
#include <QFont>
#include <QColor>
#include <QStringList>
#include <QApplication>
#include <QDialog>
#include <QVBoxLayout>
#include <QLabel>
#include <QTableWidget>
#include <QHeaderView>
#include <QCheckBox>
#include "timeformat.h"
#include "historydialog.h"

static void toggleButtonColor(QPushButton* const button, const QColor& color)
{
    const QString stylesheet_string = QString("QPushButton {background-color: %1;}").arg(color.name());
    if (button->styleSheet() != stylesheet_string)
        button->setStyleSheet(stylesheet_string);
    else
        button->setStyleSheet("");
}

ContentWidget::ContentWidget(Settings & settings, TimeTracker &timetracker, QWidget *parent)
	: QWidget(parent), settings_(settings), timetracker_(timetracker), button_hold_color_(QColor(180,216,228,255))
{
	setupGUI();

	QObject::connect(startpause_button_, SIGNAL(clicked()), this, SLOT(pressedStartPauseButton()));
	QObject::connect(stop_button_, SIGNAL(clicked()), this, SLOT(pressedStopButton()));
	QObject::connect(mintotray_button_, SIGNAL(clicked()), this, SLOT(pressedMinToTrayButton()));
	QObject::connect(pintotop_button_, SIGNAL(clicked()), this, SLOT(pressedPinToTopButton()));
	QObject::connect(autopause_button_, SIGNAL(clicked()), this, SLOT(pressedAutoPauseButton()));
	QObject::connect(show_history_button_, SIGNAL(clicked()), this, SLOT(pressedShowHistoryButton()));

	// D10 (R10-002/requirement 4): one connection to the one cause-carrying
	// transition event, reduced below by onTransitioned(). Previously this
	// required four connections to started()/paused()/stopped()/modeChanged()
	// and a comment warning that lock resume's two signals must not both be
	// acted on; a single event makes double-acting structurally impossible.
	connect(&timetracker_, &TimeTracker::transitioned,
	        this, &ContentWidget::onTransitioned);
}

void ContentWidget::setupGUI()
{
	rows_ = new QVBoxLayout(this);

	setupTimeRows();
	setupButtonRows();

	rows_->addLayout(starttime_row_);
	rows_->addLayout(activity_row_);
	rows_->addLayout(pause_row_);
	rows_->addLayout(timerbutton_row_);
	rows_->addLayout(optionbutton_row_);
	rows_->addLayout(bottombutton_row_);
	

	applyStartupSettingsToGui();
}

void ContentWidget::setupTimeRows()
{
	QFont label_font = QApplication::font();
	label_font.setPixelSize(12);

	starttime_row_ = new QHBoxLayout();
	starttime_text_ = new QLabel("Start Time:");
	starttime_text_->setFont(label_font);
	starttime_value_ = new QLabel("--:--");
	starttime_value_->setFont(label_font);
	starttime_value_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	starttime_row_->addWidget(starttime_text_);
	starttime_row_->addWidget(starttime_value_);

	activity_row_ = new QHBoxLayout();
	activity_text_ = new QLabel("Activity Time:");
	activity_text_->setFont(label_font);
	activity_time_ = new QLabel("00:00:00");
	activity_time_->setFont(label_font);
	activity_time_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	activity_row_->addWidget(activity_text_);
	activity_row_->addWidget(activity_time_);

	pause_row_ = new QHBoxLayout();
	pause_text_ = new QLabel("Pause Time:");
	pause_text_->setFont(label_font);
	pause_time_ = new QLabel("00:00:00");
	pause_time_->setFont(label_font);
	pause_time_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	pause_row_->addWidget(pause_text_);
	pause_row_->addWidget(pause_time_);
}

void ContentWidget::setupButtonRows()
{
	QFont button_font = QApplication::font();
	button_font.setPixelSize(11);

	timerbutton_row_ = new QHBoxLayout();
	startpause_button_ = new QPushButton("START");
	startpause_button_->setObjectName("startPauseButton");
	startpause_button_->setFont(button_font);
	startpause_button_->setToolTip("Start/Pause Activity Time");
	startpause_button_->setFixedSize(100,25);
	stop_button_ = new QPushButton("STOP");
	stop_button_->setFont(button_font);
	stop_button_->setToolTip("Stop all Timing");
	stop_button_->setFixedSize(100, 25);
	timerbutton_row_->addWidget(startpause_button_);
	timerbutton_row_->addWidget(stop_button_);

	optionbutton_row_ = new QHBoxLayout();
	pintotop_button_ = new QPushButton("Stay on Top");
	pintotop_button_->setFont(button_font);
	pintotop_button_->setToolTip("Keep this Window in Foreground");
	autopause_button_ = new QPushButton("Auto-Pause");
	autopause_button_->setFont(button_font);
	autopause_tooltip1_ = "min after locking the PC:\nPause the Timer and count these ";
	autopause_tooltip2_ = "min retroactively as a Pause";
	autopause_button_->setToolTip(settings_.getAutopauseThresholdMinString() + autopause_tooltip1_ + settings_.getAutopauseThresholdMinString() + autopause_tooltip2_);
	optionbutton_row_->addWidget(pintotop_button_);
	optionbutton_row_->addWidget(autopause_button_);

	bottombutton_row_ = new QHBoxLayout();
	mintotray_button_ = new QPushButton("Min to Tray");
	mintotray_button_->setFont(button_font);
	mintotray_button_->setToolTip("Minimize to Tray Icon now");
	show_history_button_ = new QPushButton("History..");
	show_history_button_->setFont(button_font);
	show_history_button_->setToolTip("Show History");
	bottombutton_row_->addWidget(mintotray_button_);
	bottombutton_row_->addWidget(show_history_button_);
}

void ContentWidget::applyStartupSettingsToGui()
{
	if (settings_.isAutopauseEnabled())
		toggleButtonColor(autopause_button_, button_hold_color_);

	if (settings_.isPinnedStartEnabled())
		toggleButtonColor(pintotop_button_, button_hold_color_);
}

void ContentWidget::pressedStartPauseButton()
{
	emit startPausePressed();
}

void ContentWidget::pressedStopButton()
{
	emit stopPressed();
}

void ContentWidget::pressedMinToTrayButton()
{
	emit minToTray();
}

void ContentWidget::pressedPinToTopButton()
{
	toggleButtonColor(pintotop_button_, button_hold_color_);
	settings_.setPinToTopState(!settings_.isPinnedStartEnabled());
	emit toggleAlwaysOnTop();
}

void ContentWidget::pressedAutoPauseButton()
{
	toggleButtonColor(autopause_button_, button_hold_color_);
	settings_.setAutopauseState(!settings_.isAutopauseEnabled());
	autopause_button_->setToolTip(settings_.getAutopauseThresholdMinString() + autopause_tooltip1_ + settings_.getAutopauseThresholdMinString() + autopause_tooltip2_);
}

void ContentWidget::pressedShowHistoryButton()
{
    HistoryDialog dlg(timetracker_, settings_, this);
    const QString reconciliationMessage = dlg.getLoadReconciliationMessage();
    if (!reconciliationMessage.isEmpty()) {
        emit historyLoadReconciliationAvailable(reconciliationMessage);
    }
    dlg.exec();
}

void ContentWidget::setActivityTimeTooltip(const QString &hours /* ="0.00" */)
{
	const QString tooltip_label = "That's " + hours + activity_time_tooltip_base_;
	activity_time_->setToolTip(tooltip_label);
}

void ContentWidget::setPauseTimeTooltip()
{
	const QString tooltip_label = "Last Pause ended at " + QTime::currentTime().toString("hh:mm") + " o'clock";
	pause_time_->setToolTip(tooltip_label);
}

void ContentWidget::resetPauseTimeTooltip()
{
	pause_time_->setToolTip("");
}

void ContentWidget::manageTooltipsForActivity(bool wasPaused)
{
	if (!wasPaused) {
		activity_time_tooltip_base_ = "h overall since " + QTime::currentTime().toString("HH:mm") + " o'clock";
		setActivityTimeTooltip();
		resetPauseTimeTooltip();
		starttime_value_->setText(QTime::currentTime().toString("HH:mm"));
	} else {
		setPauseTimeTooltip();
	}
}

void ContentWidget::onTransitioned(TimeTracker::Transition t)
{
	if (t.to == TimeTracker::Mode::Activity)
		setGUItoActivity(t.from == TimeTracker::Mode::Pause);
	else if (t.to == TimeTracker::Mode::Pause)
		setGUItoPause();
	else
		setGUItoStop();
}

void ContentWidget::setGUItoActivity(bool wasPaused)
{
	manageTooltipsForActivity(wasPaused);

	startpause_button_->setText("PAUSE");
	activity_time_->setStyleSheet("QLabel {color : green; }");
	pause_time_->setStyleSheet("QLabel { color : black; }");
}

void ContentWidget::setGUItoStop()
{
	startpause_button_->setText("START");
	activity_time_->setStyleSheet("QLabel {color : black; }");
	pause_time_->setStyleSheet("QLabel { color : black; }");
	starttime_value_->setText("--:--");
}

void ContentWidget::setGUItoPause()
{
	startpause_button_->setText("CONTINUE");
	activity_time_->setStyleSheet("QLabel {color : black; }");
	pause_time_->setStyleSheet("QLabel { color : green; }");
}

void ContentWidget::updateTimes()
{
	qint64 t_active = timetracker_.getActiveTime();
	qint64 t_pause = timetracker_.getPauseTime();
	pause_time_->setText(convMSecToTimeStr(t_pause));
	activity_time_->setText(convMSecToTimeStr(t_active));
	setActivityTimeTooltip(convTimeStrToDurationStr(convMSecToTimeStr(t_active)));
}

QString ContentWidget::getTooltip()
{
	if (timetracker_.isPaused())
		return QString("µTimer:  In Pause (Overall " + convMSecToTimeStr(timetracker_.getPauseTime()) + ")");
	else if (timetracker_.isActive()) {
		const QString timeStr = convMSecToTimeStr(timetracker_.getActiveTime());
		return QString("µTimer:  In Activity (Overall " + convTimeStrToDurationStr(timeStr) + "h / " + timeStr + ")");
	}
	else
		return QString("µTimer:  Timing inactive");
}

