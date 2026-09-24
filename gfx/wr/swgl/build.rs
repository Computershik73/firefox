/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

extern crate cc;
extern crate glsl_to_cxx;
extern crate webrender_build;

use std::collections::HashSet;
use std::fmt::Write;
use webrender_build::shader::{get_shader_features, ShaderFeatureFlags};

// Shader key is in "name feature,feature" format.
// File name needs to be formatted as "name_feature_feature".
fn shader_file(shader_key: &str) -> String {
    shader_key.replace([' ', ','], "_")
}

fn write_load_shader(shader_keys: &[String]) {
    let mut load_shader = String::new();
    for s in shader_keys {
        let _ = writeln!(load_shader, "#include \"{}_gpu.h\"", shader_file(s));
        let _ = writeln!(load_shader, "#include \"{}.h\"", shader_file(s));
    }
    load_shader.push_str("ProgramLoader load_shader(const char* name) {\n");
    for s in shader_keys {
        let _ = writeln!(
            load_shader,
            "  if (!strcmp(name, \"{}\")) {{ return {}_program::loader; }}",
            s,
            shader_file(s)
        );
    }
    load_shader.push_str("  return nullptr;\n}\n");
    std::fs::write(
        std::env::var("OUT_DIR").unwrap() + "/load_shader.h",
        load_shader,
    )
    .unwrap();
}

fn process_imports(
    shader_dir: &str,
    shader: &str,
    included: &mut HashSet<String>,
    output: &mut String,
) {
    if !included.insert(shader.into()) {
        return;
    }
    println!("cargo:rerun-if-changed={shader_dir}/{shader}.glsl");
    let source = std::fs::read_to_string(format!("{shader_dir}/{shader}.glsl")).unwrap();
    for line in source.lines() {
        if let Some(imports) = line.strip_prefix("#include ") {
            let imports = imports.split(',');
            for import in imports {
                process_imports(shader_dir, import, included, output);
            }
        } else if line.starts_with("#version ") || line.starts_with("#extension ") {
            // ignore
        } else if line.trim_ascii_start().find('#').is_some() {
            let processed = line.replace("__VERSION__", "__SWGL_GLSL_VERSION__");
            output.push_str(&processed);
            output.push('\n');
        } else {
            output.push_str(line);
            output.push('\n');
        }
    }
}

fn translate_shader(
    shader_key: &str,
    shader_dir: &str,
    suppressed_env_vars: &mut Option<Vec<EnvVarGuard>>,
) {
    let mut imported = String::from("#define SWGL 1\n#define __SWGL_GLSL_VERSION__ 150\n");
    let _ = writeln!(
        imported,
        "#define WR_MAX_VERTEX_TEXTURE_WIDTH {}U",
        webrender_build::MAX_VERTEX_TEXTURE_WIDTH
    );

    let (basename, features) =
        shader_key.split_at(shader_key.find(' ').unwrap_or(shader_key.len()));
    if !features.is_empty() {
        for feature in features.trim().split(',') {
            let _ = writeln!(imported, "#define WR_FEATURE_{feature}");
        }
    }

    process_imports(shader_dir, basename, &mut HashSet::new(), &mut imported);

    let shader = shader_file(shader_key);

    let out_dir = std::env::var("OUT_DIR").unwrap();
    let imp_name = format!("{out_dir}/{shader}.c");
    std::fs::write(&imp_name, imported).unwrap();

    // We need to ensure that the C preprocessor does not pull compiler flags from the host or
    // target environment. Set all `CFLAGS` or `CXXFLAGS` env. vars. to empty to work around this.
    let _ = suppressed_env_vars.get_or_insert_with(|| {
        cflags_env_vars()
            .map(|(key, value)| {
                std::env::set_var(&key, "");
                EnvVarGuard {
                    key,
                    old_value: Some(value),
                }
            })
            .collect::<Vec<_>>()
    });

    let mut build = cc::Build::new();
    build.no_default_flags(true);
    if let Ok(tool) = build.try_get_compiler() {
        if tool.is_like_msvc() {
            build.flag("/EP");
            if tool.path().to_str().is_some_and(|p| p.contains("clang")) {
                build.flag("/clang:-undef");
            } else {
                build.flag("/u");
            }
        } else {
            build.flag("-xc").flag("-P").flag("-undef");
        }
    }
    build.file(&imp_name);
    let vs = build.clone().define("WR_VERTEX_SHADER", Some("1")).expand();
    let fs = build
        .clone()
        .define("WR_FRAGMENT_SHADER", Some("1"))
        .expand();
    let vs_name = format!("{out_dir}/{shader}.vert");
    let fs_name = format!("{out_dir}/{shader}.frag");
    std::fs::write(&vs_name, vs).unwrap();
    std::fs::write(&fs_name, fs).unwrap();

    let args = vec!["glsl_to_cxx".to_string(), vs_name, fs_name];
    let fxc = gpu_compiler();
    let (result, gpu, gpu_errors) =
        glsl_to_cxx::translate_with_gpu(&mut args.into_iter(), fxc.is_some());
    std::fs::write(format!("{out_dir}/{shader}.h"), result).unwrap();
    write_gpu_header(&out_dir, &shader, fxc.as_deref(), gpu, &gpu_errors);
}

