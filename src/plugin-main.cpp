#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QString>

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
				g_dock->setStatus(
					QString::fromStdString(g_session->startWithReport(iso::StartedBy::Stream)));
			}
		}
		g_dock->refreshSources();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		// A session OBS's Record button started ends with that button, not with the stream.
		if (g_session->active() && g_session->startedBy() != iso::StartedBy::Record) {
			g_session->stop();
		}
		g_dock->refreshSources();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		g_dock->refreshSessionConfig();
		if (g_session->config().withRecording && !g_session->active()) {
			if (!g_session->isArmedAny()) {
				blog(LOG_WARNING,
				     "[iso-recorder] OBS started recording with no sources ticked, so nothing was recorded");
				g_dock->setStatus(QStringLiteral(
					"OBS started recording with nothing ticked, so nothing is being recorded."));
			} else {
				g_dock->setStatus(
					QString::fromStdString(g_session->startWithReport(iso::StartedBy::Record)));
			}
		}
		g_dock->refreshSources();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		if (g_session->active() && g_session->startedBy() == iso::StartedBy::Record) {
			g_session->stop();
		}
		g_dock->refreshSources();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_PAUSED:
	case OBS_FRONTEND_EVENT_RECORDING_UNPAUSED: {
		if (g_session->active() && g_session->startedBy() == iso::StartedBy::Record) {
			std::string err;
			if (!g_session->setPaused(event == OBS_FRONTEND_EVENT_RECORDING_PAUSED, &err)) {
				g_dock->setStatus(QString::fromStdString(err));
			}
		}
		g_dock->refreshSources();
		break;
	}
	case OBS_FRONTEND_EVENT_SCENE_CHANGED: {
		obs_source_t *scene = obs_frontend_get_current_scene();
		g_session->onSceneChanged(scene);
		if (scene)
			obs_source_release(scene);
		break;
	}
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		// The old collection's sources are removed next, which would close every file
		// as aborted. This is the last moment they are whole, so finish them here.
		if (g_session->active()) {
			g_session->stop(true);
			blog(LOG_INFO, "[iso-recorder] stopped recording: the scene collection is changing");
			g_dock->setStatus(QStringLiteral(
				"Stopped: the scene collection changed. Press Record to carry on in the same folder."));
		}
		break;
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
		// On macOS OBS never destroys the dock and unload's frontend calls do nothing,
		// so the dock lets go of its sources here, while they still exist.
		g_dock->shutdown();
		break;
	default:
		break;
	}
}

bool obs_module_load(void)
{
	g_session = new iso::IsoSession();
	g_dock = new iso::IsoDock(g_session);
	g_dockOwnedByOBS = obs_frontend_add_dock_by_id("iso-recorder-dock", "ISO Recorder", g_dock);
	if (!g_dockOwnedByOBS)
		blog(LOG_WARNING, "[iso-recorder] dock id already in use; the dock was not added");
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
	blog(LOG_INFO, "[iso-recorder] loaded (v" ISO_VERSION ")");
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
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
