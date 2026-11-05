"""Compile the production Session methods against fake time, window and transport."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SESSION = ROOT / "app" / "streaming" / "session.cpp"


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


class ResolutionMatchTests(unittest.TestCase):
    def test_production_session_lifecycle(self):
        source = SESSION.read_text(encoding="utf-8")
        header = SESSION.with_suffix(".h").read_text(encoding="utf-8")
        state_start = header.index("    struct ResolutionMatchState")
        state_end = header.index("    } m_ResolutionMatch;", state_start)
        state = header[state_start:state_end + len("    } m_ResolutionMatch;")]
        methods = "\n".join(function(source, signature) for signature in (
            "static bool isHostStreamingDisplay1To1(",
            "void Session::scheduleWindowResolutionMatch(",
            "void Session::processWindowResolutionMatch(",
            "void Session::handleHostResolutionReport(",
        ))
        harness = Path(__file__).with_name("harness.cpp").read_text(encoding="utf-8")
        harness = harness.replace("// RESOLUTION_STATE", state)
        harness = harness.replace("// SESSION_METHODS", methods)
        compiler = shutil.which("cl" if os.name == "nt" else "c++")
        self.assertIsNotNone(compiler, "Run from a C++ compiler environment")
        with tempfile.TemporaryDirectory(prefix="moonlight-resolution-") as directory:
            path = Path(directory)
            unit = path / "resolution.cpp"
            unit.write_text(harness, encoding="utf-8")
            executable = path / ("resolution.exe" if os.name == "nt" else "resolution")
            if os.name == "nt":
                command = [compiler, "/nologo", "/std:c++17", "/EHsc", str(unit),
                           f"/Fe:{executable}"]
            else:
                command = [compiler, "-std=c++17", str(unit), "-o", str(executable)]
            build = subprocess.run(command, cwd=path, capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_user_event_codes_are_unique(self):
        entries = re.findall(r"^#define (SDL_CODE_\w+) (\d+)$",
                             SESSION.read_text(encoding="utf-8"), re.MULTILINE)
        values = {}
        for name, value in entries:
            self.assertNotIn(value, values, f"{name} collides with {values.get(value)}")
            values[value] = name

    def test_input_dimension_setter_is_not_duplicated(self):
        source = (ROOT / "app/streaming/input/input.cpp").read_text(encoding="utf-8")
        self.assertEqual(source.count("void SdlInputHandler::setStreamDimensions("), 1)

    def test_preference_defaults_off(self):
        source = (ROOT / "app/settings/streamingpreferences.cpp").read_text(encoding="utf-8")
        self.assertRegex(source, r"resolutionMatchWindow\s*=\s*settings\.value\("
                                 r"SER_RESOLUTION_MATCH_WINDOW,\s*false\)\.toBool\(\)")

    def test_each_connection_resets_resolution_state_before_reading_capabilities(self):
        body = function(SESSION.read_text(encoding="utf-8"),
                        "bool Session::startConnectionAsync(")
        self.assertIn("m_ResolutionMatch = {}", body)
        self.assertLess(body.index("LiStartConnection("),
                        body.index("m_ResolutionMatch = {}"))
        self.assertLess(body.index("m_ResolutionMatch = {}"),
                        body.index("LiGetHostFeatureFlags()"))


if __name__ == "__main__":
    unittest.main()
