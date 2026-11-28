import sqlite3
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "app"))
from professional_release import readiness_checks, summarize_checks


class ProfessionalReleaseTests(unittest.TestCase):
    def test_readiness_is_read_only_and_reports_missing_optional_hub(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "data").mkdir()
            db = root / "employees.sqlite3"
            con = sqlite3.connect(db)
            con.execute("CREATE TABLE employees(id INTEGER PRIMARY KEY)")
            con.commit(); con.close()
            template = root / "template.card"
            template.write_text("{}", encoding="utf-8")
            before = db.read_bytes()
            checks = readiness_checks(app_data_dir=root / "data", template_path=template,
                                      db_path=db, hub_path="")
            self.assertEqual([x["status"] for x in checks], ["ok", "ok", "ok", "warning"])
            self.assertEqual(db.read_bytes(), before)

    def test_corrupt_database_is_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); (root / "data").mkdir()
            db = root / "bad.sqlite3"; db.write_bytes(b"not sqlite")
            checks = readiness_checks(app_data_dir=root / "data", template_path=root / "missing.card", db_path=db)
            self.assertEqual(next(x for x in checks if x["name"] == "База сотрудников")["status"], "error")

    def test_summary_counts_statuses(self):
        self.assertEqual(summarize_checks([{"status": "ok"}, {"status": "warning"}, {"status": "error"}]),
                         {"ok": 1, "warning": 1, "error": 1})


if __name__ == "__main__":
    unittest.main()
