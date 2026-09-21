"""A-90 ransomware event overlay for Roblox games."""

from __future__ import annotations

import configparser
import ctypes
import random
import time
from ctypes import wintypes
from dataclasses import dataclass
from pathlib import Path
from tkinter import Frame, Label, Toplevel, Tk, ttk
from typing import Callable

from PIL import Image, ImageTk
import pygame


ROOT = Path(__file__).parent
ASSETS = ROOT / "assets"
WASD_KEYS = (0x57, 0x41, 0x53, 0x44)
KEY_DOWN_MASK = 0x8000
REFERENCE_SCREEN_WIDTH = 1920
REFERENCE_SCREEN_HEIGHT = 1080
DISPLAY_SIZE_RATIO = 0.7
TRANSPARENT_COLOR = "#010203"
NOISE_WIDTH = 640
NOISE_HEIGHT = 360
NOISE_COLORS = ((2, 0, 0), (38, 0, 0), (82, 3, 3), (128, 8, 8))
WORN_NOISE_COLORS = ((0, 0, 0), (8, 0, 0), (24, 1, 1), (48, 2, 2))
ATTACK_NOISE_COLORS = ((18, 0, 0), (82, 0, 0), (170, 8, 8), (255, 28, 28))
BACKGROUND_ALPHA = 128
APPEAR_DURATION = 0.5
ATTACK_CHECK_DURATION = 0.6
ATTACK_PREVIEW_DURATION = 0.1
JUMPSCARE_DURATION = 1.5
POPUP_WIDTH = 340
POPUP_HEIGHT = 128
POPUP_PADDING = 300
POPUP_MESSAGES = (
    "Downloading..",
    "Getting the latest updates...",
    "Virus detected! downloading antivirus..",
    ":3",
)
POPUP_TITLES = (
    "Windows Security Update",
    "pls speed i need this ;-;",
    "A-90 moment",
    "Downloading",
    "Security Update",
    "IMPORTANT!!",
    "sans undertale",
)
GWL_EXSTYLE = -20
WS_EX_TRANSPARENT = 0x00000020
WS_EX_NOACTIVATE = 0x08000000
WS_EX_TOOLWINDOW = 0x00000080
WM_NCHITTEST = 0x0084
WM_MOUSEACTIVATE = 0x0021
HTTRANSPARENT = -1
MA_NOACTIVATE = 3
MA_NOACTIVATEANDEAT = 4
HWND_TOPMOST = -1
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_NOACTIVATE = 0x0010
SWP_SHOWWINDOW = 0x0040


@dataclass(frozen=True)
class Config:
    min_interval: float
    max_interval: float
    appear_duration: float
    sample_hz: float
    screen_padding: int
    roblox_window_title: str
    attack_loading_duration: float


def load_config(path: Path = ROOT / "config.txt") -> Config:
    parser = configparser.ConfigParser()
    parser.read_string("[a90]\n" + path.read_text(encoding="utf-8"))
    values = parser["a90"]
    min_interval = values.getfloat("min_interval")
    max_interval = values.getfloat("max_interval")
    if min_interval < 0 or max_interval < min_interval:
        raise ValueError("config.txt requires 0 <= min_interval <= max_interval")
    if values.getfloat("appear_duration") <= 0 or values.getfloat("sample_hz") <= 0:
        raise ValueError("appear_duration and sample_hz must be greater than zero")
    attack_loading_duration = values.getfloat("attack_loading_duration", fallback=5)
    if attack_loading_duration <= 0:
        raise ValueError("attack_loading_duration must be greater than zero")
    return Config(
        min_interval=min_interval,
        max_interval=max_interval,
        appear_duration=values.getfloat("appear_duration"),
        sample_hz=values.getfloat("sample_hz"),
        screen_padding=values.getint("screen_padding"),
        roblox_window_title=values.get("roblox_window_title"),
        attack_loading_duration=attack_loading_duration,
    )


def wasd_were_held_for_every_frame(
    frames: list[tuple[bool, bool, bool, bool]],
) -> bool:
    """Return whether every sampled frame had at least one WASD key down."""
    return bool(frames) and all(any(frame) for frame in frames)


def is_roblox_active(title_fragment: str) -> bool:
    user32 = ctypes.windll.user32
    foreground = user32.GetForegroundWindow()
    if not foreground:
        return False
    title_length = user32.GetWindowTextLengthW(foreground)
    title_buffer = ctypes.create_unicode_buffer(title_length + 1)
    user32.GetWindowTextW(foreground, title_buffer, title_length + 1)
    return title_fragment.casefold() in title_buffer.value.casefold()


