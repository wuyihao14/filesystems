/*
 * procfs as a MacFUSE file system for Mac OS X
 *
 * Copyright Amit Singh. All Rights Reserved.
 * http://osxbook.com
 *
 * http://code.google.com/p/macfuse/
 *
 * Source License: GNU GENERAL PUBLIC LICENSE (GPL)
 */

#define MACFUSE_PROCFS_VERSION "1.0"
#define FUSE_USE_VERSION 26

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <dirent.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <Carbon/Carbon.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

#include <grp.h>
#include <pwd.h>

#include <cassert>
#include <vector>
#include <pcrecpp.h>
#include <fuse.h>

#if MACFUSE_PROCFS_ENABLE_TPM
#include "procfs_tpm.h"
#endif /* MACFUSE_PROCFS_ENABLE_TPM */

static mach_port_t p_default_set = 0;
static mach_port_t p_default_set_control = 0;
static host_priv_t host_priv;
static processor_port_array_t processor_list;
static natural_t processor_count = 0;
static int total_file_patterns = 0;
static int total_directory_patterns = 0;
static int total_link_patterns = 0;
static io_connect_t lightsensor_port = 0;
static io_connect_t motionsensor_port = 0;
static unsigned int sms_gIndex = 0;
static IOItemCount sms_gStructureInputSize = 0;
static IOByteCount sms_gStructureOutputSize = 0;

static pcrecpp::RE *valid_process_pattern = new pcrecpp::RE("/(\\d+)");

typedef struct {
    char x;
    char y;
    char z;
    short v;
#define FILLER_SIZE 60
    char scratch[FILLER_SIZE];
} MotionSensorData_t;

static kern_return_t
sms_getOrientation_hardware_apple(MotionSensorData_t *odata)
{
    kern_return_t      kr;
    IOItemCount        isize = sms_gStructureInputSize;
    IOByteCount        osize = sms_gStructureOutputSize;
    MotionSensorData_t idata;

    kr = IOConnectMethodStructureIStructureO(motionsensor_port,
                                             sms_gIndex,
                                             isize,
                                             &osize,
                                             &idata,
                                             odata);
    return kr;
}

static int init_task_list(task_array_t           *task_list,
                          mach_msg_type_number_t *task_count)
{
    return processor_set_tasks(p_default_set_control, task_list, task_count);
}

static void fini_task_list(task_array_t           task_list,
                           mach_msg_type_number_t task_count)
{
    unsigned int i;
    for (i = 0; i < task_count; i++) {
        mach_port_deallocate(mach_task_self(), task_list[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)task_list,
                  task_count * sizeof(task_t));
}

static int init_thread_list(task_t                  the_task,
                            thread_array_t         *thread_list,
                            mach_msg_type_number_t *thread_count)
{
    return task_threads(the_task, thread_list, thread_count);
}

static void fini_thread_list(thread_array_t         thread_list,
                             mach_msg_type_number_t thread_count)
{
    unsigned int i;
    for (i = 0; i < thread_count; i++) {
        mach_port_deallocate(mach_task_self(), thread_list[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)thread_list,
                  thread_count * sizeof(thread_act_t));
}

static int init_port_list(task_t                  the_task,
                          mach_port_name_array_t *name_list,
                          mach_msg_type_number_t *name_count,
                          mach_port_type_array_t *type_list,
                          mach_msg_type_number_t *type_count)
{
    return mach_port_names(the_task,
                           name_list, name_count, type_list, type_count);
}

static void fini_port_list(mach_port_name_array_t name_list,
                           mach_msg_type_number_t name_count,
                           mach_port_type_array_t type_list,
                           mach_msg_type_number_t type_count)
{
    vm_deallocate(mach_task_self(), (vm_address_t)name_list,
                  name_count * sizeof(mach_port_name_t));
    vm_deallocate(mach_task_self(), (vm_address_t)type_list,
                  type_count * sizeof(mach_port_type_t));
}
                           

#define DECL_PORT_LIST() \
    mach_port_name_array_t name_list;  \
    mach_msg_type_number_t name_count; \
    mach_port_type_array_t type_list;  \
    mach_msg_type_number_t type_count;
#define INIT_PORT_LIST(the_task) \
    if (init_port_list(the_task, &name_list, &name_count, &type_list, &type_count) != 0) { \
        return -EIO; \
    }
#define FINI_PORT_LIST() \
    fini_port_list(name_list, name_count, type_list, type_count)

#define DECL_TASK_LIST() \
    task_array_t           task_list; \
    mach_msg_type_number_t task_count;
#define INIT_TASK_LIST() \
    if (init_task_list(&task_list, &task_count) != 0) { return -EIO; }
#define FINI_TASK_LIST() \
    fini_task_list(task_list, task_count)

#define DECL_THREAD_LIST() \
    thread_array_t         thread_list; \
    mach_msg_type_number_t thread_count;
#define INIT_THREAD_LIST(the_task) \
    if (init_thread_list(the_task, &thread_list, &thread_count) != 0) { \
        return -EIO; \
    }
#define FINI_THREAD_LIST() \
    fini_thread_list(thread_list, thread_count)

struct procfs_dispatcher_entry;
typedef struct procfs_dispatcher_entry * procfs_dispatcher_entry_t;

typedef int (*procfs_getattr_handler_t)(procfs_dispatcher_entry_t  e,
                                        const char                *argv[],
                                        struct stat               *stbuf);

typedef int (*procfs_read_handler_t)(procfs_dispatcher_entry_t  e,
                                     const char                *argv[],
                                     char                      *buf,
                                     size_t                     size,
                                     off_t                      offset,
                                     struct fuse_file_info     *fi);

typedef int (*procfs_readdir_handler_t)(procfs_dispatcher_entry_t  e,
                                        const char                *argv[],
                                        void                      *buf,
                                        fuse_fill_dir_t            filler,
                                        off_t                      offset,
                                        struct fuse_file_info     *fi);

typedef int (*procfs_readlink_handler_t)(procfs_dispatcher_entry_t  e,
                                         const char                *argv[],
                                         char                      *buf,
                                         size_t                    size);

typedef struct procfs_dispatcher_entry {
    char                     *pattern;
    pcrecpp::RE              *compiled_pattern;
    int                       argc;
    procfs_getattr_handler_t  getattr;
    procfs_read_handler_t     read;
    procfs_readdir_handler_t  readdir;
    procfs_readlink_handler_t readlink;
    const char               *content_files[32];
    const char               *content_directories[32];
};

#define PROCFS_MAX_ARGS       3

#define GETATTR_HANDLER(handler) \
int \
procfs_getattr_##handler(procfs_dispatcher_entry_t  e,      \
                         const char                *argv[], \
                         struct stat               *stbuf)  \

#define READ_HANDLER(handler) \
int \
procfs_read_##handler(procfs_dispatcher_entry_t  e,      \
                      const char                *argv[], \
                      char                      *buf,    \
                      size_t                     size,   \
                      off_t                      offset, \
                      struct fuse_file_info     *fi)     \

#define READDIR_HANDLER(handler) \
int \
procfs_readdir_##handler(procfs_dispatcher_entry_t  e,      \
                         const char                *argv[], \
                         void                      *buf,    \
                         fuse_fill_dir_t            filler, \
                         off_t                      offset, \
                         struct fuse_file_info     *fi)     \

#define READLINK_HANDLER(handler) \
int \
procfs_readlink_##handler(procfs_dispatcher_entry_t  e,      \
                          const char                *argv[], \
                          char                      *buf,    \
                          size_t                     size)   \

#define PROTO_READ_HANDLER(handler)     READ_HANDLER(handler)
#define PROTO_READDIR_HANDLER(handler)  READDIR_HANDLER(handler)
#define PROTO_READLINK_HANDLER(handler) READLINK_HANDLER(handler)
#define PROTO_GETATTR_HANDLER(handler)  GETATTR_HANDLER(handler)

#define DECL_FILE(pattern, argc, getattrp, readp) \
    {                              \
        pattern,                   \
        new pcrecpp::RE(pattern),  \
        argc,                      \
        procfs_getattr_##getattrp, \
        procfs_read_##readp,       \
        procfs_readdir_enotdir,    \
        procfs_readlink_einval,    \
        { NULL },                  \
        { NULL }                   \
    },

#define DECL_DIRECTORY(pattern, argc, getattrp, readdirp, contents, ...) \
    {                              \
        pattern,                   \
        new pcrecpp::RE(pattern),  \
        argc,                      \
        procfs_getattr_##getattrp, \
        procfs_read_eisdir,        \
        procfs_readdir_##readdirp, \
        procfs_readlink_einval,    \
        contents,                  \
        __VA_ARGS__                \
    },

#define DECL_DIRECTORY_COMPACT(pattern, contents, ...) \
    {                                     \
        pattern,                          \
        new pcrecpp::RE(pattern),         \
        0,                                \
        procfs_getattr_default_directory, \
        procfs_read_eisdir,               \
        procfs_readdir_default,           \
        procfs_readlink_einval,           \
        contents,                         \
        ##__VA_ARGS__                     \
    },

#define DECL_LINK(pattern, argc, getattrp, readlinkp) \
    {                                \
        pattern,                     \
        new pcrecpp::RE(pattern),    \
        argc,                        \
        procfs_getattr_##getattrp,   \
        procfs_read_einval,          \
        procfs_readdir_enotdir,      \
        procfs_readlink_##readlinkp, \
        { NULL },                    \
        { NULL }                     \
    },

#define DECL_LINK_COMPACT(pattern, argc, readlinkp) \
    {                                \
        pattern,                     \
        new pcrecpp::RE(pattern),    \
        argc,                        \
        procfs_getattr_default_link, \
        procfs_read_einval,          \
        procfs_readdir_enotdir,      \
        procfs_readlink_##readlinkp, \
        { NULL },                    \
        { NULL }                     \
    },

PROTO_READ_HANDLER(einval);
PROTO_READ_HANDLER(eisdir);
PROTO_READ_HANDLER(proc__carbon);
PROTO_READ_HANDLER(proc__generic);
PROTO_READ_HANDLER(proc__task__absolutetime_info);
PROTO_READ_HANDLER(proc__task__basic_info);
PROTO_READ_HANDLER(proc__task__events_info);
PROTO_READ_HANDLER(proc__task__thread_times_info);
PROTO_READ_HANDLER(proc__task__mach_name);
PROTO_READ_HANDLER(proc__task__ports__port);
PROTO_READ_HANDLER(proc__task__role);
PROTO_READ_HANDLER(proc__task__threads__thread__basic_info);
PROTO_READ_HANDLER(proc__task__threads__thread__states__debug);
PROTO_READ_HANDLER(proc__task__threads__thread__states__exception);
PROTO_READ_HANDLER(proc__task__threads__thread__states__float);
PROTO_READ_HANDLER(proc__task__threads__thread__states__thread);
PROTO_READ_HANDLER(proc__task__tokens);
PROTO_READ_HANDLER(proc__task__vmmap);
PROTO_READ_HANDLER(proc__xcred);

PROTO_READ_HANDLER(hardware__xsensor);
PROTO_READ_HANDLER(hardware__cpus__cpu__data);
#if MACFUSE_PROCFS_ENABLE_TPM
PROTO_READ_HANDLER(hardware__tpm__hwmodel);
PROTO_READ_HANDLER(hardware__tpm__hwvendor);
PROTO_READ_HANDLER(hardware__tpm__hwversion);
PROTO_READ_HANDLER(hardware__tpm__keyslots__slot);
PROTO_READ_HANDLER(hardware__tpm__pcrs__pcr);
#endif /* MACFUSE_PROCFS_ENABLE_TPM */

PROTO_READDIR_HANDLER(default);
PROTO_READDIR_HANDLER(enotdir);
PROTO_READDIR_HANDLER(proc__task__ports);
PROTO_READDIR_HANDLER(proc__task__threads);
PROTO_READDIR_HANDLER(root);
PROTO_READDIR_HANDLER(hardware__cpus);
PROTO_READDIR_HANDLER(hardware__cpus__cpu);
#if MACFUSE_PROCFS_ENABLE_TPM
PROTO_READDIR_HANDLER(hardware__tpm__keyslots);
PROTO_READDIR_HANDLER(hardware__tpm__pcrs);
#endif /* MACFUSE_PROCFS_ENABLE_TPM */

