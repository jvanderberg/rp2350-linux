fn main() {
    // memory.x / rp235x_riscv.x come from rp2350-board's build script
    println!("cargo:rustc-link-arg-bins=--nmagic");
    println!("cargo:rustc-link-arg-bins=-Trp235x_riscv.x");
    println!("cargo:rerun-if-changed=../jittest-guest/jittest.bin");
}
