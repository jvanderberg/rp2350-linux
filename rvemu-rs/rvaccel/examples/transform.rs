fn main() {
    let a: Vec<_> = std::env::args().collect();
    assert_eq!(a.len(), 5, "FID INPUT OUTPUT PARAMS");
    let input = std::fs::read(&a[2]).unwrap();
    let params = std::fs::read(&a[4]).unwrap();
    let mut output = vec![0; rvaccel::MAX_BUFFER];
    let n = rvaccel::run(a[1].parse().unwrap(), &input, &mut output, &params).unwrap();
    std::fs::write(&a[3], &output[..n]).unwrap();
}