/// fxc, when building for Windows: the fragment shaders are also compiled for
/// Direct3D feature level 9_3 so that SWGL can hand pixel shading to GPUs that
/// cannot run WebRender. SWGL_FXC names it explicitly (empty turns the GPU
/// path off); otherwise the newest Windows 10 SDK's is used. Without it the
/// GPU path is simply absent.
fn gpu_compiler() -> Option<String> {
    if std::env::var("CARGO_CFG_TARGET_OS").ok().as_deref() != Some("windows") {
        return None;
    }
    println!("cargo:rerun-if-env-changed=SWGL_FXC");
    if let Ok(fxc) = std::env::var("SWGL_FXC") {
        return if fxc.is_empty() { None } else { Some(fxc) };
    }
    let root = "C:/Program Files (x86)/Windows Kits/10/bin";
    let mut versions: Vec<_> = std::fs::read_dir(root)
        .ok()?
        .filter_map(|e| e.ok())
        .map(|e| e.path().join("x64").join("fxc.exe"))
        .filter(|p| p.exists())
        .collect();
    versions.sort();
    versions.pop().map(|p| p.to_string_lossy().into_owned())
}

fn fxc_compile(fxc: &str, hlsl: &str, profile: &str, out: &str) -> Result<Vec<u8>, String> {
    let src = format!("{out}.hlsl");
    let obj = format!("{out}.cso");
    std::fs::write(&src, hlsl).unwrap();
    let _ = std::fs::remove_file(&obj);
    let output = std::process::Command::new(fxc)
        .args(["/nologo", "/O3", "/T", profile, "/E", "main", "/Fo", &obj, &src])
        .output()
        .map_err(|e| e.to_string())?;
    match std::fs::read(&obj) {
        Ok(bytes) => Ok(bytes),
        Err(_) => Err(String::from_utf8_lossy(&output.stderr)
            .lines()
            .filter(|l| l.contains("error"))
            .take(2)
            .collect::<Vec<_>>()
            .join(" ")),
    }
}

fn bytes_array(name: &str, bytes: &[u8]) -> String {
    let mut s = format!("static const unsigned char {name}[{}] = {{", bytes.len());
    for (i, b) in bytes.iter().enumerate() {
        if i % 24 == 0 {
            s.push_str("\n ");
        }
        let _ = write!(s, "{},", b);
    }
    s.push_str("\n};\n");
    s
}

fn write_gpu_header(
    out_dir: &str,
    shader: &str,
    fxc: Option<&str>,
    gpu: Option<(Vec<String>, glsl_to_cxx::hlsl::Layout)>,
    errors: &[String],
) {
    let path = format!("{out_dir}/{shader}_gpu.h");
    let stub = format!("static const GpuProgramInfo* {shader}_gpu_info() {{ return nullptr; }}\n");
    let (fxc, (sources, layout)) = match (fxc, gpu) {
        (Some(f), Some(g)) => (f, g),
        (Some(_), None) => {
            println!("cargo:warning=swgl: {shader} stays on the CPU: {}", errors.join(" | "));
            std::fs::write(path, stub).unwrap();
            return;
        }
        _ => {
            std::fs::write(path, stub).unwrap();
            return;
        }
    };
    // Loops are unrolled; the first limit that fits in 512 instructions wins.
    let mut compiled = Err(String::new());
    for hlsl in &sources {
        compiled = fxc_compile(fxc, hlsl, "ps_4_0_level_9_3", &format!("{out_dir}/{shader}_ps"));
        if compiled.is_ok() {
            break;
        }
    }
    match compiled {
        Ok(bytes) => {
            let mut h = bytes_array(&format!("{shader}_ps"), &bytes);
            let rect: Vec<String> = layout
                .samplers
                .iter()
                .map(|s| {
                    let r = format!("{:?}", s.kind).contains("Rect");
                    (r as u8).to_string()
                })
                .chain(std::iter::once("0".to_string()))
                .collect();
            let _ = writeln!(
                h,
                "static const unsigned char {shader}_srect[] = {{ {} }};",
                rect.join(", ")
            );
            let _ = writeln!(
                h,
                "static const GpuProgramInfo {shader}_gpu_info_data = {{ {shader}_ps, sizeof({shader}_ps), {}, {}, {}, {}, {}, \"{shader}\", {shader}_srect }};",
                layout.slots,
                layout.texture_floats,
                layout.flat_coord_offset.map_or(-1, |o| o as i64),
                layout.fragcoord_offset.map_or(-1, |o| o as i64),
                layout.samplers.len(),
            );
            let _ = writeln!(
                h,
                "static const GpuProgramInfo* {shader}_gpu_info() {{ return &{shader}_gpu_info_data; }}"
            );
            std::fs::write(path, h).unwrap();
        }
        Err(e) => {
            println!("cargo:warning=swgl: {shader} stays on the CPU: fxc: {e}");
            std::fs::write(path, stub).unwrap();
        }
    }
}

