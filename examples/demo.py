#!/usr/bin/env python3
"""End-to-end demo: drives the editing-tools MCP server over stdio like an AI client would.

Generates a few seconds of synthetic media, then cuts, mixes music (with ducking and loudness
normalization), adds subtitles, grades color with a LUT, previews frames and exports a video.

    python3 examples/demo.py [--out DIR] [--server PATH]

Set EDITING_TOOLS_FFMPEG to an ffmpeg with libass for the subtitle step.
"""
import argparse
import base64
import json
import os
import subprocess
import sys
import tempfile


class Client:
    def __init__(self, server):
        self.p = subprocess.Popen([server], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.n = 0
        self.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "demo", "version": "0"}})
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")

    def rpc(self, method, params):
        self.n += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.n, "method": method, "params": params}) + "\n")
        self.p.stdin.flush()
        reply = json.loads(self.p.stdout.readline())
        assert reply["id"] == self.n, reply
        return reply

    def call(self, tool, /, **args):
        result = self.rpc("tools/call", {"name": tool, "arguments": args})["result"]
        if result["isError"]:
            raise RuntimeError(f"{tool}: {result['content'][0]['text']}")
        text = next(c["text"] for c in result["content"] if c["type"] == "text")
        image = next((c for c in result["content"] if c["type"] == "image"), None)
        return json.loads(text), image

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=10)


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-nostdin", *args], check=True)


def make_media(d):
    # 6 s "interview": colour test pattern, a 300 Hz "voice" that speaks 2 s out of every 3.
    ffmpeg("-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=6", "-f", "lavfi", "-i",
           "sine=frequency=300:duration=6,volume='if(lt(mod(t,3),2),1,0)':eval=frame",
           "-shortest", "-pix_fmt", "yuv420p", f"{d}/interview.mp4")
    ffmpeg("-f", "lavfi", "-i", "smptebars=size=640x360:rate=25:duration=4", "-pix_fmt", "yuv420p", f"{d}/broll.mp4")
    ffmpeg("-f", "lavfi", "-i", "sine=frequency=220:duration=12", f"{d}/song.wav")
    with open(f"{d}/warm.cube", "w") as f:  # a tiny orange/teal-ish LUT
        n = 9
        f.write(f"LUT_3D_SIZE {n}\n")
        for b in range(n):
            for g in range(n):
                for r in range(n):
                    R, G, B = r / (n - 1), g / (n - 1), b / (n - 1)
                    f.write(f"{min(1, R * 1.12):.4f} {G:.4f} {B * 0.88:.4f}\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None, help="output directory (default: a temp dir)")
    ap.add_argument("--server", default=os.path.expanduser("~/.editing-tools/bin/editing-tools"))
    a = ap.parse_args()
    d = a.out or tempfile.mkdtemp(prefix="et_demo_")
    os.makedirs(d, exist_ok=True)
    make_media(d)

    c = Client(a.server)
    c.call("create_project", project_path=f"{d}/project.json", name="demo", width=640, height=360, fps=25)

    # Cut: interview 0.5-5.5 s, then 3 s of b-roll.
    c.call("add_clip", path=f"{d}/interview.mp4", **{"in": 0.5, "out": 5.5})
    c.call("add_clip", path=f"{d}/broll.mp4", **{"in": 0, "out": 3})

    # Audio: music under the whole piece, ducked while the interviewee talks, loudness-normalized.
    c.call("add_music", path=f"{d}/song.wav", start=0, out=8, volume_db=-8)
    c.call("set_audio", id="m1", fade_in=1, fade_out=2)
    c.call("auto_duck", strength="strong")
    c.call("normalize_loudness", lufs=-16)

    # Subtitles (times are on the OUTPUT timeline, after the cut).
    c.call("set_subtitles", cues=[{"start": 0.2, "end": 2.2, "text": "大家好，欢迎来到演示"},
                                   {"start": 2.5, "end": 4.8, "text": "这是第二句字幕\nwith a second line"}])
    c.call("style_subtitles", box=True, box_opacity=0.55, color="#FFE066", bold=True)

    # Color: project-wide warm grade + S-curve + LUT at 60%.
    c.call("color_adjust", warmth=0.3, contrast=1.1, saturation=1.1)
    c.call("color_curves", points=[[0, 0], [0.25, 0.2], [0.75, 0.82], [1, 1]])
    c.call("apply_lut", path=f"{d}/warm.cube", intensity=0.6)

    for t in (1.0, 6.0):
        info, image = c.call("preview_frame", time=t, max_width=480)
        with open(f"{d}/frame_{t:.0f}s.jpg", "wb") as f:
            f.write(base64.b64decode(image["data"]))

    out, _ = c.call("render_export", output_path=f"{d}/final.mp4")
    c.call("export_srt", output_path=f"{d}/final.srt")
    c.close()

    probe = json.loads(subprocess.run(["ffprobe", "-v", "error", "-print_format", "json", "-show_format", "-show_streams",
                                       f"{d}/final.mp4"], capture_output=True, text=True, check=True).stdout)
    kinds = [s["codec_type"] for s in probe["streams"]]
    print(f"output dir : {d}")
    print(f"final.mp4  : {float(probe['format']['duration']):.2f}s, streams={kinds}, "
          f"{os.path.getsize(d + '/final.mp4') // 1024} KB")
    assert abs(float(probe["format"]["duration"]) - 8.0) < 0.2 and sorted(kinds) == ["audio", "video"]
    print("OK")


if __name__ == "__main__":
    sys.exit(main())
