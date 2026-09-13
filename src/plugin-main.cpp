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
		if (g_session->config().withStream && g_session->isArmedAny() &&
		    !g_session->active()) {
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
	case OBS_FRONTEND_EVENT_EXIT:
		if (g_session->active())
			g_session->stop();
		g_dock->saveSettings();
		break;
	default:
		break;
	}
}

static void onSourceRemoveSignal(void *, calldata_t *calldata)
{
	void *ptr = nullptr;
	calldata_get_ptr(calldata, "source", &ptr);
	auto *source = static_cast<obs_source_t *>(ptr);
	if (source)
		g_session->onSourceRemoved(source);
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
	blog(LOG_INFO, "[iso-recorder] loaded");
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
