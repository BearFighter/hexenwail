#!/usr/bin/env python3
"""Keep the visual-switch presets, safe mode, menu rows and diff sweep in step.

The large-world contract moves both far clips to 65536 and asks HDR, bloom,
SSAO, FXAA and both OIT modes to work there.  Four places have to agree or a
"preset" silently leaves one switch behind:

  * M_Display_AdjustSliders' PRESET_EFFECTS macro and its seven call sites;
  * GL_RendererSafe_f, which must put every one of those switches back;
  * the Rendering submenu, whose new rows must be hidden on WEBSOFT, gated on
    the capability the engine reports, driven by name and noted with the cvar;
  * tools/sweep-visual-switches.steps, the same-binary screenshot-diff gate
    that exercises them at the large far clip.

Pure source checks; no engine build or retail data needed.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
menu = (ROOT / "engine/hexen2/menu.c").read_text(encoding="utf-8")
safe = (ROOT / "engine/h2shared/gl_vidsdl.c").read_text(encoding="utf-8")
steps = ROOT / "tools/sweep-visual-switches.steps"
errors = []

EFFECT_CVARS = (
    "r_hdr", "r_hdr_exposure", "r_bloom", "r_bloom_intensity",
    "r_bloom_threshold", "r_oit", "r_ssao", "r_softparticles",
    "r_farclip", "gl_farclip",
)
# hdr, bloom, oit, ssao, soft particles, r_farclip, gl_farclip
PRESETS = {
    1: (0, 0, 0, 0, 0, 4096, 16384),
    2: (0, 0, 0, 0, 0, 4096, 16384),
    3: (0, 0, 0, 0, 0, 4096, 16384),
    4: (0, 0, 0, 0, 0, 4096, 16384),
    5: (0, 0, 0, 0, 1, 4096, 16384),
    6: (1, 1, 1, 2, 1, 16384, 16384),
    7: (1, 1, 2, 3, 1, 65536, 65536),
}
SAFE_VALUES = {
    "r_hdr": "0", "r_hdr_exposure": "1", "r_oit": "0", "r_bloom": "0",
    "r_bloom_intensity": "1", "r_bloom_threshold": "1", "r_ssao": "0",
    "r_softparticles": "0", "r_farclip": "4096", "gl_farclip": "16384",
    "r_scale": "1", "r_softemu": "0", "gl_fxaa": "0", "r_motionblur": "0",
    "gl_lightmapfmt": "GL_RGBA", "gl_lmatlas": "1",
}
ROWS = {
    "REND_BLOOM": ("r_bloom", "gl_renderer_caps.postprocess"),
    "REND_OIT": ("r_oit", "gl_renderer_caps.oit"),
    "REND_SSAO": ("r_ssao", "gl_renderer_caps.compute_shaders"),
}
SHOTS = (
    "01-base-farclip", "02-fxaa", "03-hdr", "04-hdr-exposure", "05-bloom",
    "06-torch-bloom", "07-torch-hdr-bloom", "08-ssao-off", "09-ssao-low",
    "10-ssao-high", "11-oit-off", "12-oit-weighted", "13-oit-moment",
)


def function_body(text, signature):
    m = re.search(signature + r"\s*\{", text)
    if not m:
        return ""
    start = text.index("{", m.end() - 1)
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    return ""


# --- the preset macro -------------------------------------------------------
lines = menu.splitlines()
starts = [i for i, line in enumerate(lines) if line.startswith("#define PRESET_EFFECTS")]
if not starts:
    errors.append("menu.c: no PRESET_EFFECTS macro")
else:
    end = starts[0]
    while lines[end].rstrip().endswith("\\"):
        end += 1
    body = "\n".join(lines[starts[0]:end + 1])
    for cvar in EFFECT_CVARS:
        if not re.search(r'Cvar_SetValue\s*\(\s*"%s"\s*,' % re.escape(cvar), body):
            errors.append(f"PRESET_EFFECTS does not set {cvar}")

case = re.search(r"case DISP_PRESET:(.*?)\n\tcase DISP_GAMMA:", menu, re.S)
if not case:
    errors.append("menu.c: DISP_PRESET case not found")
else:
    blocks = {}
    for m in re.finditer(r"(?:else\s+)?if \(preset == (\d)\)[^\n]*\s*\{(.*?)\n\t\t\}",
                         case.group(1), re.S):
        n = int(m.group(1))
        if n in blocks:
            errors.append(f"preset {n} has two blocks")
        blocks[n] = m.group(2)
    if sorted(blocks) != list(range(1, 8)):
        errors.append(f"preset blocks are {sorted(blocks)}, want 1..7")
    for n in range(1, 8):
        calls = re.findall(r"PRESET_EFFECTS\s*\(([^)]*)\)", blocks.get(n, ""))
        if len(calls) != 1:
            errors.append(f"preset {n} calls PRESET_EFFECTS {len(calls)} times")
            continue
        got = tuple(int(x.strip().rstrip("f")) for x in calls[0].split(","))
        if got != PRESETS[n]:
            errors.append(f"preset {n}: PRESET_EFFECTS {got}, want {PRESETS[n]}")

# --- renderer_safe ----------------------------------------------------------
body = function_body(safe, r"static\s+void\s+GL_RendererSafe_f\s*\(void\)")
if not body:
    errors.append("gl_vidsdl.c: GL_RendererSafe_f not found")
else:
    for cvar, value in SAFE_VALUES.items():
        if not re.search(r'Cvar_Set\s*\(\s*"%s"\s*,\s*"%s"\s*\)'
                         % (re.escape(cvar), re.escape(value)), body):
            errors.append(f"renderer_safe does not restore {cvar} to {value}")
    if "Cvar_SetValue" in body:
        errors.append("renderer_safe uses Cvar_SetValue; these defaults are strings")

# --- the Rendering submenu rows ---------------------------------------------
for row, (cvar, cap) in ROWS.items():
    if not re.search(r"\b%s\s*," % row, menu):
        errors.append(f"menu.c: no {row} enum member")
    if not re.search(r'"[^\"]*".*?/\*\s*%s\s*\*/' % row, menu):
        errors.append(f"menu.c: no label tagged /* {row} */")
    skip = function_body(menu, r"static\s+qboolean\s+M_Rendering_IsSkip\s*\(int i\)")
    if not re.search(r"case\s+%s\s*:" % row, skip):
        errors.append(f"M_Rendering_IsSkip does not hide {row} under WEBSOFT")
    if not re.search(r"i == %s && !%s" % (row, re.escape(cap)), skip):
        errors.append(f"M_Rendering_IsSkip does not gate {row} on {cap}")
    adjust = function_body(menu, r"static\s+void\s+M_Rendering_AdjustSliders\s*\(int dir\)")
    cases = list(re.finditer(r"case\s+%s\s*:(.*?)(?=\n\tcase\s|\n\t\})" % row,
                             adjust, re.S))
    if not cases or not re.search(r'Cvar_Set\w*\s*\(\s*"%s"' % re.escape(cvar),
                                  cases[0].group(1)):
        errors.append(f"M_Rendering_AdjustSliders' {row} row does not set {cvar}")
    draw = function_body(menu, r"static\s+void\s+M_Rendering_Draw\s*\(void\)")
    if not re.search(r"rend_labels\[%s\]" % row, draw):
        errors.append(f"M_Rendering_Draw never draws {row}")
    if not re.search(r"case\s+%s:[^\n]*%s\s*\*/" % (row, re.escape(cvar)), menu):
        errors.append(f"M_Rendering_Note's {row} note does not name {cvar}")

# --- the screenshot-diff sweep ----------------------------------------------
if not steps.is_file():
    errors.append("tools/sweep-visual-switches.steps is missing")
else:
    text = steps.read_text(encoding="utf-8")
    commands = re.findall(r"^cmd\s+(.*)$", text, re.M)
    for cmd in ("renderer_safe", "r_farclip 65536", "gl_farclip 65536",
                "gl_fxaa 1", "r_hdr 1", "r_hdr_exposure 0.5", "r_bloom 1",
                "r_ssao 1", "r_ssao 3", "r_oit 1", "r_oit 2",
                "r_ssao_debug 0", "gpu_unrouted"):
        if cmd not in commands:
            errors.append(f"sweep-visual-switches.steps: missing `cmd {cmd}`")
    if "renderer_safe" in commands and "r_farclip 65536" in commands:
        if commands.index("renderer_safe") > commands.index("r_farclip 65536"):
            errors.append("sweep-visual-switches.steps: renderer_safe must run before the far clips")
    for shot in SHOTS:
        if f"shot {shot}" not in text:
            errors.append(f"sweep-visual-switches.steps: missing shot {shot}")
    if re.search(r"^\s*console\s*$", text, re.M):
        errors.append("sweep-visual-switches.steps: no console frame (the cursor blinks)")
    if "shot-diff.sh" not in text:
        errors.append("sweep-visual-switches.steps: header has no shot-diff invocation")

if errors:
    sys.exit("FAIL test_visual_switch_gate:\n  " + "\n  ".join(errors))
print("PASS visual switches: seven presets, safe mode, three menu rows and the "
      "large-farclip screenshot sweep agree")
