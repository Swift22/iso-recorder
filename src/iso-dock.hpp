#pragma once

#include <QWidget>

#include <vector>

#include "iso-session.hpp"

class QListWidget;
class QListWidgetItem;
class QComboBox;
class QCheckBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QTimer;

namespace iso {

class IsoDock : public QWidget {
	Q_OBJECT
public:
	explicit IsoDock(IsoSession *session, QWidget *parent = nullptr);
	~IsoDock() override;
	void refreshSources();
	void setStatus(const QString &text);
	void saveSettings();

signals:
	void sourceAdded(obs_source_t *source);
	void sourceRemoved(obs_source_t *source);

private slots:
	void onRecordClicked();
	void onNewGameClicked();
	void onItemChanged(QListWidgetItem *item);
	void onBrowse();
	void onTick();

private:
	void addSource(QListWidget *list, obs_source_t *source, Kind kind);
	void applySourceState(QListWidgetItem *item, obs_source_t *source, Kind kind);
	void restoreArms(const std::vector<obs_source_t *> &pictureSources,
			 const std::vector<obs_source_t *> &soundSources);
	void rememberArmed(const std::string &name, bool armed);
	SessionConfig configFromWidgets() const;
	SessionConfig sessionConfigFromWidgets() const;
	void applyConfig(const SessionConfig &cfg);
	void onConfigChanged();
	void releaseHeld();

	IsoSession *session_;
	QListWidget *picture_ = nullptr;
	QListWidget *sound_ = nullptr;
	QLineEdit *path_ = nullptr;
	QPushButton *browse_ = nullptr;
	QComboBox *encoder_ = nullptr;
	QCheckBox *withStream_ = nullptr;
	QCheckBox *recordScene_ = nullptr;
	QPushButton *record_ = nullptr;
	QPushButton *newGame_ = nullptr;
	QLabel *recording_ = nullptr; // the red "Recording · 00:04:32" line
	QLabel *status_ = nullptr;
	QTimer *timer_ = nullptr;
	std::vector<obs_source_t *> held_; // strong refs backing the listed raw pointers
	std::vector<obs_source_t *> lastPicture_;
	std::vector<obs_source_t *> lastSound_;
	std::vector<std::string> armed_; // the ticked set, remembered per collection
	std::string armedCollection_;
	bool updating_ = false;
	bool dirty_ = false;
	bool transientStatus_ = false;
};

} // namespace iso
