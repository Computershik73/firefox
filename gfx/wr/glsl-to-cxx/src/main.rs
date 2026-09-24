/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use glsl_to_cxx::translate;
use std::io::Write;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    // glsl-to-cxx --hlsl <out dir> <file.frag>...: the feature level 9_3
    // pixel shaders, one .hlsl per preprocessed fragment shader, and a line
    // per shader saying whether it translated and how its varyings are laid
    // out. Compile the results with fxc /T ps_4_0_level_9_3.
    if args.len() >= 3 && args[1] == "--hlsl" {
        let out_dir = &args[2];
        for f in &args[3..] {
            let stem = std::path::Path::new(f)
                .file_stem()
                .unwrap()
                .to_string_lossy()
                .to_string();
            match glsl_to_cxx::hlsl::translate_fragment_file(f) {
                Ok((src, layout)) => {
                    std::fs::File::create(format!("{}/{}.hlsl", out_dir, stem))
                        .unwrap()
                        .write_all(src.as_bytes())
                        .unwrap();
                    println!(
                        "OK   {} slots {} varying floats {} texture floats {} fragcoord {:?} samplers {}",
                        stem,
                        layout.slots,
                        layout.varying_floats,
                        layout.texture_floats,
                        layout.fragcoord_offset,
                        layout.samplers.len()
                    );
                }
                Err(errs) => {
                    let msgs: Vec<&str> = errs
                        .iter()
                        .filter(|e| !e.starts_with("-- partial"))
                        .map(|s| s.as_str())
                        .collect();
                    println!("FAIL {} : {}", stem, msgs.join(" | "));
                }
            }
        }
        return;
    }
    println!("{}", translate(&mut std::env::args()));
}
