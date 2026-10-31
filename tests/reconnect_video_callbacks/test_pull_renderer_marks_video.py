"""The FFmpeg decoder is a pull renderer, so moonlight-common-c never calls
Session::drSubmitDecodeUnit() for it. Its decoder thread must report every
decode unit it pulls, or m_HasReceivedVideo stays false and
clConnectionTerminated() never attempts an auto-reconnect."""

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
FFMPEG = ROOT / "app" / "streaming" / "video" / "ffmpeg.cpp"
SESSION_H = ROOT / "app" / "streaming" / "session.h"


def function_body(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}\n", start)]


class PullRendererMarksVideoTests(unittest.TestCase):
    def test_ffmpeg_decoder_is_a_pull_renderer(self):
        self.assertIn("capabilities |= CAPABILITY_PULL_RENDERER;", FFMPEG.read_text(encoding="utf-8"))

    def test_every_pulled_decode_unit_marks_video_received(self):
        body = function_body(FFMPEG.read_text(encoding="utf-8"),
                             "void FFmpegVideoDecoder::decoderThreadProc()")
        submits = [m.start() for m in re.finditer(r"submitDecodeUnit\(du\)", body)]
        self.assertGreater(len(submits), 0)
        for pos in submits:
            preceding = body[:pos]
            pulled = max(preceding.rfind("LiWaitForNextVideoFrame("),
                         preceding.rfind("LiPollNextVideoFrame("))
            self.assertGreaterEqual(pulled, 0)
            self.assertIn("Session::markVideoReceived();", preceding[pulled:],
                          "a pulled decode unit is submitted without marking video as received")

    def test_flag_is_safe_across_threads(self):
        self.assertRegex(SESSION_H.read_text(encoding="utf-8"),
                         r"std::atomic<bool>\s+m_HasReceivedVideo;")


if __name__ == "__main__":
    unittest.main()