/// The vertex half on the GPU does nothing but place the quad SWGL already
/// transformed: flip Y so that row 0 in memory is GL's row 0, as it is in
/// SWGL's buffers, and move depth from GL's [-1, 1] to Direct3D's [0, 1].
/// One per number of texture coordinates, plus a clear shader.
fn write_gpu_common(out_dir: &str) {
    let path = format!("{out_dir}/gpu_shaders.h");
    let fxc = match gpu_compiler() {
        Some(f) => f,
        None => {
            std::fs::write(path, "#define SWGL_GPU_SHADERS 0\n").unwrap();
            return;
        }
    };
    let mut h = String::from("#define SWGL_GPU_SHADERS 1\n");
    for slots in 1..=8 {
        let mut src = String::from("struct VIn { float4 pos : POSITION;");
        for i in 0..slots {
            let _ = write!(src, " float4 t{i} : TEXCOORD{i};");
        }
        // The texture coordinates come first: the pixel shaders declare only
        // those (9_3 cannot read SV_Position, and declaring it would cost one
        // of the eight inputs), and Direct3D links the stages by position,
        // so they must be a prefix of what this stage writes.
        src.push_str(" };\nstruct VOut {");
        for i in 0..slots {
            let _ = write!(src, " float4 t{i} : TEXCOORD{i};");
        }
        src.push_str(" float4 pos : SV_Position;");
        src.push_str(
            " };\nVOut main(VIn i) { VOut o;\n o.pos = float4(i.pos.x, -i.pos.y, (i.pos.z + i.pos.w) * 0.5, i.pos.w);\n",
        );
        for i in 0..slots {
            let _ = write!(src, " o.t{i} = i.t{i};");
        }
        src.push_str("\n return o; }\n");
        let bytes = fxc_compile(&fxc, &src, "vs_4_0_level_9_3", &format!("{out_dir}/gpu_vs{slots}"))
            .expect("the GPU vertex shader must compile");
        h.push_str(&bytes_array(&format!("swgl_gpu_vs{slots}"), &bytes));
    }
    let clear_vs = "float4 main(float4 pos : POSITION) : SV_Position { return float4(pos.x, -pos.y, (pos.z + pos.w) * 0.5, pos.w); }\n";
    let bytes = fxc_compile(&fxc, clear_vs, "vs_4_0_level_9_3", &format!("{out_dir}/gpu_clear_vs"))
        .expect("the GPU clear vertex shader must compile");
    h.push_str(&bytes_array("swgl_gpu_clear_vs", &bytes));
    let clear_ps =
        "cbuffer c : register(b0) { float4 color; };\nfloat4 main() : SV_Target { return color; }\n";
    let bytes = fxc_compile(&fxc, clear_ps, "ps_4_0_level_9_3", &format!("{out_dir}/gpu_clear_ps"))
        .expect("the GPU clear pixel shader must compile");
    h.push_str(&bytes_array("swgl_gpu_clear_ps", &bytes));
    std::fs::write(path, h).unwrap();
}

