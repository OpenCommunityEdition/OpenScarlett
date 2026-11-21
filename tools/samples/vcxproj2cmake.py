#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read a Microsoft sample's .vcxproj and emit the Release|Gaming.Xbox.Scarlett.x64 build
inputs as CMake variables: sources, include dirs, defines, shaders and deployed assets.

Usage: vcxproj2cmake.py <sample.vcxproj> <out.cmake>
"""
import os
import re
import sys
import xml.etree.ElementTree as ET

NS = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
CFG = "'$(Configuration)|$(Platform)'=='Release|Gaming.Xbox.Scarlett.x64'"
STAGES = {"Vertex": "vs", "Pixel": "ps", "Compute": "cs", "Geometry": "gs", "Hull": "hs", "Domain": "ds",
          "Mesh": "ms", "Amplification": "as", "Library": "lib", "RootSignature": "rootsig"}


PROPS = {"Configuration": "Release", "Platform": "Gaming.Xbox.Scarlett.x64"}  # argv[3] may name another platform


def eval_condition(cond):
    """Evaluate an MSBuild condition for Release|Gaming.Xbox.Scarlett.x64: quoted comparisons with
    == and !=, joined by And/Or, with parentheses. Anything else (Exists(), other properties)
    evaluates to False."""
    text = re.sub(r"\$\((\w+)\)", lambda m: PROPS.get(m.group(1), "\0" + m.group(1)), cond)
    tokens = re.findall(r"'[^']*'|==|!=|\(|\)|\bAnd\b|\bOr\b|\S+", text, re.I)
    out = []
    for t in tokens:
        if t.startswith("'"):
            if "\0" in t:
                return False  # an unknown property
            out.append(repr(t[1:-1].lower()))
        elif t in ("==", "!=", "(", ")"):
            out.append(t)
        elif t.lower() in ("and", "or"):
            out.append(t.lower())
        else:
            return False
    try:
        return bool(eval(" ".join(out), {"__builtins__": {}}, {}))
    except Exception:
        return False


def cond_ok(el):
    c = el.get("Condition")
    return c is None or eval_condition(c)


def resolve_case(p):
    """MSBuild paths are case-insensitive; find the on-disk spelling (Linux is case-sensitive)."""
    if os.path.exists(p):
        return p
    parts = p.split("/")
    cur = "/" if p.startswith("/") else ""
    for comp in parts:
        if not comp:
            continue
        cand = os.path.join(cur, comp) if cur else comp
        if not os.path.exists(cand) and os.path.isdir(cur or "."):
            low = comp.lower()
            match = next((e for e in os.listdir(cur or ".") if e.lower() == low), None)
            if match is None:
                return p
            cand = os.path.join(cur, match) if cur else match
        cur = cand
    return cur


def path(base, p):
    return resolve_case(os.path.normpath(os.path.join(base, p.replace("\\", "/"))).replace("\\", "/"))


def main():
    if len(sys.argv) > 3:
        PROPS["Platform"] = sys.argv[3]  # e.g. Gaming.Desktop.x64 for reference builds
    proj, out = sys.argv[1], sys.argv[2]
    base = os.path.dirname(os.path.abspath(proj))
    root = ET.parse(proj).getroot()
    srcs, incs, defs, shaders, assets, shader_incs = [], [], [], [], [], []
    uses_dxtk = False
    shader_defaults = {"model": "6.0", "entry": "main", "opts": ""}
    default_dest = ""
    for idg in root.findall("m:ItemDefinitionGroup", NS):
        if idg.get("Condition") is None:
            for c in idg.findall("m:CopyFileToFolders", NS):
                for e in c.findall("m:DestinationFolders", NS):
                    default_dest = e.text or ""
    for idg in root.findall("m:ItemDefinitionGroup", NS):
        if not cond_ok(idg):
            continue
        for cl in idg.findall("m:ClCompile", NS):
            for e in cl.findall("m:AdditionalIncludeDirectories", NS):
                for p in e.text.split(";"):
                    if p and "%(" not in p:
                        incs.append(path(base, p.replace("$(ProjectDir)", "./")))
            for e in cl.findall("m:PreprocessorDefinitions", NS):
                defs += [d for d in e.text.split(";") if d and "%(" not in d]
        for fx in [c for c in idg if c.tag.lower().endswith("}fxcompile")]:
            for e in fx.findall("m:AdditionalIncludeDirectories", NS):
                for p in (e.text or "").split(";"):
                    if p and "%(" not in p:
                        shader_incs.append(path(base, p.replace("$(ProjectDir)", "./")))
            for e in fx.findall("m:ShaderModel", NS):
                shader_defaults["model"] = e.text
            for e in fx.findall("m:EntryPointName", NS):
                shader_defaults["entry"] = e.text
            for e in fx.findall("m:AdditionalOptions", NS):
                shader_defaults["opts"] = e.text.replace("%(AdditionalOptions)", "").strip()
    for pg in root.findall("m:PropertyGroup", NS):
        if not cond_ok(pg):
            continue
        for e in pg.findall("m:CharacterSet", NS):
            if e.text == "Unicode":
                defs += ["UNICODE", "_UNICODE"]
    # Shared-items projects (<Import Project="X.vcxitems" Label="Shared"/>) contribute sources and
    # their directory as an include path.
    shared_roots = []
    for imp in root.iter("{%s}Import" % NS["m"]):
        if imp.get("Label") == "Shared" and imp.get("Project", "").endswith(".vcxitems"):
            ip = path(base, imp.get("Project"))
            if os.path.exists(ip):
                shared_roots.append(ip)
    for ip in shared_roots:
        idir = os.path.dirname(ip)
        incs.append(idir)
        for c in ET.parse(ip).getroot().iter("{%s}ClCompile" % NS["m"]):
            inc = c.get("Include")
            if inc:
                srcs.append(path(idir, inc.replace("$(MSBuildThisFileDirectory)", "")))
    for ig in root.findall("m:ItemGroup", NS):
        for c in ig.findall("m:ClCompile", NS):
            inc = c.get("Include")
            if "ATGTelemetry" in inc:
                continue
            excl = [e for e in c.findall("m:ExcludedFromBuild", NS) if cond_ok(e) and e.text == "true"]
            if not excl:
                srcs.append(path(base, inc))
        for f in [c for c in ig if c.tag.lower().endswith("}fxcompile")]:
            excl = [e for e in f.findall("m:ExcludedFromBuild", NS) if cond_ok(e) and e.text == "true"]
            stage = next((e.text for e in f.findall("m:ShaderType", NS) if cond_ok(e)), None)
            raw_opts = next((e.text for e in f.findall("m:AdditionalOptions", NS) if cond_ok(e)), "") or ""
            explicit = re.search(r"[/-]T\s*(\w+_\d+_\d+)", raw_opts)
            if excl or ((not stage or stage not in STAGES) and not explicit):
                continue
            entry = next((e.text for e in f.findall("m:EntryPointName", NS) if cond_ok(e)), None)
            entry = (entry or "").strip() or shader_defaults["entry"] or "main"
            model = next((e.text for e in f.findall("m:ShaderModel", NS) if cond_ok(e)), shader_defaults["model"])
            outname = next((e.text for e in f.findall("m:ObjectFileOutput", NS) if cond_ok(e)), None)
            opts = next((e.text for e in f.findall("m:AdditionalOptions", NS) if cond_ok(e)), shader_defaults["opts"])
            name = os.path.splitext(os.path.basename(f.get("Include").replace("\\", "/")))[0]
            if outname:
                stem = os.path.splitext(os.path.basename(f.get("Include").replace("\\", "/")))[0]
                o = outname.replace("\\", "/").replace("$(OutDir)", "").replace("%(Filename)", stem)
                name = os.path.splitext(os.path.basename(o))[0]
            sm = model.replace(".", "_")
            if explicit:
                target = explicit.group(1)  # /Tms_6_5 etc. in AdditionalOptions wins
            else:
                target = f"{STAGES[stage]}_{sm}" if STAGES[stage] != "rootsig" else "rootsig_1_1"
            toks = re.sub(r"[/-]T\s*\w+_\d+_\d+", "", (opts or "")).replace("%(AdditionalOptions)", "").split()
            keep = []
            for t in toks:
                if "$(OutDir)" in t or "%(Filename)" in t:
                    # Output paths (e.g. /Fd for PDBs): @OUTDIR@ becomes the sample's output directory.
                    t = t.replace('"', "").replace("$(OutDir)", "@OUTDIR@").replace("%(Filename)", name).replace("\\", "/")
                    t = t.replace("@OUTDIR@/", "@OUTDIR@")
                elif "$(" in t or "%(" in t:
                    # Other MSBuild variables: drop the value and its flag.
                    if keep and keep[-1][:1] in "-/" and len(keep[-1]) <= 4:
                        keep.pop()
                    continue
                keep.append(t)
            if any(k in ("/Fd", "-Fd") for k in keep) and not any(k in ("/Zi", "-Zi", "/Zs", "-Zs") for k in keep):
                keep.insert(0, "/Zs")  # a PDB path implies debug info (dxc refuses /Fd without it)
            if "/Qembed_debug" in keep and "/Zi" not in keep and "-Zi" not in keep:
                keep.insert(0, "/Zi")  # MSBuild adds /Zi when EnableDebuggingInformation is set
            opts = " ".join(keep)
            shaders.append(f"{path(base, f.get('Include'))}|{target}|{entry}|{name}|{opts}")
        for tag in ("CopyFileToFolders", "Image", "None", "Font", "Media"):
            for a in ig.findall(f"m:{tag}", NS):
                dep = [e for e in a.findall("m:DeploymentContent", NS) if e.text == "true"]
                inc = a.get("Include")
                if tag == "CopyFileToFolders" or dep:
                    STORE = ("Logo.png", "LargeLogo.png", "SmallLogo.png", "SplashScreen.png", "StoreLogo.png")
                    norm = inc.replace("\\", "/")
                    if norm.split("/")[-1] in STORE or inc.endswith(".config") or inc.endswith(".mgc"):
                        continue
                    dest = next((e.text for e in a.findall("m:DestinationFolders", NS) if cond_ok(e)), "") or default_dest
                    dfn = next((e.text for e in a.findall("m:DestinationFileName", NS) if cond_ok(e)), None)
                    if dfn:
                        # e.g. "Assets/Fonts/%(Filename)%(Extension)": folder relative to the output dir.
                        stem, ext = os.path.splitext(os.path.basename(norm))
                        dfn = dfn.replace("%(Filename)", stem).replace("%(Extension)", ext).replace("\\", "/")
                        base_dir = dest.replace("$(OutDir)", "").replace("\\", "/").strip("/") if dest else ""
                        sub = "/".join(x for x in (base_dir, os.path.dirname(dfn)) if x)
                        assets.append(f"{path(base, inc)}|{sub}|{os.path.basename(dfn)}")
                        continue
                    if dest:
                        sub = dest.replace("$(OutDir)", "").replace("%(RelativeDir)", "").replace("\\", "/").strip("/")
                    elif not norm.startswith(".."):
                        sub = os.path.dirname(norm)  # project-relative items keep their folder (Assets/...)
                    else:
                        sub = ""
                    assets.append(f"{path(base, inc)}|{sub}")
        for r in ig.findall("m:ProjectReference", NS):
            if "DirectXTK12" in r.get("Include"):
                uses_dxtk = True
    # Extra import libraries of the Scarlett configuration (the CMake side keeps those the SDK has).
    libs = []
    for idg in root.findall("m:ItemDefinitionGroup", NS):
        if not cond_ok(idg):
            continue
        for e in idg.findall("m:Link/m:AdditionalDependencies", NS):
            libs += [x.strip() for x in (e.text or "").split(";") if x.strip().lower().endswith(".lib") and "$(" not in x]
    # vcpkg manifest dependencies (e.g. zlib), provided by the layer's vendored copies.
    vcpkg = []
    manifest = os.path.join(base, "vcpkg.json")
    if os.path.exists(manifest):
        import json
        vcpkg = [d if isinstance(d, str) else d.get("name", "") for d in json.load(open(manifest)).get("dependencies", [])]
    clean = lambda x: "|".join(" ".join(f.split()) for f in x.replace("\\", "/").replace('"', "").split("|"))
    q = lambda xs: " ".join('"%s"' % clean(x) for x in xs)
    with open(out, "w") as f:
        f.write(f"set(S_SOURCES {q(srcs)})\nset(S_INCLUDES {q(sorted(set(incs)))})\n")
        f.write(f"set(S_DEFINES {q(sorted(set(defs)))})\nset(S_SHADERS {q(shaders)})\n")
        f.write(f"set(S_SHADER_INCLUDES {q(sorted(set(shader_incs + incs)))})\n")
        f.write(f"set(S_ASSETS {q(assets)})\nset(S_USES_DXTK {'ON' if uses_dxtk else 'OFF'})\n")
        f.write(f"set(S_EXTRA_LIBS {q(sorted(set(libs)))})\nset(S_VCPKG {q(vcpkg)})\n")


if __name__ == "__main__":
    main()
