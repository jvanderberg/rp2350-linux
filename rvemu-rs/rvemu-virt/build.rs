fn main() {
    let dir = std::env::var("CARGO_MANIFEST_DIR").unwrap();
    println!("cargo:rustc-link-arg=-T{dir}/link.x");
    println!("cargo:rerun-if-changed=link.x");
    println!("cargo:rerun-if-env-changed=RVEMU_CODE_KB");
    println!("cargo:rerun-if-env-changed=RVEMU_NJC");
    println!("cargo:rerun-if-env-changed=RVEMU_REGIONS");
    println!("cargo:rerun-if-env-changed=RVEMU_STORE_KB");
}
