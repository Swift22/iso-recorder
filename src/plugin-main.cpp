#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QString>
#include <QWidget>

#include <string>

#include "iso-dock.hpp"
#include "iso-session.hpp"

OBS_DECLARE_MODULE()
MODULE_EXPORT const char *obs_module_description(void)
{
	return "ISO Recorder — one file per source while streaming";
}

static iso::IsoSession *g_session = nullptr;
static iso::IsoDock *g_dock = nullptr;
static bool g_dockOwnedByOBS = false;

static void onFrontendEvent(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		// There is no streaming output at module load, so "same as the stream" resolved
		// then is only a guess. Resolve it now that the real encoder exists.
		g_dock->refreshSessionConfig();
		if (g_session->config().withStream && !g_session->active()) {
			if (!g_session->isArmedAny()) {
				// Saying nothing here is how a whole stream goes by with no files.
				blog(LOG_WARNING,
				     "[iso-recorder] the stream started with no sources ticked, so nothing was recorded");
				g_dock->setStatus(QStringLiteral(
					"The stream started with nothing ticked, so nothing is being recorded."));
			} else {
				const iso::SessionConfig &cfg = g_session->config();
				std::string why;
				std::string err;
				if (!g_session->preflight(cfg, &why)) {
					g_dock->setStatus(QString::fromStdString(why));
				} else if (!g_session->start(cfg, &err)) {
					if (g_session->active())
						g_dock->setStatus(
							QStringLiteral("Recording, but some sources failed: ") +
							QString::fromStdString(err));
					else
						g_dock->setStatus(QString::fromStdString(err));
				} else if (g_session->armedVisualCount() > iso::kEncoderWarnThreshold) {
					g_dock->setStatus(QStringLiteral(
						"Recording anyway, but this is more videos than this machine is expected to handle."));
				}
			}
		}
		g_dock->refreshSources();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		if (g_session->active())
			g_session->stop();
		g_dock->refreshSources();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED: {
		obs_source_t *scene = obs_frontend_get_current_scene();
		g_session->onSceneChanged(scene);
		if (scene)
			obs_source_release(scene);
		break;
	}
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		g_dock->refreshSources();
		break;
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
		// OBS destroys every scene and source in ClearSceneData() before it fires
		// EXIT, and a mov_output finishes only when one more encoded packet
		// reaches it, so a stop started from EXIT can never finalise. This event
		// is the last one that runs while the sources and the video graph are
		// still whole, which is where a recording has to be closed.
		if (g_session->active())
			g_session->stop();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		if (g_session->active())
			g_session->stop();
		g_dock->saveSettings();
		break;
	default:
		break;
	}
}

struct SourceRemoval {
	std::string uuid;
};

static void onSourceRemovedTask(void *param)
{
	auto *job = static_cast<SourceRemoval *>(param);
	if (g_session) {
		obs_source_t *source = obs_get_source_by_uuid(job->uuid.c_str());
		if (source) {
			g_session->onSourceRemoved(source);
			obs_source_release(source);
		}
	}
	delete job;
}

// source_remove can arrive off the main thread, while the stop path touches outputs and
// encoders; hand only the uuid across and do the real work on the UI thread.
static void onSourceRemoveSignal(void *, calldata_t *calldata)
{
	void *ptr = nullptr;
	calldata_get_ptr(calldata, "source", &ptr);
	auto *source = static_cast<obs_source_t *>(ptr);
	if (!source)
		return;
	auto *job = new SourceRemoval{obs_source_get_uuid(source)};
	obs_queue_task(OBS_TASK_UI, onSourceRemovedTask, job, false);
}

bool obs_module_load(void)
{
	g_session = new iso::IsoSession();
	g_dock = new iso::IsoDock(g_session);
	g_dockOwnedByOBS = obs_frontend_add_dock_by_id("iso-recorder-dock", "ISO Recorder", g_dock);
	if (!g_dockOwnedByOBS)
		blog(LOG_WARNING, "[iso-recorder] dock id already in use; the dock was not added");
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
	signal_handler_connect(obs_get_signal_handler(), "source_remove", onSourceRemoveSignal,
			       nullptr);
	blog(LOG_INFO, "[iso-recorder] loaded (v" ISO_VERSION ")");
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	signal_handler_disconnect(obs_get_signal_handler(), "source_remove", onSourceRemoveSignal,
				  nullptr);
	if (g_dockOwnedByOBS)
		obs_frontend_remove_dock("iso-recorder-dock");
	else
		delete g_dock;
	g_dock = nullptr;
	g_dockOwnedByOBS = false;
	delete g_session;
	g_session = nullptr;
	blog(LOG_INFO, "[iso-recorder] unloaded");
}
