#ifndef INIT_MOUNT_H
#define INIT_MOUNT_H

int mount_pseudo_filesystems(void);
int mount_root_filesystem(void);
int remount_root_rw(void);
void setup_basic_environment(void);

#endif