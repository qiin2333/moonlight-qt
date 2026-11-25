# PyroWave preferences

This standalone Qt test exercises the real `StreamingPreferences` class with
an isolated temporary settings directory. It checks the default, QML property
notification, persistence, build availability and color override compatibility.
It does not require a PyroWave library, GPU or streaming host.

From an empty build directory, run:

```sh
qmake /path/to/moonlight-qt/tests/pyrowave_preferences/pyrowave_preferences.pro
make && ./pyrowave_preferences
```

Repeat in a separate directory with `CONFIG+=pyrowave-test-enabled` appended to
the `qmake` command to test the build with PyroWave support advertised.
