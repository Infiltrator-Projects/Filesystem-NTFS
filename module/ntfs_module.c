/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_ntfs_module_v1.h"
int ifs_ntfs_worker_main(int, char **);
int ifs_bind_services(const IfsDefragServicesV1 *);
void ifs_unbind_services(void);
static int run(int argc, char **argv, const IfsDefragServicesV1 *services) {
 if (ifs_bind_services(services)!=0) return 1;
 int result=ifs_ntfs_worker_main(argc,argv);
 ifs_unbind_services();
 return result;
}
int ifs_ntfs_fixture_run(const void *,const char *,int,const void *);
static int fixture(const void *spec,const char *path,int create,
 const IfsDefragServicesV1 *services,const IfsNtfsFixtureHostV1 *host) {
 if(ifs_bind_services(services)!=0)return -1;
 int result=ifs_ntfs_fixture_run(spec,path,create,host);
 ifs_unbind_services();
 return result;
}
static const IfsNtfsModuleV1 api = {
 IFS_NTFS_MODULE_V1,sizeof(IfsNtfsModuleV1),"ntfs",IFS_NTFS_CAP_RUN | IFS_NTFS_CAP_FIXTURE,run,fixture
};
const IfsNtfsModuleV1 *infiltrator_filesystem_ntfs_v1(void) { return &api; }