fn main() {
    let shader_dir = std::env::var("CARGO_MANIFEST_DIR").unwrap() + "/../webrender/res";

    let shader_flags = ShaderFeatureFlags::GL
        | ShaderFeatureFlags::DUAL_SOURCE_BLENDING
        | ShaderFeatureFlags::ADVANCED_BLEND_EQUATION
        | ShaderFeatureFlags::DEBUG;
    let mut shaders: Vec<String> = Vec::new();
    for (name, features) in get_shader_features(shader_flags) {
        shaders.extend(features.iter().map(|f| {
            if f.is_empty() {
                name.to_owned()
            } else {
                format!("{name} {f}")
            }
        }));
    }

    shaders.sort();

    let mut suppressed_env_vars = None;
    for shader in &shaders {
        translate_shader(shader, &shader_dir, &mut suppressed_env_vars);
    }
    drop(suppressed_env_vars); // Restore env. vars. for further compilation.

    write_load_shader(&shaders);
    write_gpu_common(&std::env::var("OUT_DIR").unwrap());

    println!("cargo:rerun-if-changed=src/blend.h");
    println!("cargo:rerun-if-changed=src/composite.h");
    println!("cargo:rerun-if-changed=src/gl_defs.h");
    println!("cargo:rerun-if-changed=src/glsl.h");
    println!("cargo:rerun-if-changed=src/program.h");
    println!("cargo:rerun-if-changed=src/rasterize.h");
    println!("cargo:rerun-if-changed=src/swgl_ext.h");
    println!("cargo:rerun-if-changed=src/texture.h");
    println!("cargo:rerun-if-changed=src/vector_type.h");
    println!("cargo:rerun-if-changed=src/gl.cc");
    println!("cargo:rerun-if-changed=src/gpu_d3d11.h");
    let mut build = cc::Build::new();
    build.cpp(true);

    let _suppressed_cflags_env_vars = cflags_env_vars()
        .filter_map(|(key, value)| {
            // NOTE: Cancels out [the `moz-check` plugin added in
            // `build/moz.configure/clang_plugin.configure`'s `clang_plugin_flags`
            // assignment][crossref].
            //
            // [crossref]: https://searchfox.org/mozilla-central/rev/f008b9aa2adf2dca6bdd49855b314cb3195f6f27/build/moz.configure/clang_plugin.configure#77-80
            const MOZ_CHECK_PLUGIN_LOAD_ARGS: &str = "-Xclang -add-plugin -Xclang moz-check";

            let replaced = value
                .to_str()
                .filter(|v| v.contains(MOZ_CHECK_PLUGIN_LOAD_ARGS))?
                .replace(MOZ_CHECK_PLUGIN_LOAD_ARGS, "");

            std::env::set_var(&key, replaced);
            Some(EnvVarGuard {
                key,
                old_value: Some(value),
            })
        })
        .collect::<Vec<_>>();

    if let Ok(tool) = build.try_get_compiler() {
        if tool.is_like_msvc() {
            build
                .flag("/std:c++20")
                .flag("/EHs-")
                .flag("/GR-");
        } else {
            build
                .flag("-std=c++20")
                .flag("-fno-exceptions")
                .flag("-fno-rtti")
                .flag("-fno-math-errno");
        }
        // SWGL relies heavily on inlining for performance so override -Oz with -O2
        if tool.args().contains(&"-Oz".into()) {
            build.flag("-O2");
        }

        // Most GLSL compilers assume something like fast-math so we turn it on.
        // However, reciprocal division makes it so 1/1 = 0.999994 which can produce a lot of fuzz
        // in reftests and the use of reciprocal instructions usually involves a refinement step
        // which bloats our already bloated code. Further, our shader code is sufficiently parallel
        // that we're more likely to be throughput bound vs latency bound. Having fewer
        // instructions makes things easier on the processor and in places where it matters we can
        // probably explicitly use reciprocal instructions and avoid the refinement step.
        // Also, allow checks for non-finite values which fast-math may disable.
        if tool.is_like_msvc() {
            build
                .flag("/fp:fast")
                .flag("-Xclang")
                .flag("-mrecip=none")
                .flag("/clang:-fno-finite-math-only");
        } else if tool.is_like_clang() {
            // gcc only supports -mrecip=none on some targets so to keep
            // things simple we don't use -ffast-math with gcc at all
            build
                .flag("-ffast-math")
                .flag("-mrecip=none")
                .flag("-fno-finite-math-only");
        }
    }

    build
        .file("src/gl.cc")
        .define("_GLIBCXX_USE_CXX11_ABI", Some("0"))
        .include(shader_dir)
        .include("src")
        .include(std::env::var("OUT_DIR").unwrap())
        .compile("gl_cc");
}

struct EnvVarGuard {
    key: std::ffi::OsString,
    old_value: Option<std::ffi::OsString>,
}

impl Drop for EnvVarGuard {
    fn drop(&mut self) {
        let Self { key, old_value } = &*self;
        if let Some(old_value) = old_value.as_ref() {
            std::env::set_var(key, old_value);
        } else {
            std::env::remove_var(key);
        }
    }
}

fn cflags_env_vars() -> impl Iterator<Item = (std::ffi::OsString, std::ffi::OsString)> {
    std::env::vars_os().filter(|(key, _value)| {
        key.to_str().is_some_and(|key_utf8| {
            ["CFLAGS", "CXXFLAGS"]
                .iter()
                .any(|opt_name| key_utf8.contains(opt_name))
        })
    })
}