PROTO_GETATTR_HANDLER(default_file);
PROTO_GETATTR_HANDLER(default_directory);
PROTO_GETATTR_HANDLER(default_link);
#if MACFUSE_PROCFS_ENABLE_TPM
PROTO_GETATTR_HANDLER(hardware__tpm__keyslots__slot);
PROTO_GETATTR_HANDLER(hardware__tpm__pcrs__pcr);
#endif /* MACFUSE_PROCFS_ENABLE_TPM */

PROTO_READLINK_HANDLER(einval);

static struct procfs_dispatcher_entry
procfs_link_table[] = {
};

static struct procfs_dispatcher_entry
procfs_file_table[] = {

    DECL_FILE(
        "/hardware/(lightsensor|motionsensor|mouse)/data",
        1,
        default_file,
        hardware__xsensor
    )

    DECL_FILE(
        "/hardware/cpus/(\\d+)/data",
        1,
        default_file,
        hardware__cpus__cpu__data
    )

#if MACFUSE_PROCFS_ENABLE_TPM
    DECL_FILE(
        "/hardware/tpm/hwmodel",
        0,
        default_file,
        hardware__tpm__hwmodel
    )

    DECL_FILE(
        "/hardware/tpm/hwvendor",
        0,
        default_file,
        hardware__tpm__hwvendor
    )

    DECL_FILE(
        "/hardware/tpm/hwversion",
        0,
        default_file,
        hardware__tpm__hwversion
    )

    DECL_FILE(
        "/hardware/tpm/keyslots/key(\\d+)",
        1,
        hardware__tpm__keyslots__slot,
        hardware__tpm__keyslots__slot
    )

    DECL_FILE(
        "/hardware/tpm/pcrs/pcr(\\d+)",
        1,
        hardware__tpm__pcrs__pcr,
        hardware__tpm__pcrs__pcr
    )
#endif /* MACFUSE_PROCFS_ENABLE_TPM */

    DECL_FILE(
        "/(\\d+)/carbon/(name|psn)",
        2,
        default_file,
        proc__carbon
    )

    DECL_FILE(
        "/(\\d+)/(cmdline|jobc|paddr|pgid|ppid|tdev|tpgid|wchan)",
        2,
        default_file,
        proc__generic
    )

    DECL_FILE(
        "/(\\d+)/task/absolutetime_info/(threads_system|threads_user|total_system|total_user)",
        2,
        default_file,
        proc__task__absolutetime_info
    )

    DECL_FILE(
        "/(\\d+)/task/basic_info/(policy|resident_size|suspend_count|system_time|user_time|virtual_size)",
        2,
        default_file,
        proc__task__basic_info
    )

    DECL_FILE(
        "/(\\d+)/task/events_info/(cow_faults|csw|faults|messages_received|messages_sent|pageins|syscalls_mach|syscalls_unix)",
        2,
        default_file,
        proc__task__events_info
    )

    DECL_FILE(
        "/(\\d+)/task/thread_times_info/(system_time|user_time)",
        2,
        default_file,
        proc__task__thread_times_info
    )

    DECL_FILE(
        "/(\\d+)/task/mach_name",
        1,
        default_file,
        proc__task__mach_name
    )

    DECL_FILE(
        "/(\\d+)/task/ports/([a-f\\d]+)/(msgcount|qlimit|seqno|sorights|task_rights)",
        3,
        default_file,
        proc__task__ports__port
    )

    DECL_FILE(
        "/(\\d+)/task/role",
        1,
        default_file,
        proc__task__role
    )

    DECL_FILE(
        "/(\\d+)/task/threads/([a-f\\d]+)/basic_info/(cpu_usage|flags|policy|run_state|sleep_time|suspend_count|system_time|user_time)",
        3,
        default_file,
        proc__task__threads__thread__basic_info
    )

    DECL_FILE(
        "/(\\d+)/task/threads/([a-f\\d]+)/states/debug/(dr[0-7])",
        3,
        default_file,
        proc__task__threads__thread__states__debug
    )

    DECL_FILE(
        "/(\\d+)/task/threads/([a-f\\d]+)/states/exception/(err|faultvaddr|trapno)",
        3,
        default_file,
        proc__task__threads__thread__states__exception
    )

    DECL_FILE(
        "/(\\d+)/task/threads/([a-f\\d]+)/states/float/(fpu_fcw|fpu_fsw|fpu_ftw|fpu_fop|fpu_ip|fpu_cs|fpu_dp|fpu_ds|fpu_mxcsr|fpu_mxcsrmask)",
        3,
        default_file,
        proc__task__threads__thread__states__float
    )

    DECL_FILE(
        "/(\\d+)/task/threads/([a-f\\d]+)/states/thread/(e[a-d]x|edi|esi|ebp|esp|ss|eflags|eip|[cdefg]s)",
        3,
        default_file,
        proc__task__threads__thread__states__thread
    )

    DECL_FILE(
        "/(\\d+)/task/tokens/(audit|security)",
        2,
        default_file,
        proc__task__tokens
    )

    DECL_FILE(
        "/(\\d+)/task/vmmap",
        1,
        default_file,
        proc__task__vmmap
    )

    DECL_FILE(
        "/(\\d+)/(ucred|pcred)/(groups|rgid|ruid|svgid|svuid|uid)",
        3,
        default_file,
        proc__xcred
    )
};

static struct procfs_dispatcher_entry
procfs_directory_table[] = {
    DECL_DIRECTORY(
        "/",
        0,
        default_directory,
        root,
        { NULL },
        { "hardware", NULL }
    )

    DECL_DIRECTORY_COMPACT(
        "/hardware",
        { NULL },
#if MACFUSE_PROCFS_ENABLE_TPM
        { "cpus", "lightsensor", "motionsensor", "mouse", "tpm", NULL }
#else
        { "cpus", "lightsensor", "motionsensor", "mouse", NULL }
#endif /* MACFUSE_PROCFS_ENABLE_TPM */
    )

    DECL_DIRECTORY(
        "/hardware/cpus",
        0,
        default_directory,
        hardware__cpus,
        { NULL },
        { NULL },
    )

    DECL_DIRECTORY(
        "/hardware/cpus/(\\d+)",
        1,
        default_directory,
        hardware__cpus__cpu,
        { "data", NULL },
        { NULL },
    )

    DECL_DIRECTORY_COMPACT(
        "/hardware/lightsensor",
        { "data", NULL },
        { NULL },
    )

    DECL_DIRECTORY_COMPACT(
        "/hardware/motionsensor",
        { "data", NULL },
        { NULL },
    )

    DECL_DIRECTORY_COMPACT(
        "/hardware/mouse",
        { "data", NULL },
        { NULL },
    )

#if MACFUSE_PROCFS_ENABLE_TPM
    DECL_DIRECTORY_COMPACT(
        "/hardware/tpm",
        { "hwmodel", "hwvendor", "hwversion", NULL },
        { "keyslots", "pcrs" }
    )

    DECL_DIRECTORY(
        "/hardware/tpm/keyslots",
        0,
        default_directory,
        hardware__tpm__keyslots,
        { NULL },
        { NULL },
    )

    DECL_DIRECTORY(
        "/hardware/tpm/pcrs",
        0,
        default_directory,
        hardware__tpm__pcrs,
        { NULL },
        { NULL },
    )
#endif /* MACFUSE_PROCFS_ENABLE_TPM */

    DECL_DIRECTORY_COMPACT(
        "/\\d+",
        { "cmdline", "jobc", "paddr", "pgid", "ppid", "tdev", "tpgid", "wchan", NULL },
        { "carbon", "pcred", "task", "ucred", NULL }
    )

    DECL_DIRECTORY_COMPACT(
        "/\\d+/carbon",
        { "name", "psn", NULL },
        { NULL }
    )

    DECL_DIRECTORY_COMPACT(
        "/\\d+/pcred",
        { "rgid", "ruid", "svgid", "svgid", NULL },
        { NULL }
    )

    DECL_DIRECTORY_COMPACT(
        "/\\d+/task",
        { "mach_name", "role", NULL },
        {
            "absolutetime_info", "basic_info", "events_info", "ports",
            "thread_times_info", "threads", "tokens", "vmmap", NULL
        }
    )

    DECL_DIRECTORY_COMPACT(
        "/\\d+/task/absolutetime_info",
        {
            "threads_system", "threads_user", "total_system",
            "total_user", NULL
        },
        { NULL }
    )

    DECL_DIRECTORY_COMPACT(
        "/\\d+/task/basic_info",
        {
            "policy", "resident_size", "suspend_count", "system_time",
            "user_time", "virtual_size", NULL
        },
        { NULL }
    )

    DECL_DIRECTORY_COMPACT(
        "/\\d+/task/events_info",
        {
            "cow_faults", "csw", "faults", "messages_received",
            "messages_sent", "pageins", "syscalls_mach", "syscalls_unix", NULL
        },
        { NULL }
    )

    DECL_DIRECTORY(
        "/(\\d+)/task/ports",
        1,
        default_directory,
        proc__task__ports,
        { NULL },
        { NULL }
    )

    DECL_DIRECTORY_COMPACT(
        "/\\d+/task/ports/[a-f\\d]+",
        { "msgcount", "qlimit", "seqno", "sorights", "task_rights", NULL },
        { NULL }
    )

    DECL_DIRECTORY_COMPACT(
        "/\\d+/task/thread_times_info",
        { "system_time", "user_time", NULL },
        { NULL }
    )

    DECL_DIRECTORY(
        "/(\\d+)/task/threads",
        1,
        default_directory,
        proc__task__threads,
        { NULL },
        { NULL }
    )

    DECL_DIRECTORY_COMPACT(
        "/\\d+/task/threads/[a-f\\d]+",
        { NULL },
        {
            "basic_info", "states", NULL
        }
    )

    DECL_DIRECTORY_COMPACT(
        "/\\d+/task/threads/[a-f\\d]+/basic_info",
        {
            "cpu_usage", "flags", "policy", "run_state", "sleep_time",
            "suspend_count", "system_time", "user_time", NULL
        },
        { NULL }
    )

    DECL_DIRECTORY_COMPACT(
        "/\\d+/task/threads/[a-f\\d]+/states",
        { "debug", "exception", "float", "thread", NULL },
        { NULL }
    )

    DECL_DIRECTORY_COMPACT(
        "/\\d+/task/threads/[a-f\\d]+/states/debug",
        { "dr0", "dr1", "dr2", "dr3", "dr4", "dr5", "dr6", "dr7", NULL },
        { NULL }
    )

    DECL_DIRECTORY_COMPACT(
        "/\\d+/task/threads/[a-f\\d]+/states/exception",
        { "err", "faultvaddr", "trapno", NULL },
        { NULL }
    )

    DECL_DIRECTORY_COMPACT(
        "/\\d+/task/threads/[a-f\\d]+/states/float",
        {
            "fpu_cs", "fpu_dp", "fpu_ds", "fpu_fcw", "fpu_fop", "fpu_fsw",
            "fpu_ftw", "fpu_ip", "fpu_mxcsr", "fpu_mxcsrmask", NULL
        },
        { NULL }
    )

    DECL_DIRECTORY_COMPACT(
        "/\\d+/task/threads/[a-f\\d]+/states/thread",
        {
            "eax", "ebx", "ecx", "edx", "edi", "esi", "ebp", "esp", "ss",
            "eflags", "eip", "cs", "ds", "es", "fs", "gs", NULL
        },
        { NULL }
    )

    DECL_DIRECTORY_COMPACT(
        "/\\d+/task/tokens",
        { "audit", "security", NULL },
        { NULL }
    )

    DECL_DIRECTORY_COMPACT(
        "/\\d+/ucred",
        { "groups", "uid", NULL },
        { NULL }
    )
};


// BEGIN: GETATTR

//
//  int
//  procfs_getattr_<handler>(procfs_dispatcher_entry_t  e,
//                           const char                *argv[],
//                           struct stat               *stbuf)

                          
GETATTR_HANDLER(default_file)
{                         
    time_t current_time = time(NULL);
    stbuf->st_mode = S_IFREG | 0444;             
    stbuf->st_nlink = 1;  
    stbuf->st_size = 0;
    stbuf->st_atime = stbuf->st_ctime = stbuf->st_mtime = current_time;

    return 0;
}   
    
