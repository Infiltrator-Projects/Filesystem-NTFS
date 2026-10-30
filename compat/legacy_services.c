/* SPDX-License-Identifier: GPL-3.0-or-later */
/* First-party native SFS code calls host-owned generic functions through a typed ABI. */
#include "ifs_defrag_services_v1.h"
#include "ld_io.h"
#include "ld_device.h"
#include "ld_path.h"
#include "ld_runtime.h"
#include "ld_protocol.h"
#include "ld_stop.h"
#include <errno.h>
#include <stdlib.h>
static _Thread_local const IfsDefragServicesV1 *active;
int ifs_bind_services(const IfsDefragServicesV1 *s) {
    if (!s || active || s->abi_version != IFS_DEFRAG_SERVICES_V1 ||
        s->structure_bytes < sizeof(*s) ||
        !s->ld_pread_full ||
        !s->ld_pwrite_full ||
        !s->ld_bitmap_size ||
        !s->ld_bitmap_calloc ||
        !s->ld_bitmap_get ||
        !s->ld_bitmap_set ||
        !s->ld_sync_fd ||
        !s->ld_fd_size_bytes ||
        !s->ld_device_capture_binding ||
        !s->ld_device_open_verified_fd ||
        !s->ld_path_is_mounted ||
        !s->ld_path_parent_directory ||
        !s->ld_path_append_suffix ||
        !s->ld_path_is_derived_from ||
        !s->ld_path_ensure_trusted_directory_tree ||
        !s->ld_path_require_staging_capacity ||
        !s->ld_xstrdup ||
        !s->ld_emit_result_event ||
        !s->ld_stop_requested ||
        !s->ld_stop_clear ||
        !s->ld_stop_install_handlers ||
        !s->ld_stop_report_ready ||
        !s->ld_die || !s->ld_xstrndup ||
        !s->ld_device_number_is_mounted || !s->ld_fd_matches_identity ||
        !s->ld_device_open || !s->ld_device_close) {
        errno=EINVAL; return -1;
    }
    active=s; return 0;
}
void ifs_unbind_services(void) { active=NULL; }
ssize_t ld_pread_full(int fd, void *p, size_t n, uint64_t at) { if(active) return active->ld_pread_full(fd,p,n,at); errno=ENOSYS; return -1; }
ssize_t ld_pwrite_full(int fd, const void *p, size_t n, uint64_t at) { if(active) return active->ld_pwrite_full(fd,p,n,at); errno=ENOSYS; return -1; }
bool ld_bitmap_size(uint64_t bits, size_t *bytes) { if(active) return active->ld_bitmap_size(bits,bytes); errno=ENOSYS; return false; }
uint8_t * ld_bitmap_calloc(uint64_t bits) { if(active) return active->ld_bitmap_calloc(bits); errno=ENOSYS; return NULL; }
bool ld_bitmap_get(const uint8_t *p, uint64_t bit) { if(active) return active->ld_bitmap_get(p,bit); errno=ENOSYS; return false; }
void ld_bitmap_set(uint8_t *p, uint64_t bit, bool value) { if(active) active->ld_bitmap_set(p,bit,value); }
int ld_sync_fd(int fd) { if(active) return active->ld_sync_fd(fd); errno=ENOSYS; return -1; }
int ld_fd_size_bytes(int fd, uint64_t *bytes) { if(active) return active->ld_fd_size_bytes(fd,bytes); errno=ENOSYS; return -1; }
int ld_device_capture_binding(const char *path, char **canonical, char **identity, uint64_t *bytes) { if(active) return active->ld_device_capture_binding(path,canonical,identity,bytes); errno=ENOSYS; return -1; }
int ld_device_open_verified_fd(const char *path, bool writable, const char *identity, uint64_t bytes) { if(active) return active->ld_device_open_verified_fd(path,writable,identity,bytes); errno=ENOSYS; return -1; }
bool ld_path_is_mounted(const char *path) { if(active) return active->ld_path_is_mounted(path); errno=ENOSYS; return true; }
char * ld_path_parent_directory(const char *path) { if(active) return active->ld_path_parent_directory(path); errno=ENOSYS; return NULL; }
char * ld_path_append_suffix(const char *base, const char *suffix) { if(active) return active->ld_path_append_suffix(base,suffix); errno=ENOSYS; return NULL; }
bool ld_path_is_derived_from(const char *candidate, const char *base, const char *suffix) { if(active) return active->ld_path_is_derived_from(candidate,base,suffix); errno=ENOSYS; return false; }
int ld_path_ensure_trusted_directory_tree(const char *path) { if(active) return active->ld_path_ensure_trusted_directory_tree(path); errno=ENOSYS; return -1; }
int ld_path_require_staging_capacity(const char *path, uint64_t n, uint64_t *available, uint64_t *required) { if(active) return active->ld_path_require_staging_capacity(path,n,available,required); errno=ENOSYS; return -1; }
char * ld_xstrdup(const char *s) { if(active) return active->ld_xstrdup(s); errno=ENOSYS; return NULL; }
int ld_emit_result_event(FILE *stream, const char *operation, const char *status, const char *message) { if(active) return active->ld_emit_result_event(stream,operation,status,message); errno=ENOSYS; return -1; }
bool ld_stop_requested(void) { if(active) return active->ld_stop_requested(); errno=ENOSYS; return true; }
void ld_stop_clear(void) { if(active) active->ld_stop_clear(); }
void ld_stop_install_handlers(void) { if(active) active->ld_stop_install_handlers(); }
void ld_stop_report_ready(void) { if(active) active->ld_stop_report_ready(); }

_Noreturn void ld_die(const char *msg) {
 if (active) active->ld_die(msg);
 /* The host's failure hook must not return. */
 _Exit(2);
}
char *ld_xstrndup(const char *p, size_t n) {
 if(active) return active->ld_xstrndup(p,n);
 errno=ENOSYS; return NULL;
}
bool ld_device_number_is_mounted(dev_t dev) {
 if(active) return active->ld_device_number_is_mounted(dev);
 errno=ENOSYS; return true;
}
bool ld_fd_matches_identity(int fd,const char *id,uint64_t n) {
 if(active) return active->ld_fd_matches_identity(fd,id,n);
 errno=ENOSYS; return false;
}
LdDevice ld_device_open(const char *path,bool writable) {
 if(active) return active->ld_device_open(path,writable);
 _Exit(2);
}
void ld_device_close(LdDevice *p) {
 if(active) active->ld_device_close(p);
}
