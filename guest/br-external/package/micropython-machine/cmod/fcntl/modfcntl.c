// fcntl.ioctl(fd, request, arg=0) for MicroPython's unix port, as CPython's:
// fd is an int or has fileno(); arg is an int, or a writable buffer the
// call reads and fills in place. Returns ioctl's result; raises OSError.
#include <errno.h>
#include <sys/ioctl.h>

#include "py/runtime.h"

static mp_obj_t fcntl_ioctl(size_t n_args, const mp_obj_t *args) {
    mp_obj_t f = args[0];
    if (!mp_obj_is_int(f)) {
        f = mp_call_function_0(mp_load_attr(f, MP_QSTR_fileno));
    }
    int fd = mp_obj_get_int(f);
    unsigned long req = mp_obj_get_int_truncated(args[1]);
    int r;
    if (n_args < 3 || mp_obj_is_int(args[2])) {
        r = ioctl(fd, req, n_args < 3 ? 0L : (long)mp_obj_get_int_truncated(args[2]));
    } else {
        mp_buffer_info_t b;
        mp_get_buffer_raise(args[2], &b, MP_BUFFER_RW);
        r = ioctl(fd, req, b.buf);
    }
    if (r == -1) {
        mp_raise_OSError(errno);
    }
    return mp_obj_new_int(r);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(fcntl_ioctl_obj, 2, 3, fcntl_ioctl);

static const mp_rom_map_elem_t fcntl_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_fcntl) },
    { MP_ROM_QSTR(MP_QSTR_ioctl), MP_ROM_PTR(&fcntl_ioctl_obj) },
};
static MP_DEFINE_CONST_DICT(fcntl_globals, fcntl_globals_table);

const mp_obj_module_t fcntl_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&fcntl_globals,
};
MP_REGISTER_MODULE(MP_QSTR_fcntl, fcntl_module);