GETATTR_HANDLER(default_directory)
{
    time_t current_time = time(NULL);

    stbuf->st_mode = S_IFDIR | 0555;
    stbuf->st_nlink = 1;
    stbuf->st_size = 0;
    stbuf->st_atime = stbuf->st_ctime = stbuf->st_mtime = current_time;
    
    return 0;
}

GETATTR_HANDLER(default_link)
{
    stbuf->st_mode = S_IFLNK | 0755;
    stbuf->st_nlink = 1;
    stbuf->st_size = 0;
    
    return 0;
}

#if MACFUSE_PROCFS_ENABLE_TPM
GETATTR_HANDLER(hardware__tpm__keyslots__slot)
{
    uint32_t keys[256];
    unsigned long slotno = strtol(argv[0], NULL, 10);

    uint16_t slots_used = 0;
    uint32_t slots_free = 0;
    uint32_t slots_total = 0;

    if (TPM_GetCapability_Slots(&slots_free)) {
        return -ENOENT;
    }

    if (TPM_GetCapability_Key_Handle(&slots_used, keys)) {
        return -ENOENT;
    }

    slots_total = slots_used + slots_free;

    if (slotno >= slots_total) {
        return -ENOENT;
    }

    time_t current_time = time(NULL);
    stbuf->st_nlink = 1;
    stbuf->st_size = 9;
    stbuf->st_atime = stbuf->st_ctime = stbuf->st_mtime = current_time;

    if (slotno >= slots_used) {
        stbuf->st_mode = S_IFREG | 0000;
    } else {
        stbuf->st_mode = S_IFREG | 0444;
    }

    return 0;
}

GETATTR_HANDLER(hardware__tpm__pcrs__pcr)
{
    time_t current_time = time(NULL);
    stbuf->st_mode = S_IFREG | 0444;
    stbuf->st_nlink = 1;
    stbuf->st_size = 60;
    stbuf->st_atime = stbuf->st_ctime = stbuf->st_mtime = current_time;

    return 0;
}  
#endif /* MACFUSE_PROCFS_ENABLE_TPM */

// END: GETATTR


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <sys/sysctl.h>

int
procinfo(pid_t pid, struct kinfo_proc *kp)
{
    int mib[4];
    size_t bufsize = 0, orig_bufsize = 0;
    struct kinfo_proc *kprocbuf;
    int retry_count = 0;
    int local_error;

    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PID;
    mib[3] = pid;

    kprocbuf = kp;
    orig_bufsize = bufsize = sizeof(struct kinfo_proc);
    for (retry_count = 0; ; retry_count++) {
        local_error = 0;
        bufsize = orig_bufsize;
        if ((local_error = sysctl(mib, 4, kp, &bufsize, NULL, 0)) < 0) {
            if (retry_count < 1000) {
                sleep(1);
                continue;
            }
            return local_error;
        } else if (local_error == 0) {
            break;
        }
        sleep(1);
    }

    return local_error;
}

int
getproccmdline(pid_t pid, char *cmdlinebuf, int len)
{
    int i, mib[4], rlen, tlen, thislen;
    int    argmax, target_argc;
    char *target_argv;
    char  *cp;
    size_t size;

    if (pid == 0) {
        return snprintf(cmdlinebuf, len, "kernel\n");
    }

    mib[0] = CTL_KERN;
    mib[1] = KERN_ARGMAX;

    size = sizeof(argmax);
    if (sysctl(mib, 2, &argmax, &size, NULL, 0) == -1) {
        return -1;
    }

    target_argv = (char *)malloc(argmax);
    if (target_argv == NULL) {
        return -1;
    }

    mib[0] = CTL_KERN;
    mib[1] = KERN_PROCARGS2;
    mib[2] = pid;

    size = (size_t)argmax;
    if (sysctl(mib, 3, target_argv, &size, NULL, 0) == -1) {
        free(target_argv);
        return -1;
    }

    memcpy(&target_argc, target_argv, sizeof(target_argc));
    cp = target_argv + sizeof(target_argc);
    cp += strlen(cp) + 1; // saved exec path
    rlen = len;
    tlen = 0;
    for (i = 1; i < target_argc + 1; i++) {
        while (*cp == '\0')
            cp++;
        thislen = snprintf(cmdlinebuf + tlen, rlen, "%s ", cp);
        tlen += thislen; 
        rlen -= thislen;
        if (rlen <= 0) {
            break;
        }
        cp += strlen(cp) + 1;
    }
    if (rlen > 0) {
        thislen = snprintf(cmdlinebuf + tlen, rlen, "\n");
        tlen += thislen;
        rlen -= thislen;
    }
    return tlen;
}

// BEGIN: READ

//    int
//    procfs_read_<handler>(procfs_dispatcher_entry_t  e,
//                          const char                *argv[],
//                          void                      *buf,
//                          size_t                     size,
//                          off_t                      offset,
//                          struct fuse_file_info     *fi)


READ_HANDLER(einval)
{
    return -EINVAL;
}

READ_HANDLER(eisdir)
{
    return -EISDIR;
}

READ_HANDLER(hardware__cpus__cpu__data)
{
    int len;
    kern_return_t kr;
    char tmpbuf[4096];
    const char *whichfile = argv[0];
    unsigned int whichcpu = atoi(whichfile);

    if (whichcpu >= processor_count) {
        return -ENOENT;
    }

    processor_basic_info_data_t    basic_info;
    processor_cpu_load_info_data_t cpu_load_info;
    natural_t                      info_count;
    host_name_port_t               myhost = mach_host_self();

    info_count = PROCESSOR_BASIC_INFO_COUNT;
    kr = processor_info(processor_list[whichcpu], PROCESSOR_BASIC_INFO,
                        &myhost, (processor_info_t)&basic_info, &info_count);
    if (kr != KERN_SUCCESS) {
        return -EIO;
    }
    info_count = PROCESSOR_CPU_LOAD_INFO_COUNT;
    kr = processor_info(processor_list[whichcpu], PROCESSOR_CPU_LOAD_INFO,
                        &myhost, (processor_info_t)&cpu_load_info, &info_count);
    if (kr != KERN_SUCCESS) {
        return -EIO;
    }

    len = 0;
    unsigned long ticks;

    len += snprintf(tmpbuf + len, 4096 - len, "slot %d%s%s",
                    basic_info.slot_num,
                    (basic_info.is_master) ? " (master)," : ",",
                    (basic_info.running) ? " running\n" : " not running\n");
    len += snprintf(tmpbuf + len, 4096 - len, "type %d, subtype %d\n",
                    basic_info.cpu_type, basic_info.cpu_subtype);

    ticks = cpu_load_info.cpu_ticks[CPU_STATE_USER] +
            cpu_load_info.cpu_ticks[CPU_STATE_SYSTEM] +
            cpu_load_info.cpu_ticks[CPU_STATE_IDLE] +
            cpu_load_info.cpu_ticks[CPU_STATE_NICE];
    len += snprintf(tmpbuf + len, 4096 - len,
                    "%ld ticks (user %ld system %ld idle %ld nice %ld)\n",
                    ticks,
                    cpu_load_info.cpu_ticks[CPU_STATE_USER],
                    cpu_load_info.cpu_ticks[CPU_STATE_SYSTEM],
                    cpu_load_info.cpu_ticks[CPU_STATE_IDLE],
                    cpu_load_info.cpu_ticks[CPU_STATE_NICE]);
    len += snprintf(tmpbuf + len, 4096 - len, "cpu uptime %ldh %ldm %lds\n",
                    (ticks / 100) / 3600, ((ticks / 100) % 3600) / 60,
                    (ticks / 100) % 60);

    if (len < 0) {
        return -EIO;
    }
    
    if (offset < len) {
        if (offset + size > len)
            size = len - offset;
        memcpy(buf, tmpbuf + offset, size);
    } else             
        size = 0;
    
    return size;

}

typedef struct {
    const char  *classname;
    unsigned int index;
    IOItemCount  structureInputSize;
    IOByteCount  structureOutputSize;
} sms_configuration_t;

sms_configuration_t
SMS_CONFIGURATIONS[] = {
    { "SMCMotionSensor",    5, 40, 40 }, // 0
    { "PMUMotionSensor",   21, 60, 60 }, // 1
    { "IOI2CMotionSensor", 21, 60, 60 }, // 2
    { NULL,                -1,  0,  0 },
};

enum { sms_maxConfigurationID = 2 };

static int sms_configurationID = -1;

READ_HANDLER(hardware__xsensor)
{
    int len = -1;
    kern_return_t kr;
    char tmpbuf[4096];
    const char *whichfile = argv[0];

    if (strcmp(whichfile, "lightsensor") == 0) {
        unsigned int  gIndex = 0;
        IOItemCount   scalarInputCount = 0;
        IOItemCount   scalarOutputCount = 2;
        SInt32 left = 0, right = 0;
        if (lightsensor_port == 0) {
            len = snprintf(tmpbuf, 4096, "not available\n");
            goto gotdata;
        }
        kr = IOConnectMethodScalarIScalarO(lightsensor_port, gIndex,
                                           scalarInputCount, scalarOutputCount,
                                           &left, &right);
        if (kr == KERN_SUCCESS) {
            len = snprintf(tmpbuf, 4096, "%ld %ld\n", left, right);
        } else if (kr == kIOReturnBusy) {
            len = snprintf(tmpbuf, 4096, "busy\n");
        } else {
            len = snprintf(tmpbuf, 4096, "error %d\n", kr);
        }
        goto gotdata;
    }

    if (strcmp(whichfile, "motionsensor") == 0) {
        MotionSensorData_t sms_data;
        if (motionsensor_port == 0) {
            len = snprintf(tmpbuf, 4096, "not available\n");
            goto gotdata;
        }
        kr = sms_getOrientation_hardware_apple(&sms_data);
        if (kr != KERN_SUCCESS) {
            len = snprintf(tmpbuf, 4096, "error %d\n", kr);
        } else {
            len = snprintf(tmpbuf, 4096, "%hhd %hhd %hhd\n",
                           sms_data.x, sms_data.y, sms_data.z);
        }
        goto gotdata;
    }

    if (strcmp(whichfile, "mouse") == 0) {
        Point mouselocation;
        GetMouse(&mouselocation);
        len = snprintf(tmpbuf, 4096, "%d %d\n",
                       mouselocation.h, mouselocation.v);
        goto gotdata;
    }

gotdata:

    if (len < 0) {
        return -EIO;
    }
    
    if (offset < len) {
        if (offset + size > len)
            size = len - offset;
        memcpy(buf, tmpbuf + offset, size);
    } else             
        size = 0;
    
    return size;
}

/*
 * To make the tpm stuff work, you need to:
 *
 * 1. Get the Mac OS X TPM device driver from
 *    http://osxbook.com/book/bonus/chapter10/tpm/
 *
 * 2. Get IBM's libtpm user-space library.
 *
 * 3. Define MACFUSE_PROCFS_ENABLE_TPM to 1, compile procfs.cc, and link with
 *    libtpm.
 */

#if MACFUSE_PROCFS_ENABLE_TPM
READ_HANDLER(hardware__tpm__hwmodel)
{
    int len;
    char tmpbuf[4096];

    len = snprintf(tmpbuf, 4096, "%s\n", "SLB 9635 TT 1.2");

    if (len <= 0) {
        return -EIO;
    }

    if (offset < len) {
        if (offset + size > len)
            size = len - offset;
        memcpy(buf, tmpbuf + offset, size);
    } else
        size = 0;

    return size;
}

READ_HANDLER(hardware__tpm__hwvendor)
{
    int len;
    char tmpbuf[4096];

    len = snprintf(tmpbuf, 4096, "%s\n", "Infineon");

    if (len <= 0) {
        return -EIO;
    }

    if (offset < len) {
        if (offset + size > len)
            size = len - offset;
        memcpy(buf, tmpbuf + offset, size);
    } else
        size = 0;

    return size;
}

