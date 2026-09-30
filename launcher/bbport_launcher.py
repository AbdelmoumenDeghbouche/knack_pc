#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bloodborne launcher for the native port (GTK4 / libadwaita).

Picks the game folder, edits the port's settings (bbport.ini: upscaler, preset, ...) and the
start-up tweaks passed as environment variables to run.sh, starts and stops the game and shows
its output. Launcher settings live in ~/.config/bbport-launcher/settings.json.
"""

import json
import os
import signal
import sys
from pathlib import Path

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gio, GLib, Gtk  # noqa: E402

PORT_DIR = Path(__file__).resolve().parent.parent  # native_probe
CONFIG_DIR = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "bbport-launcher"
CONFIG_FILE = CONFIG_DIR / "settings.json"
MAX_LOG_LINES = 5000

# Choices: (label, value). The first entry is the default.
UPSCALERS = [("FSR 4", "fsr4"), ("FSR 3", "fsr3"), ("Выключен", "off")]
PRESETS = [("Native AA", 0), ("Quality (x1.5)", 1), ("Balanced (x1.7)", 2),
           ("Performance (x2)", 3), ("Ultra Performance (x3)", 4)]
OUTPUT_RES = [("1920×1080", ""), ("2560×1440", "2560x1440"), ("3840×2160", "3840x2160")]
FPS_MODES = [("Без ограничения (патч)", "uncap"), ("60", "60"), ("90", "90"),
             ("30 (как на PS4)", "30")]
PRESENT_MODES = [("Mailbox", "Mailbox"), ("FIFO (VSync)", "Fifo"),
                 ("FIFO Relaxed", "FifoRelaxed"), ("Immediate", "Immediate")]
DRAW_PIPE = [("Авто (8+ потоков)", ""), ("Включён", "1"), ("Выключен (стабильнее)", "0")]
LANGUAGES = [("Английский", "1"), ("Русский", "8"), ("Японский", "0"), ("Французский", "2"),
             ("Испанский", "3"), ("Немецкий", "4"), ("Итальянский", "5")]
READBACKS = [("Relaxed (по умолчанию)", ""), ("Выключены", "0"), ("Precise", "2")]

DEFAULTS = {
    "game_dir": str(PORT_DIR.parent / "CUSA03173"),
    "user_dir": "",
    "language": "1",
    "fullscreen": False,
    "hdr": False,
    "present_mode": "Mailbox",
    "output_res": "",
    "fps_mode": "uncap",
    "fps_limit": 0,
    "draw_pipe": "",
    "readbacks": "",
    "mangohud": False,
    "frame_stats": False,
    "gpu_profile": False,
    "vk_validation": False,
    "extra_env": "",
}

# bbport.ini keys the launcher edits; the rest of the file is kept.
INI_DEFAULTS = {
    "upscaler": "fsr4",
    "preset": "4",
    "sharpen": "1",
    "sharpness": "0.50",
    "object_motion": "1",
    "show_fps": "1",
}


def load_settings():
    settings = dict(DEFAULTS)
    try:
        settings.update(json.loads(CONFIG_FILE.read_text()))
    except (OSError, ValueError):
        pass
    return settings


def save_settings(settings):
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    CONFIG_FILE.write_text(json.dumps(settings, indent=2, ensure_ascii=False))


def ini_path():
    return Path(os.environ.get("BB_CONFIG", PORT_DIR / "bbport.ini"))


def load_ini():
    values = dict(INI_DEFAULTS)
    lines = []
    try:
        lines = ini_path().read_text().splitlines()
    except OSError:
        pass
    for line in lines:
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    return values, lines


def save_ini(values, lines):
    """Rewrites the edited keys in place, appends missing ones, keeps comments and others."""
    written = set()
    out = []
    for line in lines:
        if "=" in line and not line.lstrip().startswith("#"):
            key = line.split("=", 1)[0].strip()
            if key in values:
                out.append(f"{key}={values[key]}")
                written.add(key)
                continue
        out.append(line)
    if not lines:
        out.append("# bbport settings (in-game menu: Insert / L3+R3)")
    for key, value in values.items():
        if key not in written:
            out.append(f"{key}={value}")
    ini_path().write_text("\n".join(out) + "\n")


def combo_row(title, subtitle, choices, current):
    model = Gtk.StringList.new([label for label, _ in choices])
    row = Adw.ComboRow(title=title, model=model)
    if subtitle:
        row.set_subtitle(subtitle)
    values = [value for _, value in choices]
    row.set_selected(values.index(current) if current in values else 0)
    row.values = values
    return row


def combo_value(row):
    return row.values[row.get_selected()]


class LauncherWindow(Adw.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title="Bloodborne")
        self.set_default_size(760, 820)
        self.settings = load_settings()
        self.ini, self.ini_lines = load_ini()
        self.process = None

        toolbar = Adw.ToolbarView()
        header = Adw.HeaderBar()
        self.stack = Adw.ViewStack()
        switcher = Adw.ViewSwitcher(stack=self.stack, policy=Adw.ViewSwitcherPolicy.WIDE)
        header.set_title_widget(switcher)
        self.launch_button = Gtk.Button(label="Запустить")
        self.launch_button.add_css_class("suggested-action")
        self.launch_button.connect("clicked", self.on_launch)
        header.pack_end(self.launch_button)
        toolbar.add_top_bar(header)

        self.toasts = Adw.ToastOverlay()
        self.toasts.set_child(self.stack)
        toolbar.set_content(self.toasts)
        self.set_content(toolbar)

        self.stack.add_titled_with_icon(self.build_settings_page(), "settings", "Настройки",
                                        "preferences-system-symbolic")
        self.stack.add_titled_with_icon(self.build_log_page(), "log", "Журнал",
                                        "utilities-terminal-symbolic")
        self.connect("close-request", self.on_close)
        self.update_game_status()

    # --- settings page -------------------------------------------------------------------

    def build_settings_page(self):
        page = Adw.PreferencesPage()

        game = Adw.PreferencesGroup(title="Игра")
        self.game_row = Adw.ActionRow(title="Папка игры (CUSA03173)")
        choose = Gtk.Button(icon_name="folder-open-symbolic", valign=Gtk.Align.CENTER,
                            tooltip_text="Выбрать папку с eboot.bin")
        choose.add_css_class("flat")
        choose.connect("clicked", self.on_choose_game)
        self.game_status = Gtk.Image()
        self.game_row.add_suffix(self.game_status)
        self.game_row.add_suffix(choose)
        game.add(self.game_row)
        self.user_row = Adw.EntryRow(title="Папка сохранений (пусто — native_probe/user)",
                                     text=self.settings["user_dir"])
        game.add(self.user_row)
        self.language_row = combo_row("Язык системы", None, LANGUAGES, self.settings["language"])
        game.add(self.language_row)
        page.add(game)

        screen = Adw.PreferencesGroup(title="Экран")
        self.output_row = combo_row("Разрешение вывода",
                                    "Апскейлер дорисовывает кадр до этого размера",
                                    OUTPUT_RES, self.settings["output_res"])
        screen.add(self.output_row)
        self.fullscreen_row = Adw.SwitchRow(title="Полноэкранный режим",
                                            active=self.settings["fullscreen"])
        screen.add(self.fullscreen_row)
        self.present_row = combo_row("Режим показа кадров", None, PRESENT_MODES,
                                     self.settings["present_mode"])
        screen.add(self.present_row)
        self.hdr_row = Adw.SwitchRow(title="Разрешить HDR", active=self.settings["hdr"])
        screen.add(self.hdr_row)
        page.add(screen)

        upscaler = Adw.PreferencesGroup(
            title="Апскейлер",
            description="Хранится в bbport.ini; в игре меняется через меню (Insert или L3+R3)")
        self.upscaler_row = combo_row("Апскейлер", None, UPSCALERS, self.ini["upscaler"])
        upscaler.add(self.upscaler_row)
        self.preset_row = combo_row("Пресет", None, PRESETS, int(self.ini.get("preset", "4")))
        upscaler.add(self.preset_row)
        self.sharpen_row = Adw.SwitchRow(title="Резкость (RCAS)",
                                         active=self.ini.get("sharpen") == "1")
        upscaler.add(self.sharpen_row)
        self.sharpness_row = Adw.SpinRow.new_with_range(0.0, 1.0, 0.05)
        self.sharpness_row.set_title("Сила резкости")
        self.sharpness_row.set_digits(2)
        self.sharpness_row.set_value(float(self.ini.get("sharpness", "0.5")))
        upscaler.add(self.sharpness_row)
        self.motion_row = Adw.SwitchRow(
            title="Векторы движения объектов",
            subtitle="Меньше гостинга на персонажах; стоит около 10% FPS",
            active=self.ini.get("object_motion") == "1")
        upscaler.add(self.motion_row)
        self.show_fps_row = Adw.SwitchRow(title="Показывать FPS",
                                          active=self.ini.get("show_fps") == "1")
        upscaler.add(self.show_fps_row)
        page.add(upscaler)

        frames = Adw.PreferencesGroup(title="Частота кадров")
        self.fps_row = combo_row("Режим", "Какой патч частоты кадров применить к игре",
                                 FPS_MODES, self.settings["fps_mode"])
        frames.add(self.fps_row)
        self.limit_row = Adw.SpinRow.new_with_range(0, 480, 1)
        self.limit_row.set_title("Ограничение FPS")
        self.limit_row.set_subtitle("0 — без ограничения (или по частоте экрана)")
        self.limit_row.set_value(self.settings["fps_limit"])
        frames.add(self.limit_row)
        page.add(frames)

        perf = Adw.PreferencesGroup(title="Производительность")
        self.pipe_row = combo_row(
            "Двухстадийный конвейер GPU",
            "Быстрее на 20–30%; при нестабильности выключите", DRAW_PIPE,
            self.settings["draw_pipe"])
        perf.add(self.pipe_row)
        self.readbacks_row = combo_row("Чтение данных GPU процессором", None, READBACKS,
                                       self.settings["readbacks"])
        perf.add(self.readbacks_row)
        page.add(perf)

        dev = Adw.PreferencesGroup(title="Для разработчика")
        self.mangohud_row = Adw.SwitchRow(title="MangoHud", active=self.settings["mangohud"])
        dev.add(self.mangohud_row)
        self.stats_row = Adw.SwitchRow(title="Статистика кадров в журнале",
                                       subtitle="BB_FRAME_STATS",
                                       active=self.settings["frame_stats"])
        dev.add(self.stats_row)
        self.profile_row = Adw.SwitchRow(title="Профиль GPU в журнале",
                                         subtitle="BB_GPU_PROFILE",
                                         active=self.settings["gpu_profile"])
        dev.add(self.profile_row)
        self.validation_row = Adw.SwitchRow(title="Слои валидации Vulkan",
                                            subtitle="Сильно замедляет",
                                            active=self.settings["vk_validation"])
        dev.add(self.validation_row)
        self.extra_row = Adw.EntryRow(title="Доп. переменные (ИМЯ=значение через пробел)",
                                      text=self.settings["extra_env"])
        dev.add(self.extra_row)
        page.add(dev)
        return page

    def game_dir(self):
        return Path(self.settings["game_dir"]).expanduser()

    def update_game_status(self):
        path = self.game_dir()
        ok = (path / "eboot.bin").is_file()
        self.game_row.set_subtitle(str(path))
        self.game_status.set_from_icon_name("emblem-ok-symbolic" if ok else "dialog-warning-symbolic")
        self.game_status.set_tooltip_text("Найден eboot.bin" if ok else "Нет eboot.bin в папке")
        self.launch_button.set_sensitive(ok or self.process is not None)

    def on_choose_game(self, _button):
        dialog = Gtk.FileDialog(title="Папка игры (с eboot.bin)")
        if self.game_dir().is_dir():
            dialog.set_initial_folder(Gio.File.new_for_path(str(self.game_dir())))
        dialog.select_folder(self, None, self.on_game_chosen)

    def on_game_chosen(self, dialog, result):
        try:
            folder = dialog.select_folder_finish(result)
        except GLib.Error:
            return
        if folder:
            self.settings["game_dir"] = folder.get_path()
            self.update_game_status()
            self.store()

    def store(self):
        s = self.settings
        s["user_dir"] = self.user_row.get_text().strip()
        s["language"] = combo_value(self.language_row)
        s["output_res"] = combo_value(self.output_row)
        s["fullscreen"] = self.fullscreen_row.get_active()
        s["present_mode"] = combo_value(self.present_row)
        s["hdr"] = self.hdr_row.get_active()
        s["fps_mode"] = combo_value(self.fps_row)
        s["fps_limit"] = int(self.limit_row.get_value())
        s["draw_pipe"] = combo_value(self.pipe_row)
        s["readbacks"] = combo_value(self.readbacks_row)
        s["mangohud"] = self.mangohud_row.get_active()
        s["frame_stats"] = self.stats_row.get_active()
        s["gpu_profile"] = self.profile_row.get_active()
        s["vk_validation"] = self.validation_row.get_active()
        s["extra_env"] = self.extra_row.get_text().strip()
        save_settings(s)
        self.ini.update({
            "upscaler": combo_value(self.upscaler_row),
            "preset": str(combo_value(self.preset_row)),
            "sharpen": "1" if self.sharpen_row.get_active() else "0",
            "sharpness": f"{self.sharpness_row.get_value():.2f}",
            "object_motion": "1" if self.motion_row.get_active() else "0",
            "show_fps": "1" if self.show_fps_row.get_active() else "0",
        })
        save_ini(self.ini, self.ini_lines)
        self.ini, self.ini_lines = load_ini()

    def environment(self):
        s = self.settings
        env = dict(os.environ)
        env["BB_GAME_DIR"] = str(self.game_dir())
        if s["user_dir"]:
            env["BB_USER_DIR"] = s["user_dir"]
        env["BB_LANGUAGE"] = s["language"]
        if s["output_res"]:
            env["BB_OUTPUT_RES"] = s["output_res"]
        env["BB_FULLSCREEN"] = "1" if s["fullscreen"] else "0"
        env["BB_PRESENT_MODE"] = s["present_mode"]
        if s["hdr"]:
            env["BB_HDR"] = "1"
        env["BB_FPS"] = s["fps_mode"]
        if s["fps_limit"] > 0:
            env["BB_FPS_LIMIT"] = str(s["fps_limit"])
        if s["draw_pipe"]:
            env["BB_DRAW_PIPE"] = s["draw_pipe"]
        if s["readbacks"]:
            env["BB_READBACKS"] = s["readbacks"]
        if s["mangohud"]:
            env["MANGOHUD"] = "1"
        if s["frame_stats"]:
            env["BB_FRAME_STATS"] = "1"
        if s["gpu_profile"]:
            env["BB_GPU_PROFILE"] = "1"
        if s["vk_validation"]:
            env["BB_VK_VALIDATION"] = "1"
        for item in s["extra_env"].split():
            if "=" in item:
                key, value = item.split("=", 1)
                env[key] = value
        return env

    # --- log page ------------------------------------------------------------------------

    def build_log_page(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.log_view = Gtk.TextView(editable=False, monospace=True, cursor_visible=False,
                                     wrap_mode=Gtk.WrapMode.WORD_CHAR)
        self.log_view.set_top_margin(8)
        self.log_view.set_left_margin(8)
        self.log_view.set_right_margin(8)
        scroller = Gtk.ScrolledWindow(vexpand=True, child=self.log_view)
        self.log_scroller = scroller
        box.append(scroller)
        return box

    def append_log(self, text):
        buffer = self.log_view.get_buffer()
        buffer.insert(buffer.get_end_iter(), text)
        extra = buffer.get_line_count() - MAX_LOG_LINES
        if extra > 0:
            buffer.delete(buffer.get_start_iter(), buffer.get_iter_at_line(extra)[1])
        adj = self.log_scroller.get_vadjustment()
        GLib.idle_add(lambda: adj.set_value(adj.get_upper()) and False)

    # --- process -------------------------------------------------------------------------

    def on_launch(self, _button):
        if self.process:
            self.stop_game()
            return
        self.store()
        self.log_view.get_buffer().set_text("")
        launcher = Gio.SubprocessLauncher.new(
            Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_MERGE)
        launcher.set_environ([f"{k}={v}" for k, v in self.environment().items()])
        launcher.set_cwd(str(PORT_DIR))
        try:
            # setsid: the game and its helpers form one process group, stopped together.
            self.process = launcher.spawnv(["setsid", "bash", str(PORT_DIR / "run.sh")])
        except GLib.Error as error:
            self.toasts.add_toast(Adw.Toast(title=f"Не удалось запустить: {error.message}"))
            return
        self.stream = Gio.DataInputStream.new(self.process.get_stdout_pipe())
        self.read_line()
        self.process.wait_async(None, self.on_exit)
        self.launch_button.set_label("Остановить")
        self.launch_button.remove_css_class("suggested-action")
        self.launch_button.add_css_class("destructive-action")
        self.stack.set_visible_child_name("log")

    def read_line(self):
        self.stream.read_line_async(GLib.PRIORITY_DEFAULT, None, self.on_line)

    def on_line(self, stream, result):
        try:
            line, _length = stream.read_line_finish_utf8(result)
        except GLib.Error:
            return
        if line is None:
            return
        self.append_log(line + "\n")
        self.read_line()

    def stop_game(self):
        if not self.process:
            return
        pid = int(self.process.get_identifier())
        try:
            os.killpg(pid, signal.SIGTERM)
        except OSError:
            self.process.force_exit()
        GLib.timeout_add_seconds(3, self.kill_if_running, pid)

    def kill_if_running(self, pid):
        if self.process:
            try:
                os.killpg(pid, signal.SIGKILL)
            except OSError:
                pass
        return False

    def on_exit(self, process, result):
        try:
            process.wait_finish(result)
        except GLib.Error:
            pass
        status = process.get_exit_status() if process.get_if_exited() else -1
        self.process = None
        self.launch_button.set_label("Запустить")
        self.launch_button.remove_css_class("destructive-action")
        self.launch_button.add_css_class("suggested-action")
        self.update_game_status()
        self.append_log(f"\n— игра завершилась (код {status}) —\n")

    def on_close(self, _window):
        self.store()
        if self.process:
            self.stop_game()
        return False


class LauncherApp(Adw.Application):
    def __init__(self):
        super().__init__(application_id="io.github.bbport.Launcher",
                         flags=Gio.ApplicationFlags.DEFAULT_FLAGS)

    def do_activate(self):
        window = self.get_active_window() or LauncherWindow(self)
        window.present()


if __name__ == "__main__":
    sys.exit(LauncherApp().run(sys.argv))