def key_is_down(key_code: int) -> bool:
    return bool(ctypes.windll.user32.GetAsyncKeyState(key_code) & KEY_DOWN_MASK)


class SoundPlayer:
    def __init__(self) -> None:
        pygame.mixer.init()
        self.sounds = {
            name: pygame.mixer.Sound(ASSETS / f"{name}.mp3")
            for name in ("appear", "decide_attack", "block", "jumpscare")
        }
        self.popup_sounds = [
            pygame.mixer.Sound(ASSETS / f"popup{number}.mp3")
            for number in range(1, 15)
        ]

    def play(self, name: str) -> None:
        self.sounds[name].play()

    def play_random_popup(self) -> None:
        random.choice(self.popup_sounds).play()


class A90Overlay:
    def __init__(self, root: Tk, config: Config) -> None:
        self.root = root
        self.config = config
        self.return_window = ctypes.windll.user32.GetForegroundWindow()
        self.window = Toplevel(root)
        self.window.withdraw()
        self.window.overrideredirect(True)
        self.window.attributes("-topmost", True)
        self.window.configure(background=TRANSPARENT_COLOR)
        self.window.attributes("-transparentcolor", TRANSPARENT_COLOR)
        self.image_label = None
        self.current_image = None
        self.background_window = Toplevel(root)
        self.background_window.withdraw()
        self.background_window.overrideredirect(True)
        self.background_window.attributes("-topmost", True)
        self.background_window.attributes("-alpha", 0.5)
        self.background_label = None
        self.noise_images: dict[str, Image.Image] = {}
        self.noise_photo_images: dict[tuple[str, int, int], ImageTk.PhotoImage] = {}
        self.scaled_images: dict[tuple[Path, float, int, int], Image.Image] = {}
        self.popups: list[Toplevel] = []
        self.popup_progress: dict[Toplevel, ttk.Progressbar] = {}
        self._click_through_windows = {}
        self._make_click_through(self.window)
        self._make_click_through(self.background_window)
        self.sound_player = SoundPlayer()

    def _make_click_through(self, window: Toplevel, click_through: bool = True) -> None:
        """Make a window avoid activation and optionally pass mouse hit tests through."""
        user32 = ctypes.windll.user32
        hwnd = window.winfo_id()
        if hwnd in self._click_through_windows:
            return
        get_window_long = user32.GetWindowLongPtrW
        set_window_long = user32.SetWindowLongPtrW
        get_window_long.argtypes = (wintypes.HWND, ctypes.c_int)
        get_window_long.restype = ctypes.c_ssize_t
        set_window_long.argtypes = (wintypes.HWND, ctypes.c_int, ctypes.c_ssize_t)
        set_window_long.restype = ctypes.c_ssize_t
        style = get_window_long(hwnd, GWL_EXSTYLE)
        set_window_long(
            hwnd,
            GWL_EXSTYLE,
            style | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        )

        window_proc_type = ctypes.WINFUNCTYPE(
            ctypes.c_ssize_t,
            wintypes.HWND,
            wintypes.UINT,
            wintypes.WPARAM,
            wintypes.LPARAM,
        )
        original_proc = get_window_long(hwnd, -4)
        call_window_proc = user32.CallWindowProcW
        call_window_proc.argtypes = (
            ctypes.c_ssize_t,
            wintypes.HWND,
            wintypes.UINT,
            wintypes.WPARAM,
            wintypes.LPARAM,
        )
        call_window_proc.restype = ctypes.c_ssize_t

        @window_proc_type
        def window_proc(window_handle, message, wparam, lparam):
            if click_through and message == WM_NCHITTEST:
                return HTTRANSPARENT
            if message == WM_MOUSEACTIVATE:
                return MA_NOACTIVATEANDEAT if click_through else MA_NOACTIVATE
            return call_window_proc(original_proc, window_handle, message, wparam, lparam)

        callback_address = ctypes.cast(window_proc, ctypes.c_void_p).value
        set_window_long(hwnd, -4, callback_address)
        self._click_through_windows[hwnd] = window_proc

    def _raise_without_activation(self, window: Toplevel) -> None:
        ctypes.windll.user32.SetWindowPos(
            window.winfo_id(),
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_SHOWWINDOW,
        )

    def _keep_popups_above_background(self) -> None:
        set_window_pos = ctypes.windll.user32.SetWindowPos
        flags = SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_SHOWWINDOW
        set_window_pos(self.background_window.winfo_id(), HWND_TOPMOST, 0, 0, 0, 0, flags)
        for popup in self.popups:
            set_window_pos(popup.winfo_id(), HWND_TOPMOST, 0, 0, 0, 0, flags)

    def _scaled_image(self, image_path: Path, image_scale: float = 1) -> Image.Image:
        screen_width = self.root.winfo_screenwidth()
        screen_height = self.root.winfo_screenheight()
        cache_key = (image_path, image_scale, screen_width, screen_height)
        if cache_key in self.scaled_images:
            return self.scaled_images[cache_key]
        image = Image.open(image_path).convert("RGBA")
        width_ratio = screen_width / REFERENCE_SCREEN_WIDTH
        height_ratio = screen_height / REFERENCE_SCREEN_HEIGHT
        scale = min(width_ratio, height_ratio) * DISPLAY_SIZE_RATIO * image_scale
        size = (max(1, round(image.width * scale)), max(1, round(image.height * scale)))
        scaled_image = image.resize(size, Image.Resampling.LANCZOS)
        self.scaled_images[cache_key] = scaled_image
        return scaled_image

    def _noise_image(self, palette: str) -> ImageTk.PhotoImage:
        if palette not in self.noise_images:
            generator = random.Random({"normal": 90, "worn": 900, "attack": 9000}[palette])
            colors = {
                "normal": NOISE_COLORS,
                "worn": WORN_NOISE_COLORS,
                "attack": ATTACK_NOISE_COLORS,
            }[palette]
            pixels = [colors[generator.randrange(len(colors))] for _ in range(NOISE_WIDTH * NOISE_HEIGHT)]
            self.noise_images[palette] = Image.new("RGB", (NOISE_WIDTH, NOISE_HEIGHT))
            self.noise_images[palette].putdata(pixels)
        screen_width = self.root.winfo_screenwidth()
        screen_height = self.root.winfo_screenheight()
        cache_key = (palette, screen_width, screen_height)
        if cache_key not in self.noise_photo_images:
            noise = self.noise_images[palette].resize(
                (screen_width, screen_height), Image.Resampling.NEAREST
            ).convert("RGBA")
            noise.putalpha(255)
            self.noise_photo_images[cache_key] = ImageTk.PhotoImage(noise)
        return self.noise_photo_images[cache_key]

    def _show_background(self, palette: str, opacity: float = 0.5) -> None:
        screen_width = self.root.winfo_screenwidth()
        screen_height = self.root.winfo_screenheight()
        self.background_window.geometry(f"{screen_width}x{screen_height}+0+0")
        self.background_window.attributes("-alpha", opacity)
        self.background_image = self._noise_image(palette)
        self.background_label = self.background_label or __import__("tkinter").Label(
            self.background_window,
            borderwidth=0,
            highlightthickness=0,
        )
        self.background_label.configure(image=self.background_image)
        self.background_label.pack(fill="both", expand=True)
        self.background_window.deiconify()
        self._raise_without_activation(self.background_window)

    def _show_image(self, image_path: Path, x: int, y: int, image_scale: float = 1) -> None:
        image = self._scaled_image(image_path, image_scale)
        self.window.geometry(f"{image.width}x{image.height}+{x}+{y}")
        self.current_image = ImageTk.PhotoImage(image)
        self.image_label = self.image_label or __import__("tkinter").Label(
            self.window,
            background=TRANSPARENT_COLOR,
            borderwidth=0,
            highlightthickness=0,
        )
        self.image_label.configure(image=self.current_image)
        self.image_label.pack(fill="both", expand=True)
        self.window.deiconify()
        self._raise_without_activation(self.window)

    def move_entity(self, image_path: Path, x: int, y: int, image_scale: float = 1) -> None:
        image = self._scaled_image(image_path, image_scale)
        self.window.geometry(f"{image.width}x{image.height}+{x}+{y}")

    def move_entity_centered(self, image_path: Path, image_scale: float = 1) -> None:
        image = self._scaled_image(image_path, image_scale)
        x = (self.root.winfo_screenwidth() - image.width) // 2
        y = (self.root.winfo_screenheight() - image.height) // 2
        self.window.geometry(f"{image.width}x{image.height}+{x}+{y}")

    def show_random(self) -> None:
        foreground_window = ctypes.windll.user32.GetForegroundWindow()
        if foreground_window:
            self.return_window = foreground_window
        self.background_window.withdraw()
        image = self._scaled_image(ASSETS / "a90_main.webp")
        screen_width = self.root.winfo_screenwidth()
        screen_height = self.root.winfo_screenheight()
        max_x = max(self.config.screen_padding, screen_width - image.width - self.config.screen_padding)
        max_y = max(self.config.screen_padding, screen_height - image.height - self.config.screen_padding)
        self._show_image(
            ASSETS / "a90_main.webp",
            random.randint(self.config.screen_padding, max_x),
            random.randint(self.config.screen_padding, max_y),
        )

    def show_center(
        self,
        image_path: Path,
        image_scale: float = 1,
        palette: str = "normal",
        image_offset: tuple[int, int] = (0, 0),
        background_opacity: float = 0.5,
    ) -> None:
        self._show_background(palette, background_opacity)
        image = self._scaled_image(image_path, image_scale)
        x = (self.root.winfo_screenwidth() - image.width) // 2 + image_offset[0]
        y = (self.root.winfo_screenheight() - image.height) // 2 + image_offset[1]
        self._show_image(image_path, x, y, image_scale)

    def show_centered_entity(
        self,
        image_path: Path,
        image_scale: float = 1,
        image_offset: tuple[int, int] = (0, 0),
    ) -> None:
        image = self._scaled_image(image_path, image_scale)
        x = (self.root.winfo_screenwidth() - image.width) // 2 + image_offset[0]
        y = (self.root.winfo_screenheight() - image.height) // 2 + image_offset[1]
        self._show_image(image_path, x, y, image_scale)

    def show_background(self, palette: str = "normal", opacity: float = 0.5) -> None:
        self._show_background(palette, opacity)

    def hide_entity(self) -> None:
        self.window.withdraw()

    def _dismiss_popup(self, popup: Toplevel) -> None:
        if popup in self.popups:
            self.popups.remove(popup)
        self.popup_progress.pop(popup, None)
        popup.destroy()

    def _bind_popup_click(self, widget: object, popup: Toplevel) -> None:
        widget.bind("<Button-1>", lambda _event: self._dismiss_popup(popup))
        if hasattr(widget, "winfo_children"):
            for child in widget.winfo_children():
                self._bind_popup_click(child, popup)

    def _create_popup(self, message: str, x: int, y: int) -> None:
        popup = Toplevel(self.root)
        popup.overrideredirect(True)
        popup.attributes("-topmost", True)
        self._make_click_through(popup, click_through=False)
        popup.configure(background="#d9e2f3", relief="raised", borderwidth=2)
        popup.geometry(f"{POPUP_WIDTH}x{POPUP_HEIGHT}+{x}+{y}")
        popup.protocol("WM_DELETE_WINDOW", lambda: self._dismiss_popup(popup))

        title_bar = Frame(popup, background="#24589a", height=28)
        title_bar.pack(fill="x")
        Label(
            title_bar,
            text=random.choice(POPUP_TITLES),
            background="#24589a",
            foreground="white",
            anchor="w",
            font=("Segoe UI", 9, "bold"),
        ).pack(fill="both", padx=8)
        body = Frame(popup, background="#f1f4f9")
        body.pack(fill="both", expand=True, padx=10, pady=8)
        Label(
            body,
            text=message,
            background="#f1f4f9",
            foreground="#111111",
            anchor="w",
            font=("Segoe UI", 10),
        ).pack(fill="x")
        progress = ttk.Progressbar(body, orient="horizontal", mode="determinate", length=300, maximum=100)
        progress.pack(fill="x", pady=(14, 0))
        self._bind_popup_click(popup, popup)
        self.popup_progress[popup] = progress
        self.popups.append(popup)
        self.sound_player.play_random_popup()
        popup.deiconify()
        self._make_click_through(popup, click_through=False)
        self._raise_without_activation(popup)

    def show_attack_popups(self, duration: float) -> bool:
        self.hide_entity()
        screen_width = self.root.winfo_screenwidth()
        screen_height = self.root.winfo_screenheight()
        max_x = max(POPUP_PADDING, screen_width - POPUP_WIDTH - POPUP_PADDING)
        max_y = max(POPUP_PADDING, screen_height - POPUP_HEIGHT - POPUP_PADDING)
        for index in range(random.randint(5, 8)):
            self._create_popup(
                random.choice(POPUP_MESSAGES),
                random.randint(POPUP_PADDING, max_x),
                random.randint(POPUP_PADDING, max_y),
            )
            self.root.update()
            time.sleep(0.08)
        self._keep_popups_above_background()
        started = time.monotonic()
        while self.popups and time.monotonic() - started < duration:
            progress = min(100, (time.monotonic() - started) / duration * 100)
            for popup in self.popups:
                self.popup_progress[popup]["value"] = progress
            self.root.update()
            time.sleep(0.01)
        completed = not self.popups
        for popup in self.popups[:]:
            self._dismiss_popup(popup)
        return completed

    def hide(self) -> None:
        for popup in self.popups[:]:
            self._dismiss_popup(popup)
        self.window.withdraw()
        self.background_window.withdraw()