READ_HANDLER(hardware__tpm__hwversion)
{
    int major, minor, version, rev, len;
    char tmpbuf[4096];

    if (TPM_GetCapability_Version(&major, &minor, &version, &rev)) {
        return -EIO;
    }

    len = snprintf(tmpbuf, 4096, "%d.%d.%d.%d\n", major, minor, version, rev);

    if (len <= 0) {
        return -EIO;
    }

    if (offset < len) {
        if (offset + size > len)
            size = len - offset;
        memcpy(buf, tmpbuf + offset, size);
    } else
        size = 0;

    return size;
}

READ_HANDLER(hardware__tpm__keyslots__slot)
{
    char tmpbuf[32] = { 0 };
    int len;
    uint32_t keys[256];
    unsigned long slotno = strtol(argv[0], NULL, 10);

    uint16_t slots_used = 0;
    uint32_t slots_free = 0;
    uint32_t slots_total = 0;

    if (TPM_GetCapability_Slots(&slots_free)) {
        return -ENOENT;
    }

    if (TPM_GetCapability_Key_Handle(&slots_used, keys)) {
        return -ENOENT;
    }

    slots_total = slots_used + slots_free;

    if (slotno >= slots_used) {
        return -EIO;
    }

    len = snprintf(tmpbuf, 32, "%08x\n", keys[slotno]);
    if (offset < len) {
        if (offset + size > len)
            size = len - offset;
        memcpy(buf, tmpbuf + offset, size);
    } else {
        size = 0;
    }

    return size;
}

READ_HANDLER(hardware__tpm__pcrs__pcr)
{
    uint32_t pcrs, the_pcr;
    unsigned char pcr_data[20];
    char tmpbuf[4096] = { 0 };
    int len, i;

    if (TPM_GetCapability_Pcrs(&pcrs)) {
        return -EIO;
    }

    the_pcr = strtol(argv[0], NULL, 10);
    if ((the_pcr < 0) || (the_pcr >= pcrs)) {
        return -ENOENT;
    }

    if (TPM_PcrRead(the_pcr, pcr_data)) {
        return -EIO;
    }

    len = 0;
    for (i = 0; i < 20; i++) {
        len += snprintf(tmpbuf + len, 4096 - len, "%02x ", pcr_data[i]);
    }
    tmpbuf[len - 1] = '\n';

    if (offset < len) {
        if (offset + size > len)
            size = len - offset;
        memcpy(buf, tmpbuf + offset, size);
    } else {
        size = 0;
    }

    return size;
}
#endif /* MACFUSE_PROCFS_ENABLE_TPM */

READ_HANDLER(proc__carbon)
{
    OSStatus status = procNotFound;
    ProcessSerialNumber psn;
    CFStringRef nameRef;
    char tmpbuf[4096];
    int len = -1;

    pid_t pid = atoi(argv[0]);
    const char *whichfile = argv[1];

    if (pid <= 0) {
        return 0;
    }

    status = GetProcessForPID(pid, &psn);

    if (status != noErr) {
        return 0;
    }

    if (strcmp(whichfile, "psn") == 0) {
        len = snprintf(tmpbuf, 4096, "%lu:%lu\n",
                       psn.highLongOfPSN, psn.lowLongOfPSN);
        goto gotdata;
    }

    if (strcmp(whichfile, "name") == 0) {
        status = CopyProcessName(&psn, &nameRef);
        CFStringGetCString(nameRef, tmpbuf, 4096, kCFStringEncodingASCII);
        len = snprintf(tmpbuf, 4096, "%s\n", tmpbuf);
        CFRelease(nameRef);
        goto gotdata;
    }

    return -ENOENT;

gotdata:

    if (len < 0) {
        return -EIO;
    }   
        
    if (offset < len) {
        if (offset + size > len)
            size = len - offset;
        memcpy(buf, tmpbuf + offset, size);
    } else
        size = 0;

    return size;
}

#define HANDLE_GENERIC_ITEM(item, fmt, datasrc)     \
    if (strcmp(whichfile, item) == 0) {             \
        len = snprintf(tmpbuf, 4096, fmt, datasrc); \
        goto gotdata;                               \
    }   

READ_HANDLER(proc__generic)
{
    pid_t pid = atoi(argv[0]);
    const char *whichfile = argv[1];
    struct kinfo_proc kp;
    int len;
    char tmpbuf[4096];

    len = procinfo(pid, &kp);
    if (len != 0) {
        return -EIO;
    }

    len = -1;

    if (strcmp(whichfile, "cmdline") == 0) {
        len = getproccmdline(pid, tmpbuf, 4096);
        goto gotdata;
    }

    if (strcmp(whichfile, "tdev") == 0) {
        char *dr = devname_r(kp.kp_eproc.e_tdev, S_IFCHR, tmpbuf, 4096);
        if (!dr) {
            len = snprintf(tmpbuf, 4096, "none\n");
        } else {
            len = snprintf(tmpbuf, 4096, "%s\n", dr);
        }
        goto gotdata;
    }

    HANDLE_GENERIC_ITEM("jobc",    "%hd\n", kp.kp_eproc.e_jobc);
    HANDLE_GENERIC_ITEM("paddr",   "%p\n",  kp.kp_eproc.e_paddr);
    HANDLE_GENERIC_ITEM("pgid",    "%d\n",  kp.kp_eproc.e_pgid);
    HANDLE_GENERIC_ITEM("ppid",    "%d\n",  kp.kp_eproc.e_ppid);
    HANDLE_GENERIC_ITEM("wchan",   "%s\n",
                        (kp.kp_eproc.e_wmesg[0]) ? kp.kp_eproc.e_wmesg : "-");
    HANDLE_GENERIC_ITEM("tpgid",   "%d\n",  kp.kp_eproc.e_tpgid);

gotdata:

    if (len < 0) {
        return -EIO;
    }

    if (offset < len) {
        if (offset + size > len)
            size = len - offset;
        memcpy(buf, tmpbuf + offset, size);
    } else
        size = 0;

    return size;
}

#define READ_PROC_TASK_PROLOGUE()                          \
    int len = -1;                                          \
    kern_return_t kr;                                      \
    char tmpbuf[4096];                                     \
    task_t the_task = MACH_PORT_NULL;                      \
    pid_t pid = strtol(argv[0], NULL, 10);                 \
                                                           \
    kr = task_for_pid(mach_task_self(), pid, &the_task);   \
    if (kr != KERN_SUCCESS) {                              \
        return -EIO;                                       \
    }

#define READ_PROC_TASK_EPILOGUE()                          \
                                                           \
    if (the_task != MACH_PORT_NULL) {                      \
        mach_port_deallocate(mach_task_self(), the_task);  \
    }                                                      \
                                                           \
    if (len < 0) {                                         \
        return -EIO;                                       \
    }                                                      \
                                                           \
    if (offset < len) {                                    \
        if (offset + size > len)                           \
            size = len - offset;                           \
        memcpy(buf, tmpbuf + offset, size);                \
    } else                                                 \
        size = 0;                                          \
                                                           \
    return size;

static const char *thread_policies[] = {
    "UNKNOWN?",
    "STANDARD|EXTENDED",
    "TIME_CONSTRAINT",
    "PRECEDENCE",
};
#define THREAD_POLICIES_MAX (int)(sizeof(thread_policies)/sizeof(char *))

READ_HANDLER(proc__task__absolutetime_info)
{
    READ_PROC_TASK_PROLOGUE();

    task_info_data_t tinfo;
    mach_msg_type_number_t task_info_count;
    task_absolutetime_info_t absolutetime_info;

    task_info_count = TASK_INFO_MAX;
    kr = task_info(the_task, TASK_ABSOLUTETIME_INFO, (task_info_t)tinfo,
                   &task_info_count);
    if (kr != KERN_SUCCESS) {
        return -EIO;
    }

    const char *whichfile = argv[1];
    absolutetime_info = (task_absolutetime_info_t)tinfo;

    if (strcmp(whichfile, "threads_system") == 0) {
        len  = snprintf(tmpbuf, 4096, "%lld\n",
                        absolutetime_info->threads_system);
        goto gotdata;
    }

    if (strcmp(whichfile, "threads_user") == 0) {
        len  = snprintf(tmpbuf, 4096, "%lld\n",
                        absolutetime_info->threads_user);
        goto gotdata;
    }

    if (strcmp(whichfile, "total_system") == 0) {
        len  = snprintf(tmpbuf, 4096, "%lld\n",
                        absolutetime_info->total_system);
        goto gotdata;
    }

    if (strcmp(whichfile, "total_user") == 0) {
        len  = snprintf(tmpbuf, 4096, "%lld\n",
                        absolutetime_info->total_user);
        goto gotdata;
    }

gotdata:

    READ_PROC_TASK_EPILOGUE();
}

READ_HANDLER(proc__task__basic_info)
{
    READ_PROC_TASK_PROLOGUE();

    task_info_data_t tinfo;
    mach_msg_type_number_t task_info_count;
    task_basic_info_t basic_info;

    task_info_count = TASK_INFO_MAX;
    kr = task_info(the_task, TASK_BASIC_INFO, (task_info_t)tinfo,
                   &task_info_count);
    if (kr != KERN_SUCCESS) {
        return -EIO;
    }

    basic_info = (task_basic_info_t)tinfo;
    const char *whichfile = argv[1];

    if (strcmp(whichfile, "policy") == 0) {
        if ((basic_info->policy < 0) &&
            (basic_info->policy > THREAD_POLICIES_MAX)) {
            basic_info->policy = 0;
        }
        len = snprintf(tmpbuf, 4096, "%s\n",
                       thread_policies[basic_info->policy]);
        goto gotdata;
    }

    if (strcmp(whichfile, "resident_size") == 0) {
        len = snprintf(tmpbuf, 4096, "%u KB\n",
                       basic_info->resident_size >> 10);
        goto gotdata;
    }

    if (strcmp(whichfile, "suspend_count") == 0) {
        len = snprintf(tmpbuf, 4096, "%u\n", basic_info->suspend_count);
        goto gotdata;
    }

    if (strcmp(whichfile, "system_time") == 0) {
        len = snprintf(tmpbuf, 4096, "%us %uus\n",
                       basic_info->system_time.seconds,
                       basic_info->system_time.microseconds);
        goto gotdata;
    }

    if (strcmp(whichfile, "user_time") == 0) {
        len = snprintf(tmpbuf, 4096, "%us %uus\n",
                       basic_info->user_time.seconds,
                       basic_info->user_time.microseconds);
        goto gotdata;
    }

    if (strcmp(whichfile, "virtual_size") == 0) {
        len = snprintf(tmpbuf, 4096, "%u KB\n", basic_info->virtual_size >> 10);
        goto gotdata;
    }

gotdata:

    READ_PROC_TASK_EPILOGUE();
}

READ_HANDLER(proc__task__events_info)
{
    READ_PROC_TASK_PROLOGUE();

    task_info_data_t tinfo;
    mach_msg_type_number_t task_info_count;
    task_events_info_t events_info;

    task_info_count = TASK_INFO_MAX;
    kr = task_info(the_task, TASK_EVENTS_INFO, (task_info_t)tinfo,
                   &task_info_count);
    if (kr != KERN_SUCCESS) {
        return -EIO;
    }

    const char *whichfile = argv[1];
    events_info = (task_events_info_t)tinfo;

#define HANDLE_TASK_EVENT_ITEM(item, datasrc)          \
    if (strcmp(whichfile, item) == 0) {                \
        len = snprintf(tmpbuf, 4096, "%u\n", datasrc); \
        goto gotdata;                                  \
    }

    HANDLE_TASK_EVENT_ITEM("cow_faults", events_info->cow_faults);
    HANDLE_TASK_EVENT_ITEM("csw", events_info->csw);
    HANDLE_TASK_EVENT_ITEM("faults", events_info->faults);
    HANDLE_TASK_EVENT_ITEM("messages_received", events_info->messages_received);
    HANDLE_TASK_EVENT_ITEM("messages_sent", events_info->messages_sent);
    HANDLE_TASK_EVENT_ITEM("pageins", events_info->pageins);
    HANDLE_TASK_EVENT_ITEM("syscalls_mach", events_info->syscalls_mach);
    HANDLE_TASK_EVENT_ITEM("syscalls_unix", events_info->syscalls_unix);

    return -EIO;

gotdata:

    READ_PROC_TASK_EPILOGUE();
}

