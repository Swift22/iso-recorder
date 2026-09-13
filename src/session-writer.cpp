#include "session-writer.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>

namespace iso {

const char *kindName(Kind kind) { return kind == Kind::Audio ? "audio" : "video"; }

const char *statusName(Status status)
{
	switch (status) {
	case Status::Complete: return "complete";
	case Status::Aborted: return "aborted";
	case Status::Failed: return "failed";
	case Status::Recording: break;
	}
	return "recording";
}

const char *extensionFor(Kind kind) { return kind == Kind::Audio ? "wav" : "mov"; }

std::string codecForEncoderId(const std::string &id)
{
	std::string lower;
	lower.reserve(id.size());
	for (unsigned char c : id)
		lower.push_back(static_cast<char>(std::tolower(c)));
	if (lower.find("prores") != std::string::npos)
		return "prores";
	if (lower.find("hevc") != std::string::npos || lower.find("265") != std::string::npos)
		return "hevc";
	if (lower.find("av1") != std::string::npos)
		return "av1";
	return "h264";
}

std::string sanitizeName(const std::string &source)
{
	std::string out;
	bool pending = false;
	for (unsigned char c : source) {
		if (std::isalnum(c) || c == '.' || c == '-') {
			out.push_back(static_cast<char>(c));
			pending = false;
		} else if (!out.empty()) {
			pending = true;
		}
		if (pending && !out.empty() && out.back() != '_')
			out.push_back('_');
	}
	while (!out.empty() && out.back() == '_')
		out.pop_back();
	return out.empty() ? "source" : out;
}

std::string makeFilename(Kind kind, int index, const std::string &source, int segment)
{
	char num[8];
	std::snprintf(num, sizeof num, "%02d", index);
	std::string name = std::string(num) + "_" + sanitizeName(source);
	if (segment > 1)
		name += "_" + std::to_string(segment);
	name += ".";
	name += extensionFor(kind);
	return name;
}

std::string formatSeconds(double seconds)
{
	char buf[32];
	std::snprintf(buf, sizeof buf, "%.4f", seconds);
	std::string t = buf;
	while (t.size() > 1 && t.back() == '0')
		t.pop_back();
	if (!t.empty() && t.back() == '.')
		t.push_back('0');
	return t;
}

std::string formatClock(double seconds)
{
	if (seconds < 0)
		seconds = 0;
	int total = static_cast<int>(std::floor(seconds));
	int tenths = static_cast<int>(std::llround((seconds - total) * 10.0));
	if (tenths == 10) {
		++total;
		tenths = 0;
	}
	const int h = total / 3600, m = (total % 3600) / 60, s = total % 60;
	char buf[32];
	if (h > 0)
		std::snprintf(buf, sizeof buf, "%d:%02d:%02d.%d", h, m, s, tenths);
	else
		std::snprintf(buf, sizeof buf, "%d:%02d.%d", m, s, tenths);
	return buf;
}

static void jsonString(std::string &o, const std::string &s)
{
	o.push_back('"');
	for (unsigned char c : s) {
		switch (c) {
		case '"': o += "\\\""; break;
		case '\\': o += "\\\\"; break;
		case '\n': o += "\\n"; break;
		case '\r': o += "\\r"; break;
		case '\t': o += "\\t"; break;
		default:
			if (c < 0x20) {
				char b[8];
				std::snprintf(b, sizeof b, "\\u%04x", c);
				o += b;
			} else {
				o.push_back(static_cast<char>(c));
			}
		}
	}
	o.push_back('"');
}

static void jsonNum(std::string &o, const std::optional<double> &v)
{
	o += v ? formatSeconds(*v) : "null";
}

static void jsonStr(std::string &o, const std::string &v)
{
	if (v.empty())
		o += "null";
	else
		jsonString(o, v);
}

std::string buildSessionJson(const SessionInfo &info, const std::vector<Recording> &recs)
{
	std::string o;
	o += "{\n  \"version\": 1,\n  \"session\": {\n";
	o += "    \"started\": ";
	jsonString(o, info.started);
	o += ",\n    \"ended\": ";
	if (info.ended)
		jsonString(o, *info.ended);
	else
		o += "null";
	o += ",\n    \"duration\": ";
	jsonNum(o, info.duration);
	o += ",\n    \"obs\": ";
	jsonString(o, info.obsVersion);
	o += ",\n    \"platform\": ";
	jsonString(o, info.platform);
	char v[128];
	std::snprintf(v, sizeof v,
		      ",\n    \"video\": { \"width\": %d, \"height\": %d, \"fps\": %.0f }\n  },\n",
		      info.video.width, info.video.height, info.video.fps);
	o += v;
	o += "  \"recordings\": [\n";
	for (size_t i = 0; i < recs.size(); ++i) {
		const Recording &r = recs[i];
		o += "    { \"source\": ";
		jsonString(o, r.source);
		o += ", \"kind\": ";
		jsonString(o, kindName(r.kind));
		o += ", \"file\": ";
		jsonStr(o, r.file);
		o += ", \"folder\": ";
		jsonStr(o, r.folder);
		o += ", \"codec\": ";
		jsonStr(o, r.codec);
		o += ", \"startOffset\": ";
		jsonNum(o, r.startOffset);
		o += ", \"duration\": ";
		jsonNum(o, r.duration);
		o += ", \"status\": ";
		jsonString(o, statusName(r.status));
		if (r.status == Status::Failed) {
			o += ", \"error\": ";
			jsonString(o, r.error);
		}
		o += " }";
		o += (i + 1 < recs.size()) ? ",\n" : "\n";
	}
	o += "  ]\n}\n";
	return o;
}

// How a recording is shown in the README: the game folder and the file, or a dash
// when nothing was written.
static std::string readmePath(const Recording &r)
{
	if (r.file.empty())
		return "--";
	return r.folder.empty() ? r.file : r.folder + "/" + r.file;
}

std::string buildReadme(const SessionInfo &info, const std::vector<Recording> &recs)
{
	std::string o;
	o += "Separate recordings from the stream on " + info.dateLine + ".\n\n";
	o += "Every file is timed from the same clock — the moment the recording session\n";
	o += "began. Most start right at the beginning; a track switched on later starts\n";
	o += "later, and its start time is listed below. Place each clip at its start time\n";
	o += "and they all line up.\n\n";
	size_t fileWidth = 18;
	for (const Recording &r : recs) {
		const size_t len = readmePath(r).size() + 2;
		if (len > fileWidth)
			fileWidth = len;
	}
	for (const Recording &r : recs) {
		const std::string file = readmePath(r);
		const std::string clock = r.startOffset ? formatClock(*r.startOffset) : "--";
		std::string desc = r.label.empty() ? r.source : r.label;
		if (r.status == Status::Failed)
			desc += " (not recorded: " + r.error + ")";
		char line[512];
		std::snprintf(line, sizeof line, "  %-*s%-14s%s\n", (int)fileWidth, file.c_str(),
			      clock.c_str(), desc.c_str());
		o += line;
	}
	o += "\nNothing is cut, mixed, or ducked — the tracks are each source exactly as\n";
	o += "OBS saw it. session.json has the exact start of every file if you ever\n";
	o += "need to nudge one.\n";
	return o;
}

} // namespace iso
