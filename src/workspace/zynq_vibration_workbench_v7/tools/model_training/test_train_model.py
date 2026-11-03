"""Synthetic fixtures test software only; no machine accuracy is asserted."""
import copy
import hashlib
import unittest
import numpy as np
import train_model as t


def fixtures():
    rng = np.random.default_rng(8)
    rows = []
    for label_index, label in enumerate(t.LABELS):
        for group in range(4):
            for win in range(6):
                features = np.ones(36) + rng.uniform(0, .05, 36)
                features[label_index] += 3
                rows.append(dict(feature_schema="lowband36.v1", quality="valid", synthetic=False,
                                 setup_id="software_fixture_only", configuration="1234abcd", fs=50000,
                                 rpm=1500, rpm_min=1499, rpm_max=1501, label=label,
                                 group_id=f"fixture_{label}_{group}", run_id=f"fixture_{label}_{group}",
                                 capture_sha256=hashlib.sha256(f"fixture_{label}_{group}".encode()).hexdigest(),
                                 raw_window_sha256=hashlib.sha256(f"fixture_{label}_{group}_{win}".encode()).hexdigest(),
                                 source_session_id="",
                                 first_index=win*83000, last_index=(win+1)*83000-1,
                                 features=features.tolist()))
    return rows


class TrainingTests(unittest.TestCase):
    def test_group_disjoint_and_real_guard(self):
        rows = fixtures()
        model, report = t.train(rows, "fixture_only", iterations=150)
        self.assertFalse(set(report["training_groups"]) & set(report["heldout_groups"]))
        self.assertEqual(len(model["weights"]), 144)
        self.assertEqual(model["status"], "provisional")
        self.assertFalse(report["mechanical_validation_complete"])
        rows[0]["synthetic"] = True
        with self.assertRaisesRegex(ValueError, "real windows"):
            t.validate_rows(rows)

    def test_no_holdout_scaler_leakage(self):
        rows = fixtures()
        train, test = t.split_groups(rows)
        first = t.fit(rows, train, iterations=100)
        changed = copy.deepcopy(rows)
        for idx in test:
            changed[int(idx)]["features"] = [v*1000 for v in changed[int(idx)]["features"]]
        second = t.fit(changed, train, iterations=100)
        for a, b in zip(first, second):
            np.testing.assert_array_equal(a, b)

    def test_unsafe_data_rejected(self):
        for field, value, error in (("label", "unlabeled", "label required"),
                                    ("quality", "invalid", "quality-valid"),
                                    ("configuration", "bad", "configuration"),
                                    ("rpm", 0, "speed interval")):
            rows = fixtures(); rows[0][field] = value
            with self.assertRaisesRegex(ValueError, error):
                t.validate_rows(rows)
        rows = fixtures(); rows[0]["features"][0] = float("nan")
        with self.assertRaisesRegex(ValueError, "finite"):
            t.validate_rows(rows)
        rows = fixtures(); rows[1]["first_index"] = 0
        with self.assertRaisesRegex(ValueError, "overlapping"):
            t.validate_rows(rows)
        rows = fixtures(); rows[1]["group_id"] = "wronggroup"
        with self.assertRaisesRegex(ValueError, "crosses independent groups"):
            t.validate_rows(rows)

    def test_relabeling_cannot_move_raw_content_across_split(self):
        rows = fixtures(); clone = copy.deepcopy(rows[0])
        clone["run_id"] = "different_run"; clone["group_id"] = "different_group"
        rows.append(clone)
        with self.assertRaisesRegex(ValueError, "duplicate raw window content"):
            t.validate_rows(rows)
        rows = fixtures(); rows[1]["group_id"] = "renamed"; rows[1]["run_id"] = "renamed"
        with self.assertRaisesRegex(ValueError, "physical source crosses"):
            t.validate_rows(rows)
        rows = fixtures()
        for row in rows[:12]:
            row["source_session_id"] = "1"*32
        with self.assertRaisesRegex(ValueError, "physical source crosses"):
            t.validate_rows(rows)

    def test_heldout_rejection_is_not_hidden(self):
        rows = fixtures()
        model, report = t.train(rows, "fixture_only", iterations=150)
        shifted = copy.deepcopy(rows)
        for idx in report["heldout_rows"]:
            shifted[idx]["rpm"] = shifted[idx]["rpm_min"] = shifted[idx]["rpm_max"] = 2000
        result = t.evaluate(shifted, report["heldout_rows"], model)
        self.assertEqual(result["coverage"], 0)
        self.assertIsNone(result["accuracy_on_accepted_only"])
        self.assertEqual(result["macro_f1_including_rejections"], 0)


if __name__ == "__main__":
    unittest.main()
