"""Host tests for the data tools (stdlib only).

Run from the repository root:  python3 -m unittest discover -s tests/tools -v

They check the published datasets in data/ against the one raw recording that
is also published (logs/csv_hand_straight_normal_195036.log): re-running the
aggregation on it must reproduce that recording's rows exactly.
"""
import contextlib
import csv
import importlib.util
import io
import os
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(REPO, "data")
LOG_NAME = "csv_hand_straight_normal_195036.log"


def load_tool(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(REPO, "tools", name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def read_csv(path):
    with open(path, newline="") as fh:
        return list(csv.reader(fh))


class AggregateTrainingData(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tool = load_tool("aggregate_training_data")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.out = os.path.join(cls.tmp.name, "out.csv")
        cls.report = io.StringIO()
        with contextlib.redirect_stdout(cls.report):
            cls.tool.main(["--logdir", os.path.join(REPO, "logs"), "--out", cls.out])
        cls.rows = read_csv(cls.out)
        cls.published = read_csv(os.path.join(DATA, "training_data_20260918.csv"))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_header_is_provenance_plus_17_features_plus_label(self):
        header = self.rows[0]
        self.assertEqual(header[:4], ["source_file", "surface", "angle", "speed"])
        self.assertEqual(header[4:-1], self.tool.FEATURE_COLS)
        self.assertEqual(len(self.tool.FEATURE_COLS), 17)
        self.assertEqual(header[-1], "label")
        self.assertEqual(header, self.published[0])

    def test_reproduces_published_rows_for_the_published_recording(self):
        want = [r for r in self.published[1:] if r[0] == LOG_NAME]
        got = self.rows[1:]
        self.assertEqual(len(want), 996)
        self.assertEqual(got, want)

    def test_only_fresh_rows_are_kept(self):
        # 1,216 [CSV] rows in the capture, 996 with both validity fields FRESH (165 = 0xA5)
        self.assertIn("Total raw [CSV] rows scanned:  1216", self.report.getvalue())
        self.assertIn("Total FRESH rows kept:         996", self.report.getvalue())

    def test_filename_parsing(self):
        self.assertEqual(self.tool.parse_filename(LOG_NAME), ("hand", "straight", "normal"))
        self.assertEqual(self.tool.parse_filename("notes.txt"), (None, None, None))

    def test_refuses_to_overwrite_without_force(self):
        with self.assertRaises(SystemExit) as ctx:
            self.tool.main(["--logdir", os.path.join(REPO, "logs"), "--out", self.out])
        self.assertIn("--force", str(ctx.exception))

    def test_empty_log_directory_does_not_crash(self):
        with tempfile.TemporaryDirectory() as empty:
            with contextlib.redirect_stdout(io.StringIO()):
                self.tool.main(["--logdir", empty, "--out", os.path.join(empty, "o.csv")])
            self.assertEqual(len(read_csv(os.path.join(empty, "o.csv"))), 1)


class PublishedDatasets(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.full = read_csv(os.path.join(DATA, "training_data_20260918.csv"))
        cls.train = read_csv(os.path.join(DATA, "ei_upload_training_20260918.csv"))
        cls.held = read_csv(os.path.join(DATA, "held_out_test_vectors_20260918.csv"))

    def test_row_counts_match_model_readme(self):
        self.assertEqual(len(self.full) - 1, 37216)
        self.assertEqual(len(self.held) - 1, 3721)
        self.assertEqual(len(self.train) - 1 + len(self.held) - 1, len(self.full) - 1)
        self.assertEqual(len({r[0] for r in self.full[1:]}), 60)  # recordings

    def test_features_are_in_firmware_normalised_range(self):
        for row in self.held[1:]:
            feats = [int(x) for x in row[4:21]]
            self.assertTrue(all(-1000 <= x <= 1000 for x in feats), row)
            self.assertIn(row[21], ("0", "1"))

    def test_held_out_rows_come_from_the_aggregate(self):
        full = {tuple(r) for r in self.full[1:]}
        self.assertTrue(all(tuple(r) in full for r in self.held[1:]))

    def test_held_out_is_a_row_level_split_of_the_same_recordings(self):
        # Documents a limitation rather than a virtue: every held-out row
        # comes from a recording that also contributes training rows, so
        # consecutive frames (which share 9 of their 10 distance samples)
        # sit on both sides of the split. See README "Limitations".
        self.assertTrue({r[0] for r in self.held[1:]} <= {r[0] for r in self.full[1:]})
        self.assertEqual(sum(r[0] == LOG_NAME for r in self.held[1:]), 100)


class AnalyzeLiveCapture(unittest.TestCase):
    def test_npudiag_line_format_matches_firmware(self):
        tool = load_tool("analyze_live_capture")
        m = tool.LINE_RE.search("[NPUDIAG] seq=150 match cpu=0 npu=0 pre=-12 postq=-128 poste=-97")
        self.assertIsNotNone(m)
        self.assertEqual((m["seq"], m["status"], m["pre"], m["poste"]), ("150", "match", "-12", "-97"))
        m = tool.LINE_RE.search("[NPUDIAG] seq=7 MISMATCH cpu=1 npu=0 pre=40 postq=3 poste=-90")
        self.assertEqual(m["status"], "MISMATCH")
        self.assertIsNone(tool.LINE_RE.search("[HB] up_ms=1 frames=2 inf=2 hazard=0"))


if __name__ == "__main__":
    unittest.main()
