//! Put memory.x and rp235x_riscv.x (from rp-hal's examples) on the linker
//! search path of every binary that depends on this crate. Binaries still
//! pass `-Trp235x_riscv.x` themselves (link args do not propagate).
use std::{env, fs, path::PathBuf};

fn main() {
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    for f in ["memory.x", "rp235x_riscv.x"] {
        fs::copy(f, out.join(f)).unwrap();
        println!("cargo:rerun-if-changed={f}");
    }
    println!("cargo:rustc-link-search={}", out.display());
    println!("cargo:rerun-if-changed=build.rs");
}