READ_HANDLER(proc__task__thread_times_info)
{
    READ_PROC_TASK_PROLOGUE();

    task_info_data_t tinfo;
    mach_msg_type_number_t task_info_count;
    task_thread_times_info_t thread_times_info;

    task_info_count = TASK_INFO_MAX;
    kr = task_info(the_task, TASK_THREAD_TIMES_INFO, (task_info_t)tinfo,
                   &task_info_count);
    if (kr != KERN_SUCCESS) {
        return -EIO;
    }

    const char *whichfile = argv[1];
    thread_times_info = (task_thread_times_info_t)tinfo;
    
    if (strcmp(whichfile, "system_time") == 0) {
        len = snprintf(tmpbuf, 4096, "%us %uus\n",
                       thread_times_info->system_time.seconds,
                       thread_times_info->system_time.microseconds);
        goto gotdata;
    }

    if (strcmp(whichfile, "user_time") == 0) {
        len = snprintf(tmpbuf, 4096, "%us %uus\n",
                       thread_times_info->user_time.seconds,
                       thread_times_info->user_time.microseconds);
        goto gotdata;
    }

    return -ENOENT;

gotdata:

    READ_PROC_TASK_EPILOGUE();
}

READ_HANDLER(proc__task__mach_name)
{
    READ_PROC_TASK_PROLOGUE();

    len = snprintf(tmpbuf, 4096, "%x\n", the_task);
   
    READ_PROC_TASK_EPILOGUE();
}

READ_HANDLER(proc__task__ports__port)
{
    READ_PROC_TASK_PROLOGUE();
    DECL_PORT_LIST();
    INIT_PORT_LIST(the_task);
    unsigned int i;
    char the_name[MAXNAMLEN + 1];
    mach_port_t the_port = MACH_PORT_NULL;
    mach_port_type_t the_type = 0;
    for (i = 0; i < name_count; i++) {
        snprintf(the_name, MAXNAMLEN, "%x", name_list[i]);
        if (strcmp(the_name, argv[1]) == 0) {
            the_port = name_list[i];
            the_type = type_list[i];
            break;
        }
    }

    if (the_port == MACH_PORT_NULL) {
        FINI_PORT_LIST();
        return -ENOENT;
    }

    const char *whichfile = argv[2];
    mach_msg_type_number_t port_info_count;

    if (strcmp(whichfile, "task_rights") == 0) {
        len = 0;
        if (the_type & MACH_PORT_TYPE_SEND) {
             len += snprintf(tmpbuf + len, 4096 - len, "%s ", "SEND");
        } 
        if (the_type & MACH_PORT_TYPE_RECEIVE) {
             len += snprintf(tmpbuf + len, 4096 - len, "%s ", "RECEIVE");
        } 
        if (the_type & MACH_PORT_TYPE_SEND_ONCE) {
             len += snprintf(tmpbuf + len, 4096 - len, "%s ", "SEND_ONCE");
        } 
        if (the_type & MACH_PORT_TYPE_PORT_SET) {
             len += snprintf(tmpbuf + len, 4096 - len, "%s ", "PORT_SET");
        } 
        if (the_type & MACH_PORT_TYPE_DEAD_NAME) {
             len += snprintf(tmpbuf + len, 4096 - len, "%s ", "DEAD_NAME");
        } 
        if (the_type & MACH_PORT_TYPE_DNREQUEST) {
             len += snprintf(tmpbuf + len, 4096 - len, "%s ", "DNREQUEST");
        } 
        len += snprintf(tmpbuf + len, 4096 - len, "\n");
        goto gotdata;
    }

    mach_port_limits_t port_limits;
    mach_port_status_t port_status;

    port_info_count = MACH_PORT_LIMITS_INFO_COUNT;
    kr = mach_port_get_attributes(the_task, the_port, MACH_PORT_LIMITS_INFO,
                                  (mach_port_info_t)&port_limits,
                                  &port_info_count);

    if ((kr != KERN_SUCCESS) && (kr != KERN_INVALID_RIGHT)) {
        FINI_PORT_LIST();
        return -EIO;
    }

    if (strcmp(whichfile, "qlimit") == 0) {
        if (kr == KERN_SUCCESS) {
            len = snprintf(tmpbuf, 4096, "%d\n", port_limits.mpl_qlimit);
        } else {
            len = snprintf(tmpbuf, 4096, "-\n");
        }
        goto gotdata;
    }

    port_info_count = MACH_PORT_RECEIVE_STATUS_COUNT;
    kr = mach_port_get_attributes(the_task, the_port, MACH_PORT_RECEIVE_STATUS,
                                  (mach_port_info_t)&port_status,
                                  &port_info_count);
    if ((kr != KERN_SUCCESS) && (kr != KERN_INVALID_RIGHT)) {
        FINI_PORT_LIST();
        return -EIO;
    }

    if (strcmp(whichfile, "msgcount") == 0) {
        if (kr == KERN_SUCCESS) {
            len = snprintf(tmpbuf, 4096, "%d\n", port_status.mps_msgcount);
        } else {
            len = snprintf(tmpbuf, 4096, "-\n");
        }
        goto gotdata;
    }

    if (strcmp(whichfile, "seqno") == 0) {
        if (kr == KERN_SUCCESS) {
            len = snprintf(tmpbuf, 4096, "%d\n", port_status.mps_seqno);
        } else {
            len = snprintf(tmpbuf, 4096, "-\n");
        }
        goto gotdata;
    }

    if (strcmp(whichfile, "sorights") == 0) {
        if (kr == KERN_SUCCESS) {
            len = snprintf(tmpbuf, 4096, "%d\n", port_status.mps_sorights);
        } else {
            len = snprintf(tmpbuf, 4096, "-\n");
        }
        goto gotdata;
    }

gotdata:

    FINI_PORT_LIST();

    READ_PROC_TASK_EPILOGUE();
}

static const char *task_roles[] = {
    "RENICED",
    "UNSPECIFIED",
    "FOREGROUND_APPLICATION",
    "BACKGROUND_APPLICATION",
    "CONTROL_APPLICATION",
    "GRAPHICS_SERVER",
};
#define TASK_ROLES_MAX (int)(sizeof(task_roles)/sizeof(char *))

READ_HANDLER(proc__task__role)
{
    READ_PROC_TASK_PROLOGUE();

    task_category_policy_data_t category_policy;
    mach_msg_type_number_t task_info_count;
    boolean_t get_default;

    len = snprintf(tmpbuf, 4096, "NONE\n");

    task_info_count = TASK_CATEGORY_POLICY_COUNT;
    get_default = FALSE;
    kr = task_policy_get(the_task, TASK_CATEGORY_POLICY,
                         (task_policy_t)&category_policy,
                         &task_info_count, &get_default);
    if (kr == KERN_SUCCESS) {
        if (get_default == FALSE) {
            if ((category_policy.role >= -1) &&
                (category_policy.role < (TASK_ROLES_MAX - 1))) {
                len = snprintf(tmpbuf, 4096, "%s\n",
                               task_roles[category_policy.role + 1]);
            }
        }
    }

    READ_PROC_TASK_EPILOGUE();
}

static const char *thread_states[] = {
    "NONE",
    "RUNNING",
    "STOPPED",
    "WAITING",
    "UNINTERRUPTIBLE",
    "HALTED",
};
#define THREAD_STATES_MAX (int)(sizeof(thread_states)/sizeof(char *))

READ_HANDLER(proc__task__threads__thread__basic_info)
{
    READ_PROC_TASK_PROLOGUE();
    DECL_THREAD_LIST();
    INIT_THREAD_LIST(the_task);

    thread_t the_thread = MACH_PORT_NULL;
    unsigned int i = strtoul(argv[1], NULL, 16);

    if (i < thread_count) {
        the_thread = thread_list[i];
    }
    
    if (the_thread == MACH_PORT_NULL) {
        FINI_THREAD_LIST();
        return -ENOENT;
    }   
        
    const char *whichfile = argv[2];

    thread_info_data_t thinfo;
    mach_msg_type_number_t thread_info_count;
    thread_basic_info_t basic_info_th;

    thread_info_count = THREAD_INFO_MAX;
    kr = thread_info(the_thread, THREAD_BASIC_INFO, (thread_info_t)thinfo,
                     &thread_info_count);

    if (kr != KERN_SUCCESS) {
        FINI_THREAD_LIST();
        return -EIO;
    }

    basic_info_th = (thread_basic_info_t)thinfo;

    if (strcmp(whichfile, "cpu_usage") == 0) {
        len = snprintf(tmpbuf, 4096, "%u\n", basic_info_th->cpu_usage);
        goto gotdata;
    }

    if (strcmp(whichfile, "flags") == 0) {
        len = 0;
        len += snprintf(tmpbuf + len, 4096 - len, "%x", basic_info_th->flags);
        len += snprintf(tmpbuf + len, 4096 - len, "%s",
                 (basic_info_th->flags & TH_FLAGS_IDLE) ? " (IDLE)" : "");
        len += snprintf(tmpbuf + len, 4096 - len, "%s",
                 (basic_info_th->flags & TH_FLAGS_SWAPPED) ? " (SWAPPED)" : "");
        len += snprintf(tmpbuf + len, 4096 - len, "\n");
        goto gotdata;
    }

    if (strcmp(whichfile, "policy") == 0) {

        len = 0;
        boolean_t get_default = FALSE;
        thread_extended_policy_data_t        extended_policy;
        thread_time_constraint_policy_data_t time_constraint_policy;
        thread_precedence_policy_data_t      precedence_policy;

        switch (basic_info_th->policy) {

        case THREAD_EXTENDED_POLICY:
            thread_info_count = THREAD_EXTENDED_POLICY_COUNT;
            kr = thread_policy_get(the_thread, THREAD_EXTENDED_POLICY,
                                   (thread_policy_t)&extended_policy,
                                   &thread_info_count, &get_default);
            if (kr != KERN_SUCCESS) {
                len += snprintf(tmpbuf + len, 4096 - len,
                                "STANDARD/EXTENDED\n");
                break;
            }
            len += snprintf(tmpbuf + len, 4096 - len, "%s\n",
                            (extended_policy.timeshare == TRUE) ? \
                            "STANDARD" : "EXTENDED");
            break;

        case THREAD_TIME_CONSTRAINT_POLICY:
            len += snprintf(tmpbuf + len, 4096 - len, "TIME_CONSTRAINT");
            thread_info_count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
            kr = thread_policy_get(the_thread, THREAD_TIME_CONSTRAINT_POLICY,
                                   (thread_policy_t)&time_constraint_policy,
                                   &thread_info_count, &get_default);
            if (kr != KERN_SUCCESS) {
                len += snprintf(tmpbuf + len, 4096 - len, "\n");
                break;
            }
            len += snprintf(tmpbuf + len, 4096 - len,
                            " (period=%u computation=%u constraint=%u "
                            "preemptible=%s)\n",
                            time_constraint_policy.period,
                            time_constraint_policy.computation,
                            time_constraint_policy.constraint,
                            (time_constraint_policy.preemptible == TRUE) ? \
                            "TRUE" : "FALSE");
            break;

        case THREAD_PRECEDENCE_POLICY:
            len += snprintf(tmpbuf + len, 4096 - len, "PRECEDENCE");
            thread_info_count = THREAD_PRECEDENCE_POLICY;
            kr = thread_policy_get(the_thread, THREAD_PRECEDENCE_POLICY,
                                   (thread_policy_t)&precedence_policy,
                                   &thread_info_count, &get_default);
            if (kr != KERN_SUCCESS) {
                len += snprintf(tmpbuf + len, 4096 - len, "\n");
                break;
            }
            len += snprintf(tmpbuf + len, 4096 - len, " (importance=%u)\n",
                            precedence_policy.importance);
            break;

        default:
            len = snprintf(tmpbuf, 4096, "UNKNOWN?\n");
            break;
        }
        goto gotdata;
    }

    if (strcmp(whichfile, "run_state") == 0) {
        len = 0;
        len += snprintf(tmpbuf + len, 4096 - len, "%u",
                        basic_info_th->run_state);
        len += snprintf(tmpbuf + len, 4096 - len, " (%s)\n",
                        (basic_info_th->run_state >= THREAD_STATE_MAX) ? \
                        "?" : thread_states[basic_info_th->run_state]);
        goto gotdata;
    }

    if (strcmp(whichfile, "sleep_time") == 0) {
        len = snprintf(tmpbuf, 4096, "%us\n", basic_info_th->sleep_time);
        goto gotdata;
    }

    if (strcmp(whichfile, "suspend_count") == 0) {
        len = snprintf(tmpbuf, 4096, "%u\n", basic_info_th->suspend_count);
        goto gotdata;
    }

    if (strcmp(whichfile, "system_time") == 0) {
        len = snprintf(tmpbuf, 4096, "%us %uus\n",
                       basic_info_th->system_time.seconds,
                       basic_info_th->system_time.microseconds);
        goto gotdata;
    }

    if (strcmp(whichfile, "user_time") == 0) {
        len = snprintf(tmpbuf, 4096, "%us %uus\n",
                       basic_info_th->user_time.seconds,
                       basic_info_th->user_time.microseconds);
        goto gotdata;
    }

gotdata:

    FINI_THREAD_LIST();

    READ_PROC_TASK_EPILOGUE();
}