def a90_attack(overlay: A90Overlay, config: Config) -> None:
    """Check WASD for one second, then run the selected attack response."""
    overlay.show_center(ASSETS / "a90_main.webp", palette="normal")
    overlay.root.update()
    overlay.sound_player.play("decide_attack")
    frames: list[tuple[bool, bool, bool, bool]] = []
    check_started = time.monotonic()
    interval = 1 / config.sample_hz
    next_sample = check_started
    while time.monotonic() - check_started < ATTACK_CHECK_DURATION:
        frames.append(tuple(key_is_down(key) for key in WASD_KEYS))
        overlay.root.update()
        next_sample += interval
        time.sleep(max(0, next_sample - time.monotonic()))

    attacked = wasd_were_held_for_every_frame(frames)
    if attacked:
        overlay.show_background("attack", opacity=0.5)
        overlay.show_center(
            ASSETS / "a90_scare.webp",
            image_scale=2,
            palette="attack",
            image_offset=(0, 0),
        )
        overlay.root.update()
        preview_started = time.monotonic()
        while time.monotonic() - preview_started < ATTACK_PREVIEW_DURATION:
            shake = 12
            overlay.show_centered_entity(
                ASSETS / "a90_scare.webp",
                image_scale=2,
                image_offset=(
                    random.randint(-shake, shake),
                    random.randint(-shake, shake),
                ),
            )
            overlay.root.update()
            time.sleep(0.01)
        popup_survived = overlay.show_attack_popups(overlay.config.attack_loading_duration)
        if popup_survived:
            overlay.hide()
            return

        # actual jumpscare
        overlay.sound_player.play("jumpscare")
        overlay.show_background("attack", opacity=1)
        overlay.show_centered_entity(
            ASSETS / "a90_scare.webp",
            image_scale=2,
            image_offset=(0, 0),
        )
        overlay.root.update()
        started = time.monotonic()
        while time.monotonic() - started < JUMPSCARE_DURATION:
            overlay.move_entity_centered(ASSETS / "a90_scare.webp", image_scale=2)
            overlay.root.update()
            time.sleep(max(0.01, min(0.04, JUMPSCARE_DURATION - (time.monotonic() - started))))
        overlay.hide()
        return
    overlay.sound_player.play("block")
    overlay.show_center(ASSETS / "a90_main.webp", palette="normal")
    overlay.root.update()
    time.sleep(0.1)
    overlay.show_center(ASSETS / "a90_block.webp", palette="worn")
    overlay.root.update()
    time.sleep(0.1)
    overlay.hide()


