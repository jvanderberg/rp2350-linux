// _rvmem: a device's memory in a MicroPython program on rp2350-linux (the
// unix port's machine.mem32 goes through /dev/mem, which this guest has
// not): mmap a device file, and 32-bit register reads and writes there.
//   mmap(fd, length, offset=0) -> address     munmap(address, length)
//   read32(address) -> int                     write32(address, value)
// rp2.py maps /dev/pio1 and /dev/pio2 with it.
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>

#include "py/runtime.h"

static mp_obj_t rvmem_mmap(size_t n_args, const mp_obj_t *args) {
    int fd = mp_obj_get_int(args[0]);
    size_t len = mp_obj_get_int_truncated(args[1]);
    off_t off = n_args > 2 ? (off_t)mp_obj_get_int_truncated(args[2]) : 0;
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
    if (p == MAP_FAILED) {
        mp_raise_OSError(errno);
    }
    return mp_obj_new_int_from_uint((uintptr_t)p);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(rvmem_mmap_obj, 2, 3, rvmem_mmap);

static mp_obj_t rvmem_munmap(mp_obj_t addr, mp_obj_t len) {
    if (munmap((void *)(uintptr_t)mp_obj_get_int_truncated(addr), mp_obj_get_int_truncated(len)) < 0) {
        mp_raise_OSError(errno);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(rvmem_munmap_obj, rvmem_munmap);

static mp_obj_t rvmem_read32(mp_obj_t addr) {
    uintptr_t a = mp_obj_get_int_truncated(addr);
    if (a & 3) {
        mp_raise_ValueError(MP_ERROR_TEXT("unaligned"));
    }
    return mp_obj_new_int_from_uint(*(volatile uint32_t *)a);
}
static MP_DEFINE_CONST_FUN_OBJ_1(rvmem_read32_obj, rvmem_read32);

static mp_obj_t rvmem_write32(mp_obj_t addr, mp_obj_t value) {
    uintptr_t a = mp_obj_get_int_truncated(addr);
    if (a & 3) {
        mp_raise_ValueError(MP_ERROR_TEXT("unaligned"));
    }
    *(volatile uint32_t *)a = (uint32_t)mp_obj_get_int_truncated(value);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(rvmem_write32_obj, rvmem_write32);

static const mp_rom_map_elem_t rvmem_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__rvmem) },
    { MP_ROM_QSTR(MP_QSTR_mmap), MP_ROM_PTR(&rvmem_mmap_obj) },
    { MP_ROM_QSTR(MP_QSTR_munmap), MP_ROM_PTR(&rvmem_munmap_obj) },
    { MP_ROM_QSTR(MP_QSTR_read32), MP_ROM_PTR(&rvmem_read32_obj) },
    { MP_ROM_QSTR(MP_QSTR_write32), MP_ROM_PTR(&rvmem_write32_obj) },
};
static MP_DEFINE_CONST_DICT(rvmem_globals, rvmem_globals_table);

const mp_obj_module_t rvmem_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&rvmem_globals,
};
MP_REGISTER_MODULE(MP_QSTR__rvmem, rvmem_module);
