CYW43439 firmware for the RM2 Wi-Fi module, copied unmodified from embassy's
`cyw43-firmware/` (https://github.com/embassy-rs/embassy), which took the
blobs from georgerobotics/cyw43-driver's `firmware/`. They are linked into
the board firmware and loaded into the radio; none of them is built here.

| File | What | Upstream |
|---|---|---|
| `43439A0.bin` | the radio's firmware | cyw43-driver `w43439A0_7_95_49_00_combined.h` (embassy, 2023-08-21) |
| `43439A0_clm.bin` | its CLM (regulatory) data | the same |
| `nvram_rp2040.bin` | the board's NVRAM settings; the one embassy's Pico 2 W example uses | cyw43-driver `wifi_nvram_43439.h`, marked "Copyright Broadcom Corporation" |

Licence: the Infineon Permissive Binary License 1.0
(`LICENSE-permissive-binary-license-1.0.txt`, as embassy ships it):
redistribution in binary form only, unmodified, with that text and its
disclaimer. Firmware built from this repository embeds the blobs, so pass
the licence on with it. The licence's "above copyright notice" is not in
the text upstream; the blobs are Cypress/Infineon's (the firmware's strings
name Broadcom).