def a90_appear(overlay: A90Overlay, config: Config) -> None:
    """Display A-90 harmlessly for 500 ms before the attack check begins."""
    overlay.sound_player.play("appear")
    overlay.show_random()
    overlay.root.update()
    deadline = time.monotonic() + APPEAR_DURATION
    while time.monotonic() < deadline:
        overlay.root.update()
        time.sleep(0.01)
    a90_attack(overlay, config)


def main(
    config: Config | None = None,
    active_window_check: Callable[[str], bool] = is_roblox_active,
) -> None:
    config = config or load_config()
    root = Tk()
    root.withdraw()
    overlay = A90Overlay(root, config)
    next_appearance = time.monotonic() + random.uniform(config.min_interval, config.max_interval)
    try:
        while True:
            root.update()
            if time.monotonic() >= next_appearance and active_window_check(config.roblox_window_title):
                a90_appear(overlay, config)
                next_appearance = time.monotonic() + random.uniform(config.min_interval, config.max_interval)
            time.sleep(0.05)
    except KeyboardInterrupt:
        overlay.hide()
        root.destroy()


def test(
    config: Config | None = None
) -> None:
    config = config or load_config()
    root = Tk()
    root.withdraw()
    overlay = A90Overlay(root, config)
    next_appearance = time.monotonic() + random.uniform(config.min_interval, config.max_interval)
    try:
        print('attacking in 3s...')
        time.sleep(3)
        a90_appear(overlay, config)
        next_appearance = time.monotonic() + random.uniform(config.min_interval, config.max_interval)
    except KeyboardInterrupt:
        overlay.hide()
        root.destroy()

if __name__ == "__main__":
    # main()
    test()