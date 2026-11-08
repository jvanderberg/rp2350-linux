fn main() {
    // memory.x / rp235x_riscv.x come from rp2350-board's build script
    println!("cargo:rustc-link-arg-bins=--nmagic");
    println!("cargo:rustc-link-search={}", std::env::var("CARGO_MANIFEST_DIR").unwrap());
    println!("cargo:rustc-link-arg-bin=rvemu-pico-shell=-Tcore1.x");
    println!("cargo:rustc-link-arg-bins=-Trp235x_riscv.x");
    println!("cargo:rerun-if-changed=../jittest-guest/jittest.bin");
    println!("cargo:rerun-if-changed=core1.x");
}