READ_HANDLER(proc__task__threads__thread__states__debug)
{
    READ_PROC_TASK_PROLOGUE();
    len = snprintf(tmpbuf, 4096, "not yet implemented\n");
    READ_PROC_TASK_EPILOGUE();
}

READ_HANDLER(proc__task__threads__thread__states__exception)
{
    READ_PROC_TASK_PROLOGUE();
    len = snprintf(tmpbuf, 4096, "not yet implemented\n");
    READ_PROC_TASK_EPILOGUE();
}

READ_HANDLER(proc__task__threads__thread__states__float)
{
    READ_PROC_TASK_PROLOGUE();
    DECL_THREAD_LIST();
    INIT_THREAD_LIST(the_task);

    thread_t the_thread = MACH_PORT_NULL;
    unsigned int i = strtoul(argv[1], NULL, 16);

    if (i < thread_count) {
        the_thread = thread_list[i];
    }

    if (the_thread == MACH_PORT_NULL) {
        FINI_THREAD_LIST();
        return -ENOENT;
    }

#if defined(__i386__)
    const char *whichfile = argv[2];

    i386_float_state_t state = { 0 };
    unsigned int count = i386_FLOAT_STATE_COUNT;
    kr = thread_get_state(the_thread, i386_FLOAT_STATE, (thread_state_t)&state,
                          &count);
    if (kr != KERN_SUCCESS) {
        FINI_THREAD_LIST();
        return -EIO;
    }

#define HANDLE_I386_FLOAT_STATE_ITEM(item, fmt)        \
    if (strcmp(whichfile, #item) == 0) {               \
        len = snprintf(tmpbuf, 4096, fmt, state.item); \
        goto gotdata;                                  \
    }

    HANDLE_I386_FLOAT_STATE_ITEM(fpu_cs, "%hx\n");
    HANDLE_I386_FLOAT_STATE_ITEM(fpu_dp, "%x\n");
    HANDLE_I386_FLOAT_STATE_ITEM(fpu_ds, "%hx\n");
    HANDLE_I386_FLOAT_STATE_ITEM(fpu_fop, "%hx\n");
    HANDLE_I386_FLOAT_STATE_ITEM(fpu_ftw, "%hhx\n");
    HANDLE_I386_FLOAT_STATE_ITEM(fpu_ip, "%x\n");
    HANDLE_I386_FLOAT_STATE_ITEM(fpu_mxcsr, "%x\n");
    HANDLE_I386_FLOAT_STATE_ITEM(fpu_mxcsrmask, "%x\n");

#define HANDLE_I386_FLOAT_STATE_ITEM_CONTROL_BIT(bit)           \
    if (state.fpu_fcw.bit) {                                    \
        len += snprintf(tmpbuf + len, 4096 - len, "%s ", #bit); \
    }

    if (strcmp(whichfile, "fpu_fcw") == 0) { /* control */
        len = 0;
        HANDLE_I386_FLOAT_STATE_ITEM_CONTROL_BIT(invalid);
        HANDLE_I386_FLOAT_STATE_ITEM_CONTROL_BIT(denorm);
        HANDLE_I386_FLOAT_STATE_ITEM_CONTROL_BIT(zdiv);
        HANDLE_I386_FLOAT_STATE_ITEM_CONTROL_BIT(ovrfl);
        HANDLE_I386_FLOAT_STATE_ITEM_CONTROL_BIT(undfl);
        HANDLE_I386_FLOAT_STATE_ITEM_CONTROL_BIT(precis);
        HANDLE_I386_FLOAT_STATE_ITEM_CONTROL_BIT(pc);
        switch (state.fpu_fcw.pc) {    
        case 0:
            len += snprintf(tmpbuf + len, 4096 - len, "(24B) ");
            break;
        case 2:
            len += snprintf(tmpbuf + len, 4096 - len, "(53B) ");
             break;
        case 3:
            len += snprintf(tmpbuf + len, 4096 - len, "(64B) ");
            break;
        }
        HANDLE_I386_FLOAT_STATE_ITEM_CONTROL_BIT(rc);
        switch (state.fpu_fcw.rc) {
        case 0:
            len += snprintf(tmpbuf + len, 4096 - len, "(round near) ");
            break;
        case 1:
            len += snprintf(tmpbuf + len, 4096 - len, "(round down) ");
            break;
        case 2:
            len += snprintf(tmpbuf + len, 4096 - len, "(round up) ");
            break;
        case 3:
            len += snprintf(tmpbuf + len, 4096 - len, "(chop) ");
            break;
        }
        len += snprintf(tmpbuf + len, 4096 - len, "\n");
        goto gotdata;
    }

#define HANDLE_I386_FLOAT_STATE_ITEM_STATUS_BIT(bit)            \
    if (state.fpu_fsw.bit) {                                    \
        len += snprintf(tmpbuf + len, 4096 - len, "%s ", #bit); \
    }

    if (strcmp(whichfile, "fpu_fsw") == 0) { /* status */
        len = 0;
        HANDLE_I386_FLOAT_STATE_ITEM_STATUS_BIT(invalid);
        HANDLE_I386_FLOAT_STATE_ITEM_STATUS_BIT(denorm);
        HANDLE_I386_FLOAT_STATE_ITEM_STATUS_BIT(zdiv);
        HANDLE_I386_FLOAT_STATE_ITEM_STATUS_BIT(ovrfl);
        HANDLE_I386_FLOAT_STATE_ITEM_STATUS_BIT(undfl);
        HANDLE_I386_FLOAT_STATE_ITEM_STATUS_BIT(precis);
        HANDLE_I386_FLOAT_STATE_ITEM_STATUS_BIT(stkflt);
        HANDLE_I386_FLOAT_STATE_ITEM_STATUS_BIT(errsumm);
        HANDLE_I386_FLOAT_STATE_ITEM_STATUS_BIT(c0);
        HANDLE_I386_FLOAT_STATE_ITEM_STATUS_BIT(c1);
        HANDLE_I386_FLOAT_STATE_ITEM_STATUS_BIT(c2);
        HANDLE_I386_FLOAT_STATE_ITEM_STATUS_BIT(c3);
        HANDLE_I386_FLOAT_STATE_ITEM_STATUS_BIT(busy);
        len += snprintf(tmpbuf + len, 4096 - len, "tos=%hhx\n",
                        state.fpu_fsw.tos);
        goto gotdata;
    }

#else
    len = -1;
    goto gotdata;
#endif

gotdata:

    FINI_THREAD_LIST();

    READ_PROC_TASK_EPILOGUE();
}

READ_HANDLER(proc__task__threads__thread__states__thread)
{
    READ_PROC_TASK_PROLOGUE();
    DECL_THREAD_LIST();
    INIT_THREAD_LIST(the_task);

    thread_t the_thread = MACH_PORT_NULL;
    unsigned int i = strtoul(argv[1], NULL, 16);

    if (i < thread_count) {
        the_thread = thread_list[i];
    }

    if (the_thread == MACH_PORT_NULL) {
        FINI_THREAD_LIST();
        return -ENOENT;
    }

#if defined(__i386__)

    const char *whichfile = argv[2];

    i386_thread_state_t state = { 0 };
    unsigned int count = i386_THREAD_STATE_COUNT;
    kr = thread_get_state(the_thread, i386_THREAD_STATE, (thread_state_t)&state,
                          &count);
    if (kr != KERN_SUCCESS) {
        FINI_THREAD_LIST();
        return -EIO;
    }

#define HANDLE_I386_THREAD_STATE_ITEM(item)               \
    if (strcmp(whichfile, #item) == 0) {                  \
        len = snprintf(tmpbuf, 4096, "%x\n", state.item); \
        goto gotdata;                                     \
    }

    HANDLE_I386_THREAD_STATE_ITEM(eax);
    HANDLE_I386_THREAD_STATE_ITEM(ebx);
    HANDLE_I386_THREAD_STATE_ITEM(ecx);
    HANDLE_I386_THREAD_STATE_ITEM(edx);
    HANDLE_I386_THREAD_STATE_ITEM(edi);
    HANDLE_I386_THREAD_STATE_ITEM(esi);
    HANDLE_I386_THREAD_STATE_ITEM(ebp);
    HANDLE_I386_THREAD_STATE_ITEM(esp);
    HANDLE_I386_THREAD_STATE_ITEM(ss);
    HANDLE_I386_THREAD_STATE_ITEM(eflags);
    HANDLE_I386_THREAD_STATE_ITEM(eip);
    HANDLE_I386_THREAD_STATE_ITEM(cs);
    HANDLE_I386_THREAD_STATE_ITEM(ds);
    HANDLE_I386_THREAD_STATE_ITEM(es);
    HANDLE_I386_THREAD_STATE_ITEM(fs);
    HANDLE_I386_THREAD_STATE_ITEM(gs);

#else
    len = -1;
    goto gotdata;
#endif

gotdata:

    FINI_THREAD_LIST();

    READ_PROC_TASK_EPILOGUE();
}

READ_HANDLER(proc__task__tokens)
{
    READ_PROC_TASK_PROLOGUE();

    unsigned int n;
    audit_token_t audit_token;
    security_token_t security_token;
    mach_msg_type_number_t task_info_count;
    const char *whichfile = argv[1];

    if (strcmp(whichfile, "audit") == 0) {
        task_info_count = TASK_AUDIT_TOKEN_COUNT;
        kr = task_info(the_task, TASK_AUDIT_TOKEN,
                       (task_info_t)&audit_token, &task_info_count);
        len = -1;
        if (kr == KERN_SUCCESS) {
            len = 0;
            for (n = 0; n < sizeof(audit_token)/sizeof(uint32_t); n++) {
                len += snprintf(tmpbuf + len, 4096 - len, "%x ",
                                audit_token.val[n]);
            }
            len += snprintf(tmpbuf + len, 4096 - len, "\n");
        }
        goto gotdata;
    }

    if (strcmp(whichfile, "security") == 0) {
        task_info_count = TASK_SECURITY_TOKEN_COUNT;
        kr = task_info(the_task, TASK_SECURITY_TOKEN,
                       (task_info_t)&security_token, &task_info_count);
        len = -1;
        if (kr == KERN_SUCCESS) {
            len = 0;
            for (n = 0; n < sizeof(security_token)/sizeof(uint32_t); n++) {
                len += snprintf(tmpbuf + len, 4096 - len, "%x ",
                                security_token.val[n]);
            }
            len += snprintf(tmpbuf + len, 4096 - len, "\n");
        }
        goto gotdata;
    }

    return -ENOENT;

gotdata:

    READ_PROC_TASK_EPILOGUE();
}

const char *
inheritance_strings[] = {
    "SHARE", "COPY", "NONE", "DONATE_COPY",
};

const char *
behavior_strings[] = {
    "DEFAULT", "RANDOM", "SEQUENTIAL", "RESQNTL", "WILLNEED", "DONTNEED",
};

READ_HANDLER(proc__task__vmmap)
{
    int len = -1;
    kern_return_t kr;
#define MAX_VMDATA_SIZE 65536 /* XXX */
    char tmpbuf[MAX_VMDATA_SIZE];
    task_t the_task;
    pid_t pid = strtol(argv[0], NULL, 10);

    kr = task_for_pid(mach_task_self(), pid, &the_task);
    if (kr != KERN_SUCCESS) {
        return -EIO;
    }

    vm_size_t vmsize;
    vm_address_t address;
    vm_region_basic_info_data_t info;
    mach_msg_type_number_t info_count;
    vm_region_flavor_t flavor; 
    memory_object_name_t object;

    kr = KERN_SUCCESS;
    address = 0;
    len = 0;

    do {
        flavor = VM_REGION_BASIC_INFO;
        info_count = VM_REGION_BASIC_INFO_COUNT;
        kr = vm_region(the_task, &address, &vmsize, flavor,
                       (vm_region_info_t)&info, &info_count, &object);
        if (kr == KERN_SUCCESS) {
            if (len >= MAX_VMDATA_SIZE) {
                goto gotdata;
            }
            len += snprintf(tmpbuf + len, MAX_VMDATA_SIZE - len,
            "%08x-%08x %8uK %c%c%c/%c%c%c %11s %6s %10s uwir=%hu sub=%u\n",
                            address, (address + vmsize), (vmsize >> 10),
                            (info.protection & VM_PROT_READ)        ? 'r' : '-',
                            (info.protection & VM_PROT_WRITE)       ? 'w' : '-',
                            (info.protection & VM_PROT_EXECUTE)     ? 'x' : '-',
                            (info.max_protection & VM_PROT_READ)    ? 'r' : '-',
                            (info.max_protection & VM_PROT_WRITE)   ? 'w' : '-',
                            (info.max_protection & VM_PROT_EXECUTE) ? 'x' : '-',
                            inheritance_strings[info.inheritance],
                            (info.shared) ? "shared" : "-",
                            behavior_strings[info.behavior],
                            info.user_wired_count,
                            info.reserved);
            address += vmsize;
        } else if (kr != KERN_INVALID_ADDRESS) {

            if (the_task != MACH_PORT_NULL) {
                mach_port_deallocate(mach_task_self(), the_task);
            }

            return -EIO;
        }
    } while (kr != KERN_INVALID_ADDRESS);

gotdata:

    if (the_task != MACH_PORT_NULL) {
        mach_port_deallocate(mach_task_self(), the_task);
    }

    READ_PROC_TASK_EPILOGUE();
}

READ_HANDLER(proc__xcred)
{
    pid_t pid = atoi(argv[0]);
    const char *whichfile = argv[2];
    struct kinfo_proc kp;
    int len;
    char tmpbuf[4096];
    struct passwd *p;
    struct group *g;

    len = procinfo(pid, &kp);
    if (len != 0) {
        return -EIO;
    }

    len = -1;

    if (strcmp(whichfile, "groups") == 0) {
        short n;
        len = 0;
        for (n = 0; n < kp.kp_eproc.e_ucred.cr_ngroups; n++) {
            g = getgrgid(kp.kp_eproc.e_ucred.cr_groups[n]);
            len += snprintf(tmpbuf + len, 4096 - len, "%d(%s) ",
                            kp.kp_eproc.e_ucred.cr_groups[n],
                            (g) ? g->gr_name : "?");
        }
        len += snprintf(tmpbuf + len, 4096 - len, "\n");
        goto gotdata;
    }   

    if (strcmp(whichfile, "rgid") == 0) {
        g = getgrgid(kp.kp_eproc.e_pcred.p_rgid);
        len = snprintf(tmpbuf, 4096, "%d(%s)\n", kp.kp_eproc.e_pcred.p_rgid,
                       (g) ? g->gr_name : "?");
        goto gotdata;
    }

    if (strcmp(whichfile, "svgid") == 0) {
        g = getgrgid(kp.kp_eproc.e_pcred.p_svgid);
        len = snprintf(tmpbuf, 4096, "%d(%s)\n", kp.kp_eproc.e_pcred.p_svgid,
                       (g) ? g->gr_name : "?");
        goto gotdata;
    }

    if (strcmp(whichfile, "ruid") == 0) {
        p = getpwuid(kp.kp_eproc.e_pcred.p_ruid);
        len = snprintf(tmpbuf, 4096, "%d(%s)\n", kp.kp_eproc.e_pcred.p_ruid,
                       (p) ? p->pw_name : "?");
        goto gotdata;
    }

    if (strcmp(whichfile, "svuid") == 0) {
        p = getpwuid(kp.kp_eproc.e_pcred.p_svuid);
        len = snprintf(tmpbuf, 4096, "%d(%s)\n", kp.kp_eproc.e_pcred.p_svuid,
                       (p) ? p->pw_name : "?");
        goto gotdata;
    }

    if (strcmp(whichfile, "uid") == 0) {
        p = getpwuid(kp.kp_eproc.e_ucred.cr_uid);
        len = snprintf(tmpbuf, 4096, "%d(%s)\n", kp.kp_eproc.e_ucred.cr_uid,
                       (p) ? p->pw_name : "?");
        goto gotdata;
    }

gotdata:
    
    if (len < 0) {
        return -EIO;
    }

    if (offset < len) {
        if (offset + size > len)
            size = len - offset;
        memcpy(buf, tmpbuf + offset, size);
    } else
        size = 0;

    return size;
}

// END: READ


// BEGIN: READDIR

//    int
//    procfs_readdir_<handler>(procfs_dispatcher_entry_t *e,
//                             const char                *argv[],
//                             void                      *buf,
//                             fuse_fill_dir_t            filler,
//                             off_t                      offset,
//                             struct fuse_file_info     *fi)


int
procfs_populate_directory(const char           **content_files,
                          const char           **content_directories,
                          void                  *buf,
                          fuse_fill_dir_t        filler,
                          off_t                  offset,
                          struct fuse_file_info *fi)
{
    int    bufferfull = 0;
    struct stat dir_stat;
    struct stat file_stat;
    const char **name;

    memset(&dir_stat, 0, sizeof(dir_stat));
    dir_stat.st_mode = S_IFDIR | 755;
    dir_stat.st_size = 0;

    memset(&file_stat, 0, sizeof(file_stat));
    dir_stat.st_mode = S_IFREG | 644;
    dir_stat.st_size = 0;

    if (filler(buf, ".", NULL, 0)) {
        bufferfull = 1;
        goto out;
    }

    if (filler(buf, "..", NULL, 0)) {
        bufferfull = 1;
        goto out;
    }

    if (!content_files && !content_directories) {
        goto out;
    }

    name = content_directories;
    if (name) {
        for (; *name; name++) {
            if (filler(buf, *name, &dir_stat, 0)) {
                bufferfull = 1;
                goto out;
            }
        }
    }

    name = content_files;
    if (name) {
        for (; *name; name++) {
            if (filler(buf, *name, &file_stat, 0)) {
                bufferfull = 1;
                goto out;
            }
        }
    }

out:
    return bufferfull;
}


READDIR_HANDLER(enotdir)
{
    return -ENOTDIR;
}

READDIR_HANDLER(default)
{
    return 0;
}

READDIR_HANDLER(root)
{
    unsigned int i;
    kern_return_t kr;
    char the_name[MAXNAMLEN + 1];  
    struct stat dir_stat;
    pid_t pid;
    DECL_TASK_LIST();

    INIT_TASK_LIST();
    for (i = 0; i < task_count; i++) {
        memset(&dir_stat, 0, sizeof(dir_stat));
        dir_stat.st_mode = S_IFDIR | 0755;
        dir_stat.st_size = 0;
        kr = pid_for_task(task_list[i], &pid);
        if (kr != KERN_SUCCESS) {
            continue;
        }
        snprintf(the_name, MAXNAMLEN, "%d", pid);
        if (filler(buf, the_name, &dir_stat, 0)) {
            break;
        }
    }
    FINI_TASK_LIST();

    return 0;
}

READDIR_HANDLER(hardware__cpus)
{
    int len;
    unsigned int i;
    char the_name[MAXNAMLEN + 1];  
    struct stat the_stat;

    memset(&the_stat, 0, sizeof(the_stat));

    for (i = 0; i < processor_count; i++) {
        the_stat.st_mode = S_IFDIR | 0555;
        the_stat.st_nlink = 1;
        len = snprintf(the_name, MAXNAMLEN, "%d", i);
        if (filler(buf, the_name, &the_stat, 0)) {
            break;
        }
    }

    return 0;
}

READDIR_HANDLER(hardware__cpus__cpu)
{
    return 0;
}

READDIR_HANDLER(hardware__tpm__keyslots)
{
#if MACFUSE_PROCFS_ENABLE_TPM
    unsigned int i, len;
    char the_name[MAXNAMLEN + 1];  
    struct stat the_stat;
    uint32_t keys[256];

    uint16_t slots_used = 0;
    uint32_t slots_free = 0;
    uint32_t slots_total = 0;

    if (TPM_GetCapability_Slots(&slots_free)) {
        return -ENOENT;
    }

    if (TPM_GetCapability_Key_Handle(&slots_used, keys)) {
        return -ENOENT;
    }

    slots_total = slots_used + slots_free;

    memset(&the_stat, 0, sizeof(the_stat));

    for (i = 0; i < slots_total; i++) {
        len = snprintf(the_name, MAXNAMLEN, "key%02d", i);
        if (i >= slots_used) {
            the_stat.st_size = 0;
            the_stat.st_mode = S_IFREG | 0000;
        } else {
            the_stat.st_size = 4096;
            the_stat.st_mode = S_IFREG | 0444;
        }
        if (filler(buf, the_name, &the_stat, 0)) {
            break;
        }
    }
#endif

    return 0;
}

READDIR_HANDLER(hardware__tpm__pcrs)
{
#if MACFUSE_PROCFS_ENABLE_TPM
    unsigned int i, len;
    uint32_t pcrs;
    char the_name[MAXNAMLEN + 1];  
    struct stat the_stat;

    if (TPM_GetCapability_Pcrs(&pcrs)) {
        return -ENOENT;
    }

    memset(&the_stat, 0, sizeof(the_stat));

    for (i = 0; i < pcrs; i++) {
        len = snprintf(the_name, MAXNAMLEN, "pcr%02d", i);
        the_stat.st_size = 4096;
        the_stat.st_mode = S_IFREG | 0444;
        if (filler(buf, the_name, &the_stat, 0)) {
            break;
        }
    }
#endif

    return 0;
}

READDIR_HANDLER(proc__task__ports)
{
    unsigned int i;
    kern_return_t kr;
    DECL_PORT_LIST();
    pid_t pid = strtol(argv[0], NULL, 10);
    struct stat dir_stat;
    char the_name[MAXNAMLEN + 1];
    task_t the_task = MACH_PORT_NULL;

    kr = task_for_pid(mach_task_self(), pid, &the_task);
    if (kr != KERN_SUCCESS) {
        return -ENOENT;
    }

    memset(&dir_stat, 0, sizeof(dir_stat));
    dir_stat.st_mode = S_IFDIR | 755;
    dir_stat.st_size = 0;

    INIT_PORT_LIST(the_task);
    for (i = 0; i < name_count; i++) {
        snprintf(the_name, MAXNAMLEN, "%x", name_list[i]);
        if (filler(buf, the_name, &dir_stat, 0)) {
            break;
        }
    }
    FINI_PORT_LIST();

    if (the_task != MACH_PORT_NULL) {
        mach_port_deallocate(mach_task_self(), the_task);
    }

    return 0;
}

READDIR_HANDLER(proc__task__threads)
{
    unsigned int i;
    kern_return_t kr;
    DECL_THREAD_LIST();
    pid_t pid = strtol(argv[0], NULL, 10);
    struct stat dir_stat;
    char the_name[MAXNAMLEN + 1];
    task_t the_task = MACH_PORT_NULL;

    kr = task_for_pid(mach_task_self(), pid, &the_task);
    if (kr != KERN_SUCCESS) {
        return -ENOENT;
    }

    memset(&dir_stat, 0, sizeof(dir_stat));
    dir_stat.st_mode = S_IFDIR | 755;
    dir_stat.st_size = 0;

    INIT_THREAD_LIST(the_task);
    FINI_THREAD_LIST();

    for (i = 0; i < thread_count; i++) {
        snprintf(the_name, MAXNAMLEN, "%x", i);
        if (filler(buf, the_name, &dir_stat, 0)) {
            break;
        }
    }

    if (the_task != MACH_PORT_NULL) {
        mach_port_deallocate(mach_task_self(), the_task);
    }

    return 0;
}

// END: READDIR


// BEGIN: READLINK

//    int
//    procfs_readlink_<handler>(procfs_dispatcher_entry_t  e,
//                              const char                *argv[],
//                              char                      *buf,
//                              size_t                     size)

READLINK_HANDLER(einval)
{
    return -EINVAL;
}

// END: READLINK


#define DEBUG 1
#ifdef DEBUG
#define TRACEME() { fprintf(stderr, "%s: path=%s\n", __FUNCTION__, path); }
#else
#define TRACEME() { }
#endif

#define EXIT_ON_MACH_ERROR(msg, retval) \
    if (kr != KERN_SUCCESS) { mach_error(msg ":" , kr); exit((retval)); }

static void *procfs_init(struct fuse_conn_info *conn)
{
    int i;
    kern_return_t kr;

    kr = processor_set_default(mach_host_self(), &p_default_set);
    EXIT_ON_MACH_ERROR("processor_default", 1);
   
    kr = host_processor_set_priv(mach_host_self(), p_default_set,
                                 &p_default_set_control);
    EXIT_ON_MACH_ERROR("host_processor_set_priv", 1);

    kr = host_get_host_priv_port(mach_host_self(), &host_priv);
    EXIT_ON_MACH_ERROR("host_get_host_priv_port", 1);
   
    processor_list = (processor_port_array_t)0;
    kr = host_processors(host_priv, &processor_list, &processor_count);
    EXIT_ON_MACH_ERROR("host_processors", 1);

    io_service_t serviceObject;

    serviceObject = IOServiceGetMatchingService(kIOMasterPortDefault,
                        IOServiceMatching("AppleLMUController"));
    if (serviceObject) {
        kr = IOServiceOpen(serviceObject, mach_task_self(), 0,
                           &lightsensor_port);
        IOObjectRelease(serviceObject);
        if (kr != KERN_SUCCESS) {
            lightsensor_port = 0;
        }
    }

    kr = KERN_FAILURE;
    CFDictionaryRef classToMatch;
    MotionSensorData_t sms_data;

    for (i = 0; i <= sms_maxConfigurationID; i++) {

        sms_gIndex = SMS_CONFIGURATIONS[i].index;
        classToMatch = IOServiceMatching(SMS_CONFIGURATIONS[i].classname);
        sms_gStructureInputSize = SMS_CONFIGURATIONS[i].structureInputSize;
        sms_gStructureOutputSize = SMS_CONFIGURATIONS[i].structureOutputSize;
        sms_configurationID = i;

        serviceObject = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                    classToMatch);
        if (!serviceObject) {
            continue;
        }

        kr = IOServiceOpen(serviceObject, mach_task_self(), 0,
                           &motionsensor_port);
        IOObjectRelease(serviceObject);
        if (kr != KERN_SUCCESS) {
            continue;
        }

        kr = sms_getOrientation_hardware_apple(&sms_data);
        if (kr != KERN_SUCCESS) {
            IOServiceClose(motionsensor_port);
            motionsensor_port = 0;
            continue;
        } else {
            break;
        }
    }

    total_file_patterns =
        sizeof(procfs_file_table)/sizeof(struct procfs_dispatcher_entry);
    total_directory_patterns =
        sizeof(procfs_directory_table)/sizeof(struct procfs_dispatcher_entry);
    total_link_patterns =
        sizeof(procfs_link_table)/sizeof(struct procfs_dispatcher_entry);
   
    return NULL;
}

static void procfs_destroy(void *arg)
{
    (void)mach_port_deallocate(mach_task_self(), p_default_set);
    (void)mach_port_deallocate(mach_task_self(), p_default_set_control);
}

static int procfs_getattr(const char *path, struct stat *stbuf)
{
    int i;
    procfs_dispatcher_entry_t e;
    string arg1, arg2, arg3;
    const char *real_argv[PROCFS_MAX_ARGS];

    if (valid_process_pattern->PartialMatch(path, &arg1)) {
        pid_t check_pid = atoi(arg1.c_str());
        if (getpgid(check_pid) == -1) {
            return -ENOENT;
        }
    }

    for (i = 0; i < PROCFS_MAX_ARGS; i++) {
        real_argv[i] = (char *)0;
    }

    for (i = 0; i < total_directory_patterns; i++) {
        e = &procfs_directory_table[i];
        switch (e->argc) {
        case 0:
            if (e->compiled_pattern->FullMatch(path)) {
                goto out;
            }
            break;

        case 1:
            if (e->compiled_pattern->FullMatch(path, &arg1)) {
                real_argv[0] = arg1.c_str();
                goto out;
            }
            break;

        case 2:
            if (e->compiled_pattern->FullMatch(path, &arg1, &arg2)) {
                real_argv[0] = arg1.c_str();
                real_argv[1] = arg2.c_str();
                goto out;
            }
            break;

        case 3:
            if (e->compiled_pattern->FullMatch(path, &arg1, &arg2, &arg3)) {
                real_argv[0] = arg1.c_str();
                real_argv[1] = arg2.c_str();
                real_argv[2] = arg3.c_str();
                goto out;
            }
            break;

        default:
            return -ENOENT;
        }
    }

    for (i = 0; i < total_file_patterns; i++) {
        e = &procfs_file_table[i];
        switch (e->argc) {
        case 0:
            if (e->compiled_pattern->FullMatch(path)) {
                goto out;
            }
            break;

        case 1:
            if (e->compiled_pattern->FullMatch(path, &arg1)) {
                real_argv[0] = arg1.c_str();
                goto out;
            }
            break;

        case 2:
            if (e->compiled_pattern->FullMatch(path, &arg1, &arg2)) {
                real_argv[0] = arg1.c_str();
                real_argv[1] = arg2.c_str();
                goto out;
            }
            break;

        case 3:
            if (e->compiled_pattern->FullMatch(path, &arg1, &arg2, &arg3)) {
                real_argv[0] = arg1.c_str();
                real_argv[1] = arg2.c_str();
                real_argv[2] = arg3.c_str();
                goto out;
            }
            break;

        default:
            return -ENOENT;
        }
    }

    for (i = 0; i < total_link_patterns; i++) {
        e = &procfs_link_table[i];
        switch (e->argc) {
        case 0:
            if (e->compiled_pattern->FullMatch(path)) {
                goto out;
            }
            break;

        case 1:
            if (e->compiled_pattern->FullMatch(path, &arg1)) {
                real_argv[0] = arg1.c_str();
                goto out;
            }
            break;

        case 2:
            if (e->compiled_pattern->FullMatch(path, &arg1, &arg2)) {
                real_argv[0] = arg1.c_str();
                real_argv[1] = arg2.c_str();
                goto out;
            }
            break;

        case 3:
            if (e->compiled_pattern->FullMatch(path, &arg1, &arg2, &arg3)) {
                real_argv[0] = arg1.c_str();
                real_argv[1] = arg2.c_str();
                real_argv[2] = arg3.c_str();
                goto out;
            }
            break;

        default:
            return -ENOENT;
        }
    }

    return -ENOENT;

out:
    return e->getattr(e, real_argv, stbuf);
}


static int procfs_readdir(const char             *path,
                          void                   *buf,
                          fuse_fill_dir_t        filler,
                          off_t                  offset,
                          struct fuse_file_info *fi)
{
    int i;
    procfs_dispatcher_entry_t e;
    string arg1, arg2, arg3;
    const char *real_argv[PROCFS_MAX_ARGS];

    if (valid_process_pattern->PartialMatch(path, &arg1)) {
        pid_t check_pid = atoi(arg1.c_str());
        if (getpgid(check_pid) == -1) {
            return -ENOENT;
        }
    }

    for (i = 0; i < PROCFS_MAX_ARGS; i++) {
        real_argv[i] = (char *)0;
    }

    for (i = 0; i < total_directory_patterns; i++) {

        e = &procfs_directory_table[i];

        switch (e->argc) {
        case 0:
            if (e->compiled_pattern->FullMatch(path)) {
                goto out;
            }
            break;

        case 1:
            if (e->compiled_pattern->FullMatch(path, &arg1)) {
                real_argv[0] = arg1.c_str();
                goto out;
            }
            break;

        case 2:
            if (e->compiled_pattern->FullMatch(path, &arg1, &arg2)) {
                real_argv[0] = arg1.c_str();
                real_argv[1] = arg2.c_str();
                goto out;
            }
            break;

        case 3:
            if (e->compiled_pattern->FullMatch(path, &arg1, &arg2, &arg3)) {
                real_argv[0] = arg1.c_str();
                real_argv[1] = arg2.c_str();
                real_argv[2] = arg3.c_str();
                goto out;
            }
            break;

        default:
            return -ENOENT;
        }
    }

    return -ENOENT;

out:
    (void)e->readdir(e, real_argv, buf, filler, offset, fi);

    (void)procfs_populate_directory(e->content_files, e->content_directories,
                                    buf, filler, offset, fi);

    return 0;
}

static int procfs_readlink(const char *path, char *buf, size_t size)
{
    int i;
    procfs_dispatcher_entry_t e;
    string arg1, arg2, arg3;
    const char *real_argv[PROCFS_MAX_ARGS];

    for (i = 0; i < PROCFS_MAX_ARGS; i++) {
        real_argv[i] = (char *)0;
    }

    for (i = 0; i < total_link_patterns; i++) {
        e = &procfs_link_table[i];

        switch (e->argc) {
        case 0:
            if (e->compiled_pattern->FullMatch(path)) {
                goto out;
            }
            break;
        case 1:
            if (e->compiled_pattern->FullMatch(path, &arg1)) {
                real_argv[0] = arg1.c_str();
                goto out;
            }
            break;
        case 2:
            if (e->compiled_pattern->FullMatch(path, &arg1, &arg2)) {
                real_argv[0] = arg1.c_str();
                real_argv[1] = arg2.c_str();
                goto out;
            }
            break;
        }
    }

    return -ENOENT;

out:
    return e->readlink(e, real_argv, buf, size);
}

static int procfs_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
    int i;
    procfs_dispatcher_entry_t e;
    string arg1, arg2, arg3;
    const char *real_argv[PROCFS_MAX_ARGS];

    for (i = 0; i < PROCFS_MAX_ARGS; i++) {
        real_argv[i] = (char *)0;
    }

    for (i = 0; i < total_file_patterns; i++) {

        e = &procfs_file_table[i];

        switch (e->argc) {
        case 0:
            if (e->compiled_pattern->FullMatch(path)) {
                goto out;
            }
            break;

        case 1:
            if (e->compiled_pattern->FullMatch(path, &arg1)) {
                real_argv[0] = arg1.c_str();
                goto out;
            }
            break;

        case 2:
            if (e->compiled_pattern->FullMatch(path, &arg1, &arg2)) {
                real_argv[0] = arg1.c_str();
                real_argv[1] = arg2.c_str();
                goto out;
            }
            break;

        case 3:
            if (e->compiled_pattern->FullMatch(path, &arg1, &arg2, &arg3)) {
                real_argv[0] = arg1.c_str();
                real_argv[1] = arg2.c_str();
                real_argv[2] = arg3.c_str();
                goto out;
            }
            break;
    
        default:
            return -EIO;
        }
    }   
    
    return -EIO;
    
out:    
    return e->read(e, real_argv, buf, size, offset, fi);
}

static struct fuse_operations procfs_oper;

static void
procfs_oper_populate(struct fuse_operations *oper)
{
    oper->init     = procfs_init;
    oper->destroy  = procfs_destroy;
    oper->getattr  = procfs_getattr;
    oper->read     = procfs_read;
    oper->readdir  = procfs_readdir;
    oper->readlink = procfs_readlink;
}

static char *def_opts = "-oallow_other,direct_io,nobrowse,nolocalcaches,ro";

int
main(int argc, char *argv[])
{
    int i;
    char **new_argv;

    argc++;
    new_argv = (char **)malloc(sizeof(char *) * argc);
    for (i = 0; i < (argc - 1); i++) {
        new_argv[i] = argv[i];
    }
    argv[i] = def_opts;

    procfs_oper_populate(&procfs_oper);
    return fuse_main(argc, argv, &procfs_oper, NULL);
}