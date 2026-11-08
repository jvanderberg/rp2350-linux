# Source from repository scripts; no work is performed on import.
need_tool() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "Missing tool: $1. $2" >&2; exit 1;
  }
}
need_file() {
  [ -s "$1" ] || { echo "Missing or empty file: $1. $2" >&2; exit 1; }
}
need_target() {
  need_tool rustup 'Install Rust with rustup.'
  rustup target list --installed | grep -qx "$1" || {
    echo "Missing Rust target: $1. Run: rustup target add $1" >&2; exit 1;
  }
}
