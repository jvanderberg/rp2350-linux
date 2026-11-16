# Contributing

## Checks

Run `scripts/check.sh host` before sending a change. It needs no guest
image or board. `scripts/check.sh emu`, `qemu` and `board` go further; `board`
flashes and reboots hardware. See the
[development and verification guide](docs/development.md).

## Licensing and attribution

### The project's own code

Original code in this repository is MIT ([LICENSE](LICENSE)). A new file
starts with:

```c
// SPDX-License-Identifier: MIT
```

in the file's own comment syntax (`#` for shell, Python and Make, `;` for
`.pio`).

### Kernel code

Everything under `guest/linux/` is built into Linux, so it is GPL-2.0-only,
not MIT ([guest/linux/COPYING](guest/linux/COPYING)):

- A driver starts with `// SPDX-License-Identifier: GPL-2.0-only` and has
  `MODULE_LICENSE("GPL");`, even when it is built in.
- A header only the kernel includes: `/* SPDX-License-Identifier: GPL-2.0-only */`.
- A uapi header, one that user space includes (`rvemu-pio.h`,
  `rvemu-crypto.h`, `rvemu-ssh.h`):
  `/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */`.
  Keep these to definitions (constants, structs, ioctl numbers), no code,
  so MIT programs such as libpio and dropbear can include them.
- A patch to a file of Linux's own takes the kernel's license; say so in
  the lines before its first `---`, which `patch` skips.
- Code brought into the kernel must be GPL-2.0-compatible: GPL-2.0,
  GPL-2.0-or-later, LGPL-2.1, MIT, BSD-2-Clause, BSD-3-Clause. Not
  GPL-3.0, Apache-2.0 alone, or anything with use restrictions.

### Code from other projects

Copying or porting code keeps its license. Before you add any:

1. **Check the license allows it** and fits where the code goes: kernel
   code as above; MIT, BSD, ISC, Zlib and Apache-2.0 are fine elsewhere.
   Ask first about GPL or LGPL code outside `guest/linux/` and Buildroot's
   own packages, and about any license that restricts use. Code with no
   license can't be used.
2. **Keep its copyright notice**, word for word, and add an SPDX line with
   its license. For a whole file:

   ```c
   // Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
   // SPDX-License-Identifier: BSD-3-Clause
   ```

   For part of a file, mark the part, and give the file its own
   `SPDX-License-Identifier: MIT` line at the top:

   ```c
   // SPDX-SnippetBegin
   // SPDX-SnippetCopyrightText: Copyright (c) Embassy project contributors
   // SPDX-License-Identifier: MIT OR Apache-2.0
   ... the copied code ...
   // SPDX-SnippetEnd
   ```

3. **Credit it in the file**, next to the code: the project, the file it
   came from, a URL, and what you changed. A README can credit it as well,
   but not instead.
4. **Include the license text** when the license requires it (MIT, BSD,
   Apache-2.0 and the GPLs all do): a `LICENSE-<name>` file next to the
   code, with the upstream copyright line. Examples:
   `guest/br-external/package/libpio/src/LICENSE-BSD-3-Clause`,
   `rvemu-rs/rvemu-pico/LICENSE-cyw43-pio`, `rp2350/LICENSE-rp-hal`. If the
   file is installed on its own, such as an example in the guest image,
   put the full notice in the file.
5. **Guest packages**: list every license in the package in its Buildroot
   `*_LICENSE` field (`LIBPIO_LICENSE = MIT, BSD-3-Clause`), and every
   license file in `*_LICENSE_FILES`. The files must be inside the
   package's source directory (`*_SITE`), which is what Buildroot copies.
6. **A vendored crate or library** keeps its own license files and a
   `PROVENANCE` file: where it came from, the version, the archive's hash,
   and what was changed (see `rvemu-rs/vendor/sntrup761`). Each changed
   file says so at the top.

If you can't tell where code came from, or its license doesn't fit, don't
add it: raise it in the pull request.

Test vectors from RFCs and similar documents: cite the document and
section next to the data.

### Binaries and firmware blobs

Commit a binary only when it can't be built from this repository.

- Put the license and notices next to it, with a README or a
  `.NOTICE.txt`: where it came from (URL, version or commit), who holds
  the copyright, and the license text. See `rvemu-rs/rvemu-pico/firmware/`
  and `rp2350/pintester/debugprobe-v2.3.1-backup.NOTICE.txt`.
- Check the license allows redistribution. Firmware licenses often allow
  it only unmodified and in binary form; don't change the blob.
- If the board firmware embeds the blob, its license travels with the
  firmware: anyone distributing a built image must pass it on.
- A binary built from this repository's own source (`jittest.bin`) is
  covered by the source's license; say how to rebuild it.
