#include "session-writer.hpp"

#include <cstdio>
#include <string>

using namespace iso;

static int failures = 0;

#define CHECK(cond)                                                                      \
	do {                                                                             \
		if (!(cond)) {                                                           \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
			++failures;                                                      \
		}                                                                        \
	} while (0)

#define CHECK_EQ(got, want)                                                              \
	do {                                                                             \
		std::string _g = (got);                                                      \
		std::string _w = (want);                                                     \
		if (_g != _w) {                                                              \
			std::printf("FAIL %s:%d: %s\n  got:  %s\n  want: %s\n", __FILE__,    \
				    __LINE__, #got, _g.c_str(), _w.c_str());                \
			++failures;                                                          \
		}                                                                        \
	} while (0)

int main()
{
	CHECK_EQ(sanitizeName("Game Capture"), "Game_Capture");
	CHECK_EQ(sanitizeName("Discord/voice"), "Discord_voice");
	CHECK_EQ(sanitizeName("  spaced  "), "spaced");
	CHECK_EQ(sanitizeName("???"), "source");

	CHECK_EQ(makeFilename(Kind::Video, 1, "game", 1), "01_game.mov");
	CHECK_EQ(makeFilename(Kind::Video, 2, "veado", 1), "02_veado.mov");
	CHECK_EQ(makeFilename(Kind::Audio, 3, "Mic", 1), "03_Mic.wav");
	CHECK_EQ(makeFilename(Kind::Audio, 3, "Mic", 2), "03_Mic_2.wav");
	CHECK_EQ(makeFilename(Kind::Audio, 3, "Mic", 3), "03_Mic_3.wav");

	CHECK_EQ(formatClock(0.0), "0:00.0");
	CHECK_EQ(formatClock(10.5), "0:10.5");
	CHECK_EQ(formatClock(65.0), "1:05.0");
	CHECK_EQ(formatClock(4210.5), "1:10:10.5");
	CHECK_EQ(formatClock(3599.9), "59:59.9");

	CHECK_EQ(formatSeconds(0.0), "0.0");
	CHECK_EQ(formatSeconds(8123.4), "8123.4");
	CHECK_EQ(formatSeconds(0.017), "0.017");

	CHECK_EQ(codecForEncoderId("obs_x264"), "h264");
	CHECK_EQ(codecForEncoderId("apple_h264"), "h264");
	CHECK_EQ(codecForEncoderId("jim_nvenc"), "h264");
	CHECK_EQ(codecForEncoderId("obs_hevc"), "hevc");
	CHECK_EQ(codecForEncoderId("hevc_texture_amf"), "hevc");
	CHECK_EQ(codecForEncoderId("h265_texture_amf"), "hevc");
	CHECK_EQ(codecForEncoderId("av1_nvenc"), "av1");
	CHECK_EQ(codecForEncoderId("prores_ks"), "prores");

	SessionInfo info;
	info.started = "2026-09-12T14:32:05+03:00";
	info.ended = "2026-09-12T16:47:28+03:00";
	info.duration = 8123.4;
	info.obsVersion = "32.0.1";
	info.platform = "macos";
	info.video = {1920, 1080, 60.0};

	std::vector<Recording> recs;
	recs.push_back({"game", Kind::Video, "01_game.mov", "h264", 0.0, 8123.4,
			Status::Complete, "", "the game, on its own"});
	recs.push_back({"ofes", Kind::Video, "", "", std::nullopt, std::nullopt,
			Status::Failed, "encoder refused: too many sessions", "ofes"});

	const std::string json = buildSessionJson(info, recs);
	const std::string want =
		"{\n"
		"  \"version\": 1,\n"
		"  \"session\": {\n"
		"    \"started\": \"2026-09-12T14:32:05+03:00\",\n"
		"    \"ended\": \"2026-09-12T16:47:28+03:00\",\n"
		"    \"duration\": 8123.4,\n"
		"    \"obs\": \"32.0.1\",\n"
		"    \"platform\": \"macos\",\n"
		"    \"video\": { \"width\": 1920, \"height\": 1080, \"fps\": 60 }\n"
		"  },\n"
		"  \"recordings\": [\n"
		"    { \"source\": \"game\", \"kind\": \"video\", \"file\": "
		"\"01_game.mov\", \"folder\": null, \"codec\": \"h264\", "
		"\"startOffset\": 0.0, "
		"\"duration\": 8123.4, \"status\": \"complete\" },\n"
		"    { \"source\": \"ofes\", \"kind\": \"video\", \"file\": null, "
		"\"folder\": null, "
		"\"codec\": null, \"startOffset\": null, \"duration\": null, "
		"\"status\": \"failed\", \"error\": \"encoder refused: too many sessions\" }\n"
		"  ]\n"
		"}\n";
	CHECK_EQ(json, want);

	const std::string readme = buildReadme(info, recs);
	CHECK(readme.find("  01_game.mov       0:00.0        the game, on its own") !=
	      std::string::npos);
	const size_t gamePos = readme.find("01_game.mov");
	const size_t ofesPos = readme.find("ofes");
	CHECK(gamePos != std::string::npos && ofesPos != std::string::npos && gamePos < ofesPos);
	CHECK(readme.find("not recorded") != std::string::npos);

	{
		std::vector<Recording> longRecs = recs;
		longRecs[0].file = "01_Desktop_Audio.wav";
		const std::string longReadme = buildReadme(info, longRecs);
		CHECK(longReadme.find("01_Desktop_Audio.wav  0:00.0") != std::string::npos);
	}

	{
		std::vector<Recording> splitRecs = recs;
		splitRecs[0].folder = "02_Game";
		const std::string splitJson = buildSessionJson(info, splitRecs);
		CHECK(splitJson.find("\"folder\": \"02_Game\"") != std::string::npos);
		const std::string splitReadme = buildReadme(info, splitRecs);
		CHECK(splitReadme.find("02_Game/01_game.mov") != std::string::npos);
	}

	if (failures == 0)
		std::printf("session-writer: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
