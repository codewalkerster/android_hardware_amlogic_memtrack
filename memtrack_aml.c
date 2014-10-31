/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "memtrack_aml"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include <hardware/memtrack.h>
#include <log/log.h>

#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))
#define min(x, y) ((x) < (y) ? (x) : (y))

static struct hw_module_methods_t memtrack_module_methods = {
    .open = NULL,
};

struct memtrack_record record_templates[] = {
    {
        .flags = MEMTRACK_FLAG_SMAPS_UNACCOUNTED |
                 MEMTRACK_FLAG_PRIVATE |
                 MEMTRACK_FLAG_NONSECURE,
    },

/*
    {
        .flags = MEMTRACK_FLAG_SMAPS_ACCOUNTED |
                 MEMTRACK_FLAG_PRIVATE |
                 MEMTRACK_FLAG_NONSECURE,
    },
*/
};

// just return 0
int aml_memtrack_init(const struct memtrack_module *module)
{
    return 0;
}

/*
 * find the userid of process @pid
 * return the userid if success, or return -1 if not
 */
int memtrack_find_userid(int pid)
{
    FILE *fp;
    char line[1024];
    char tmp[128];
    int userid;

    sprintf(tmp, "/proc/%d/status", pid);
    if ((fp=fopen(tmp, "r")) == NULL) {
        ALOGD("open file %s error %s", tmp, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (sscanf(line, "Uid: %d", &userid) == 1) {
            fclose(fp);
            return userid;
        }
    }

    // should never reach here
    fclose(fp);
    return -1;
}

unsigned int memtrack_get_gpuMem(int pid)
{
    FILE *fp;
    char line[1024];
    char tmp[128];
    unsigned int size, sum = 0;
    int skip, done = 0;

    unsigned long int start, end;
    int len;
    char *name;
    int nameLen, name_pos;

    sprintf(tmp, "/proc/%d/smaps", pid);
    fp = fopen(tmp, "r");
    if (fp == 0) {
        ALOGD("open file %s error %s", tmp, strerror(errno));
        return 0;
     }


    if(fgets(line, sizeof(line), fp) == 0) 
        return 0;

    while (!done) {
        skip = 0;

        len = strlen(line);
        if (len < 1) 
            return 0;
        line[--len] = 0;

        if (sscanf(line, "%lx-%lx %*s %*x %*x:%*x %*d%n", &start, &end, &name_pos) != 2) {
            skip = 1;
        } else {
            while (isspace(line[name_pos])) {
                name_pos += 1;
            }
            name = line + name_pos;
            nameLen = strlen(name);

            if (nameLen >= 8 && 
                    (!strncmp(name, "/dev/mali", 6) || !strncmp(name, "/dev/ump", 6))) {
                skip = 0;
            } else {
                skip = 1;
            }
            
        }

        while (1) {
            if (fgets(line, 1024, fp) == 0) {
                done = 1;
                break;
            }

            if(!skip) {
                if (line[0] == 'S' && sscanf(line, "Size: %d kB", &size) == 1) {
                    sum += size;
                } 
            }
            
            if (strlen(line) > 30 && line[8] == '-' && line[17] == ' ') {
                // looks like a new mapping
                // example: "10000000-10001000 ---p 10000000 00:00 0"
                break;
            }
        }

    }

    // converted into Bytes
    return (sum * 1024);
}
    
int memtrack_get_memory(pid_t pid, enum memtrack_type type,
                             struct memtrack_record *records,
                             size_t *num_records)
{
    FILE *fp;
    FILE *ion_fp;
    char line[1024];
    char tmp[128];
    unsigned int mali_inuse = 0;
    unsigned int size;
    size_t unaccounted_size = 0;

    char ion_name[128];
    int ion_pid;
    unsigned int ion_size;
    unsigned int gpu_size;


   // ALOGD("type is %d, pid is %d\n", type, pid);
    size_t allocated_records =  ARRAY_SIZE(record_templates);
    *num_records = ARRAY_SIZE(record_templates);

    if (records == NULL) {
        return 0;
    }

    memcpy(records, record_templates, sizeof(struct memtrack_record) * allocated_records);

    if (type == MEMTRACK_TYPE_GL) {
        // find the user id of the process, only support calculate the non root process
        int ret = memtrack_find_userid(pid);
        if (ret <= 0) {
            return -1;
        }
        gpu_size = memtrack_get_gpuMem(pid); 
        unaccounted_size += gpu_size;
    } else if (type == MEMTRACK_TYPE_GRAPHICS) {
        sprintf(tmp, "/proc/ion/vmalloc_ion"); 
       // sprintf(tmp, "/sys/kernel/debug/ion/vmalloc_ion"); 
        if ((ion_fp = fopen(tmp, "r")) == NULL) {
            ALOGD("open file %s error %s", tmp, strerror(errno));
            return -errno;
        } 

        while(fgets(line, sizeof(line), ion_fp) != NULL) {
            if (sscanf(line, "%s%d%u", ion_name, &ion_pid, &ion_size) != 3) {
                continue;
            } else {
                if (ion_pid == pid) {
                    unaccounted_size += ion_size;
                }
            }
    
        }

        fclose(ion_fp);
    }

    if (allocated_records > 0) {
        records[0].size_in_bytes = unaccounted_size;
       // ALOGD("graphic %u\n", unaccounted_size);
    }

    return 0;
}

int aml_memtrack_get_memory(const struct memtrack_module *module,
                                pid_t pid,
                                int type,
                                struct memtrack_record *records,
                                size_t *num_records)
{
    if (type == MEMTRACK_TYPE_GL || type == MEMTRACK_TYPE_GRAPHICS) {
        return memtrack_get_memory(pid, type, records, num_records);

    } else {
        return -EINVAL;
    }
}


struct memtrack_module HAL_MODULE_INFO_SYM = {
    common: {
        tag: HARDWARE_MODULE_TAG,
        module_api_version: MEMTRACK_MODULE_API_VERSION_0_1,
        hal_api_version: HARDWARE_HAL_API_VERSION,
        id: MEMTRACK_HARDWARE_MODULE_ID,
        name: "aml Memory Tracker HAL",
        author: "amlogic",
        methods: &memtrack_module_methods,
    },

    init: aml_memtrack_init,
    getMemory: aml_memtrack_get_memory,
};
