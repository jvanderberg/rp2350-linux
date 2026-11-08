//! A stand-in network for testing the virtio-net device on the Mac without
//! privileges: one "gateway" at 10.0.2.2 that answers ARP and ICMP echo.
//! Inside the guest: `ifconfig eth0 10.0.2.15 up && ping -c 3 10.0.2.2`.

use std::collections::VecDeque;

use rvcore::virtio::{FRAME_MAX, NetLink};

const GUEST_MAC: [u8; 6] = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
const GW_MAC: [u8; 6] = [0x52, 0x54, 0x00, 0x12, 0x34, 0x02];
const GW_IP: [u8; 4] = [10, 0, 2, 2];

#[derive(Default)]
pub struct StubNet {
    rx: VecDeque<Vec<u8>>,
    pub tx_frames: u64,
}

/// The internet checksum of `b`.
fn csum(b: &[u8]) -> u16 {
    let mut s: u32 = b.chunks(2).map(|c| u32::from(c[0]) << 8 | u32::from(*c.get(1).unwrap_or(&0))).sum();
    while s > 0xFFFF {
        s = (s & 0xFFFF) + (s >> 16);
    }
    !(s as u16)
}

impl StubNet {
    fn reply(&self, f: &[u8]) -> Option<Vec<u8>> {
        let ethertype = u16::from_be_bytes([*f.get(12)?, *f.get(13)?]);
        let mut r = f.to_vec();
        r[0..6].copy_from_slice(&f[6..12]);
        r[6..12].copy_from_slice(&GW_MAC);
        match ethertype {
            // ARP request (Ethernet/IPv4) for the gateway
            0x0806 if f.len() >= 42 && f[20..22] == [0, 1] && f[38..42] == GW_IP => {
                r[21] = 2;
                r[22..28].copy_from_slice(&GW_MAC);
                r[28..32].copy_from_slice(&GW_IP);
                r[32..42].copy_from_slice(&f[22..32]);
                Some(r)
            }
            // ICMP echo request to the gateway
            0x0800 if f.len() >= 34 && f[23] == 1 && f[30..34] == GW_IP => {
                let ihl = usize::from(f[14] & 0xF) * 4;
                let total = usize::from(u16::from_be_bytes([f[16], f[17]]));
                let (ip, end) = (14, 14 + total);
                if end > f.len() || ihl < 20 || f.get(ip + ihl) != Some(&8) {
                    return None;
                }
                r.truncate(end);
                r[26..30].copy_from_slice(&f[30..34]);
                r[30..34].copy_from_slice(&f[26..30]);
                r[24..26].fill(0);
                let c = csum(&r[ip..ip + ihl]);
                r[24..26].copy_from_slice(&c.to_be_bytes());
                let icmp = ip + ihl;
                r[icmp] = 0;
                r[icmp + 2..icmp + 4].fill(0);
                let c = csum(&r[icmp..end]);
                r[icmp + 2..icmp + 4].copy_from_slice(&c.to_be_bytes());
                Some(r)
            }
            _ => None,
        }
    }
}

impl NetLink for StubNet {
    fn mac(&self) -> [u8; 6] {
        GUEST_MAC
    }
    fn send(&mut self, frame: &[u8]) {
        self.tx_frames += 1;
        if let Some(r) = self.reply(frame) {
            self.rx.push_back(r);
        }
    }
    fn recv(&mut self, buf: &mut [u8; FRAME_MAX]) -> Option<usize> {
        let f = self.rx.pop_front()?;
        let n = f.len().min(FRAME_MAX);
        buf[..n].copy_from_slice(&f[..n]);
        Some(n)
    }
}
