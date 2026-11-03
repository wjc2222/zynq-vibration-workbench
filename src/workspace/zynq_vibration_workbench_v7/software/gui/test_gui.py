#!/usr/bin/env python3
"""Real Qt widget state regression checks; no hardware acceptance is implied."""
import argparse
import csv
import io
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True


def assert_four_plot_geometry(state):
    plots = state["four_plot_geometry"]
    assert [plot["name"] for plot in plots] == ["waveCH1Plot", "waveCH2Plot", "waveCH3Plot", "waveformPlot"], plots
    for plot in plots:
        assert plot["visible"] and plot["width"] >= 250 and plot["height"] >= 90, plots
        assert plot["plot_area_width"] > 150 and plot["plot_area_height"] >= 40, plots
        assert 0 <= plot["x"] and plot["x"] + plot["width"] <= state["width"], plots
        assert 0 <= plot["y"] and plot["y"] + plot["height"] <= state["height"], plots
    for i, a in enumerate(plots):
        for b in plots[i+1:]:
            assert (a["x"] + a["width"] <= b["x"] or b["x"] + b["width"] <= a["x"]
                    or a["y"] + a["height"] <= b["y"] or b["y"] + b["height"] <= a["y"]), plots
    assert plots[0]["y"] == plots[1]["y"] < plots[2]["y"] == plots[3]["y"], plots
    assert plots[0]["x"] == plots[2]["x"] < plots[1]["x"] == plots[3]["x"], plots
    assert [plot["x_axis_visible"] for plot in plots] == [False, False, True, True], plots


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    parser.add_argument("--application", required=True)
    parser.add_argument("--generator", required=True)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("generator", args.generator)
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    with tempfile.TemporaryDirectory(prefix="vibration_gui_test_") as temp:
        root = Path(temp)
        normal = root / "合成 样本.BIN"
        generator.generate(normal, seconds=0.2, header_ppr=1, noise_std_g=0)
        missing = root / "missing_speed.BIN"
        generator.generate(missing, seconds=0.2, header_ppr=1, missing_speed=True, noise_std_g=0)
        unknown = root / "unknown.BIN"
        unknown.write_bytes(normal.read_bytes())
        malformed = root / "malformed_sidecar.BIN"
        malformed.write_bytes(normal.read_bytes())
        malformed.with_suffix(".meta.json").write_text('{"synthetic":true,"synthetic":false}', encoding="utf-8")
        bad = root / "bad.BIN"
        bad.write_bytes(b"not a capture")
        startup = root / "startup_unknown_ppr.BIN"
        generator.generate(startup, seconds=0.2, header_ppr=0, noise_std_g=0)
        originals = {path: path.read_bytes() for path in root.iterdir() if path.is_file()}
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
        directory = root / "directory_fixture"
        directory.mkdir()
        renamed = directory / "renamed-record.BIN"
        renamed.write_bytes(normal.read_bytes())
        crc_bad = directory / "payload_crc_bad.bin"
        corrupted = bytearray(normal.read_bytes())
        corrupted[4096 + 16] ^= 1
        crc_bad.write_bytes(corrupted)
        boot = directory / "BOOT.bin"
        boot.write_bytes(b"not a capture boot image")
        (directory / "short.BIN").write_bytes(b"CAPDDR2")
        (directory / "wrong_terminator.BIN").write_bytes(b"CAPDDR2X")
        (directory / "ignored.txt").write_bytes(normal.read_bytes())
        nested = directory / "nested"
        nested.mkdir()
        (nested / "not_listed.BIN").write_bytes(normal.read_bytes())
        empty = root / "empty_directory"
        empty.mkdir()
        absent = root / "absent_directory"
        directory_originals = {path: path.read_bytes() for path in directory.rglob("*") if path.is_file()}
        directories = subprocess.run([args.probe, "--directory", str(directory), "--directory", str(empty),
                                      "--directory", str(absent), "--directory", str(normal)],
                                     capture_output=True, text=True, encoding="utf-8", env=env, timeout=30)
        assert directories.returncode == 0, directories.stderr
        directory_states = json.loads(directories.stdout)
        assert len(directory_states) == 4, directory_states
        assert directory_states[0]["listed_paths"] == [str(crc_bad), str(renamed)], directory_states
        assert directory_states[1]["listed_text"] == ["此目录没有采集记录"], directory_states
        assert "数据目录不存在" in directory_states[2]["listed_text"][0], directory_states
        assert "不是目录" in directory_states[3]["listed_text"][0], directory_states
        assert all(not state["is_analyzing"] and not state["export_enabled"] and state["demo_enabled"]
                   and state["banner"] == "未打开文件" for state in directory_states), directory_states
        directory_run = subprocess.run([args.application, "--directory", str(directory), "--screenshot", str(root/"directory.png")],
                                       env=env, capture_output=True, text=True, encoding="utf-8", timeout=30)
        absent_run = subprocess.run([args.application, "--directory", str(absent), "--screenshot", str(root/"absent.png")],
                                    env=env, capture_output=True, text=True, encoding="utf-8", timeout=30)
        assert directory_run.returncode == 0 and (root/"directory.png").is_file(), directory_run.stderr
        assert absent_run.returncode == 0 and (root/"absent.png").is_file(), absent_run.stderr
        assert all(path.read_bytes() == before for path, before in directory_originals.items()), "Directory listing changed files"
        identified = subprocess.run([args.probe, str(renamed), str(crc_bad), str(boot)],
                                    capture_output=True, text=True, encoding="utf-8", env=env, timeout=30)
        assert identified.returncode == 0, identified.stderr
        identified_states = json.loads(identified.stdout)
        assert identified_states[0]["export_enabled"], identified_states
        assert identified_states[1]["banner"] == "文件无效" and not identified_states[1]["export_enabled"], identified_states
        assert "Payload CRC mismatch" in identified_states[1]["evidence"], identified_states
        assert identified_states[2]["banner"] == "文件无效" and not identified_states[2]["export_enabled"], identified_states
        assert all(path.read_bytes() == before for path, before in directory_originals.items()), "Opening changed directory fixtures"
        process = subprocess.run([args.probe, *map(str, (normal, missing, unknown, malformed, bad))],
                                 capture_output=True, text=True, encoding="utf-8", env=env, timeout=120)
        assert process.returncode == 0, process.stderr
        states = json.loads(process.stdout)
        assert all(state["navigation_works"] for state in states), states
        assert all(state["four_plots_initially_cleared"] and state["four_plots_cleared_while_loading"] for state in states), states
        assert states[4]["four_plots_cleared_after_error"], states[4]
        for state in states[:4]:
            assert_four_plot_geometry(state)
            assert state["four_plots_hidden_overlay_cursor_sync"], state
            for key in ("exports_valid", "protected_exports_rejected", "symlink_export_rejected", "hardlink_export_rejected", "fft_cursor_exact", "raw_cursor_exact", "channel_toggle_changes_plot", "range_keeps_analysis", "raw_drag_cursor_exact", "fft_drag_cursor_exact", "same_range_avoids_preview_scan", "range_a_b_a_reuses_preview", "exploration_touch_targets", "zoom_pan_boundaries", "channel_full_touch_rect", "four_plots_correct_series", "four_plots_one_preview_per_range", "four_plots_range_shared", "four_plots_raw_cursor_sync_exact", "four_plots_drag_cursor_sync_exact", "four_plots_navigation_range_sync", "four_plots_checkboxes_overlay_only"):
                assert state[key], (key, state)
            rows = list(csv.reader(io.StringIO(state["csv"])))
            assert rows[0] == ["field", "channel", "value", "unit"] and all(len(row) == 4 for row in rows)
            assert any(row[:3] == ["classification", "", "not_classified"] for row in rows)
            assert sum(row[0] == "ac_rms" for row in rows) == 3
        assert states[0]["banner"] == "合成演示", states[0]
        assert "2000.0 RPM" in states[0]["metrics"], states[0]
        assert "— RPM" in states[1]["metrics"] and "0.0%" in states[1]["metrics"], states[1]
        assert states[2]["banner"] == "来源待确认", states[2]
        assert "来源说明无效" in states[3]["evidence"] and states[3]["banner"] == "来源待确认", states[3]
        assert "文件无效" in states[4]["banner"] and "不可判定" in states[4]["diagnosis"], states[4]
        assert not states[4]["export_enabled"] and "— RPM" in states[4]["metrics"], states[4]
        assert "暂无特征" not in states[0]["features"] and "CH1" in states[0]["features"], states[0]
        assert all(state["minimum_button_height"] >= 44 for state in states), states
        assert all(state["details_opened"] and state["details_touch_targets"] for state in states), states
        assert all(state["calibration_visible"] for state in states[:4]) and not states[4]["calibration_visible"]
        assert "名义灵敏度" in states[0]["evidence"] and "分类器" in states[0]["evidence"], states[0]
        assert not states[4]["sample_info"].startswith("50"), states[4]
        assert all(state["export_enabled"] for state in states[:4])
        assert all(state["diagnosis"] == "未分类" for state in states[:4]), states
        assert all(state["width"] == 1024 and state["height"] == 600 for state in states), states
        assert all(path.read_bytes() == before for path, before in originals.items()), "GUI changed input files"
        for invalid in ("0", "-1", "nan", "inf", "abc", "1e-9", "1e10"):
            invalid_run = subprocess.run([args.application, "--ppr", invalid], env=env,
                                         capture_output=True, text=True, encoding="utf-8", timeout=10)
            assert invalid_run.returncode == 2 and "Invalid --ppr" in invalid_run.stderr, invalid_run.stderr
        startup_run = subprocess.run([args.application, str(startup), "--ppr", "1", "--screenshot", str(root/"startup.png")],
                                     env=env, capture_output=True, text=True, encoding="utf-8", timeout=30)
        assert startup_run.returncode == 0 and (root/"startup.png").is_file(), startup_run.stderr
        override_run = subprocess.run([args.probe, "--ppr", "1", str(startup)], env=env,
                                      capture_output=True, text=True, encoding="utf-8", timeout=30)
        assert override_run.returncode == 0, override_run.stderr
        override_state = json.loads(override_run.stdout)[0]
        assert "2000.0 RPM" in override_state["metrics"] and "用户显式指定" in override_state["evidence"], override_run.stdout
        conflict = subprocess.run([args.probe, "--ppr", "2", str(normal)], env=env,
                                  capture_output=True, text=True, encoding="utf-8", timeout=30)
        assert conflict.returncode == 0, conflict.stderr
        conflict_state = json.loads(conflict.stdout)[0]
        assert conflict_state["banner"] == "文件无效" and not conflict_state["export_enabled"], conflict.stdout
        assert all(path.read_bytes() == before for path, before in originals.items()), "Startup override changed input files"
        unknown_ppr = subprocess.run([args.probe, str(startup)], env=env, capture_output=True, text=True, encoding="utf-8", timeout=30)
        assert unknown_ppr.returncode == 0, unknown_ppr.stderr
        unknown_ppr_state = json.loads(unknown_ppr.stdout)[0]
        assert "— RPM" in unknown_ppr_state["metrics"] and "PPR 未知" in unknown_ppr_state["metrics"], unknown_ppr_state
        chinese = root / "中文目录" / "测速.BIN"
        generator.generate(chinese, seconds=0.2, header_ppr=1, noise_std_g=0)
        chinese_originals = {path: path.read_bytes() for path in chinese.parent.iterdir() if path.is_file()}
        c_env = dict(env, LC_ALL="C", LANG="C")
        unicode_app = subprocess.run([args.application, str(chinese), "--directory", str(chinese.parent),
                                      "--page", "speed", "--screenshot", str(root / "utf8-c-locale.png")],
                                     env=c_env, capture_output=True, text=True, encoding="utf-8", timeout=30)
        assert unicode_app.returncode == 0 and (root / "utf8-c-locale.png").is_file(), unicode_app.stderr
        unicode_probe = subprocess.run([args.probe, "--directory", str(chinese.parent), str(chinese)],
                                       env=c_env, capture_output=True, text=True, encoding="utf-8", timeout=30)
        assert unicode_probe.returncode == 0, unicode_probe.stderr
        unicode_states = json.loads(unicode_probe.stdout)
        assert unicode_states[0]["listed_paths"] == [str(chinese)], unicode_states
        unicode_state = unicode_states[1]
        assert unicode_state["process_lc_all"] == "C" and unicode_state["process_lang"] == "C", unicode_state
        assert unicode_state["path"] == str(chinese) and unicode_state["file_name"] == "测速.BIN", unicode_state
        assert unicode_state["banner"] == "合成演示" and unicode_state["export_enabled"], unicode_state
        assert "2000.0 RPM" in unicode_state["metrics"] and "100.0%" in unicode_state["metrics"], unicode_state
        assert unicode_state["speed_stats"] and "2000.0" in unicode_state["speed_stats"], unicode_state
        assert all(unicode_state[key] for key in ("exports_valid", "protected_exports_rejected", "symlink_export_rejected", "hardlink_export_rejected")), unicode_state
        assert all(path.read_bytes() == before for path, before in chinese_originals.items()), "C-locale GUI changed Unicode-path input files"
        print("GUI: four visible nonoverlapping plots in 2x2, per-channel series, overlay-only checkboxes, shared range/zoom/pan/full, exact raw cursor and drag from every plot, one preview scan per range/cache reuse, initial/loading/error reset, UTF-8 C-locale paths, capture magic/CRC/list filtering, 5 asynchronous states, raw/FFT cursors, full-record semantics, atomic JSON/CSV protection, worker close, 44px controls, source/PPR and file preservation passed")


if __name__ == "__main__":
    main()
