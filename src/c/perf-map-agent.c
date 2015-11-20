/*
 *   libperfmap: a JVM agent to create perf-<pid>.map files for consumption
 *               with linux perf-tools
 *   Copyright (C) 2013-2015 Johannes Rudolph<johannes.rudolph@gmail.com>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License along
 *   with this program; if not, write to the Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>

#include <jni.h>
#include <jvmti.h>
#include <jvmticmlr.h>

#include "perf-map-file.h"

FILE *method_file = NULL;
int unfold_inlined_methods = 0;
int unfold_simple = 0;
int print_method_signatures = 0;
int print_source_loc = 0;
int clean_class_names = 0;

void open_map_file() {
    if (!method_file)
        method_file = perf_map_open(getpid());
}
void close_map_file() {
    perf_map_close(method_file);
    method_file = NULL;
}

static int get_line_number(jvmtiLineNumberEntry *table, jint entry_count, jlocation loc) {
  int i;
  for (i = 0; i < entry_count; i++)
    if (table[i].start_location > loc) return table[i - 1].line_number;

  return -1;
}

void class_name_from_sig(char *dest, size_t dest_size, const char *sig) {
    if (clean_class_names && sig[0] == 'L') {
        const char *src = sig + 1;
        int i;
        for(i = 0; i < (dest_size - 1) && src[i]; i++) {
            char c = src[i];
            if (c == '/') c = '.';
            if (c == ';') c = 0;
            dest[i] = c;
        }
        dest[i] = 0;
    } else
        strncpy(dest, sig, dest_size);
}

static void sig_string(jvmtiEnv *jvmti, jmethodID method, char *output, size_t noutput) {
    char *sourcefile = NULL;
    char *name = NULL;
    char *msig = NULL;
    char *csig = NULL;
    char *empty = "";
    jvmtiLineNumberEntry *lines = NULL;

    jclass class;
    jvmtiError error = 0;
    jint entrycount = 0;

    strncpy(output, "<error writing signature>", noutput);

    if (!(*jvmti)->GetMethodName(jvmti, method, &name, &msig, NULL)) {
        if (!(*jvmti)->GetMethodDeclaringClass(jvmti, method, &class) &&
            !(*jvmti)->GetClassSignature(jvmti, class, &csig, NULL)) {

            char source_info[1000] = "";
            char *method_signature = "";

            if (print_source_loc) {
                if (!(*jvmti)->GetSourceFileName(jvmti, class, &sourcefile)) {
                    if (!(*jvmti)->GetLineNumberTable(jvmti, method, &entrycount, &lines)) {
                        int lineno = -1;
                        if(entrycount > 0) lineno = lines[0].line_number;
                        snprintf(source_info, sizeof(source_info), "(%s:%d)", sourcefile, lineno);

                        if (lines != NULL) (*jvmti)->Deallocate(jvmti, (unsigned char *) lines);
                    }
                    if (sourcefile != NULL) (*jvmti)->Deallocate(jvmti, sourcefile);
                }
            }

            if (print_method_signatures && msig)
                method_signature = msig;

            char class_name[2000];
            class_name_from_sig(class_name, sizeof(class_name), csig);
            snprintf(output, noutput, "%s.%s%s%s", class_name, name, method_signature, source_info);

            if (csig != NULL) (*jvmti)->Deallocate(jvmti, csig);
        }
        if (name != NULL) (*jvmti)->Deallocate(jvmti, name);
        if (msig != NULL) (*jvmti)->Deallocate(jvmti, msig);
    }
}

void generate_single_entry(jvmtiEnv *jvmti, jmethodID method, const void *code_addr, jint code_size) {
    char entry[2000];
    sig_string(jvmti, method, entry, sizeof(entry));
    perf_map_write_entry(method_file, code_addr, code_size, entry);
}

/* Generates either a simple or a complex unfolded entry. */
void generate_unfolded_entry(jvmtiEnv *jvmti, jmethodID method, char *buffer, size_t buffer_size, const char *root_name) {
    if (unfold_simple)
        sig_string(jvmti, method, buffer, buffer_size);
    else {
        char entry_name[2000];
        sig_string(jvmti, method, entry_name, sizeof(entry_name));
        snprintf(buffer, buffer_size, "%s in %s", entry_name, root_name);
    }
}

/* Generates and writes a single entry for a given inlined method. */
void write_unfolded_entry(
        jvmtiEnv *jvmti,
        jmethodID cur_method,
        jmethodID root_method,
        const char *root_name,
        const void *start_addr,
        const void *end_addr) {
    // needs to accommodate: entry_name + " in " + root_name
    char inlined_name[2000 * 2 + 4];
    const char *entry_p;

    if (cur_method != root_method) {
        generate_unfolded_entry(jvmti, cur_method, inlined_name, sizeof(inlined_name), root_name);
        entry_p = inlined_name;
    } else
        entry_p = root_name;

    perf_map_write_entry(method_file, start_addr, end_addr - start_addr, entry_p);
}

void generate_unfolded_entries(
        jvmtiEnv *jvmti,
        jmethodID root_method,
        jint code_size,
        const void* code_addr,
        jint map_length,
        const jvmtiAddrLocationMap* map,
        const void* compile_info) {
    const jvmtiCompiledMethodLoadRecordHeader *header = compile_info;
    char root_name[2000];

    sig_string(jvmti, root_method, root_name, sizeof(root_name));
    if (header->kind == JVMTI_CMLR_INLINE_INFO) {
        const jvmtiCompiledMethodLoadInlineRecord *record = (jvmtiCompiledMethodLoadInlineRecord *) header;

        const void *start_addr = code_addr;
        jmethodID cur_method = root_method;

        // walk through the method meta data per PC to extract address range
        // per inlined method.
        int i;
        for (i = 0; i < record->numpcs; i++) {
            PCStackInfo *info = &record->pcinfo[i];
            jmethodID top_method = info->methods[0];

            // as long as the top method remains the same we delay recording
            if (cur_method != top_method) {

                // top method has changed, record the range for current method
                void *end_addr = info->pc;
                write_unfolded_entry(jvmti, cur_method, root_method, root_name, start_addr, end_addr);

                start_addr = info->pc;
                cur_method = top_method;
            }
        }

        // record the last range if there's a gap
        if (start_addr != code_addr + code_size) {
            // end_addr is end of this complete code blob
            const void *end_addr = code_addr + code_size;
            write_unfolded_entry(jvmti, cur_method, root_method, root_name, start_addr, end_addr);
        }
    } else
        generate_single_entry(jvmti, root_method, code_addr, code_size);
}

static void JNICALL
cbCompiledMethodLoad(
            jvmtiEnv *jvmti,
            jmethodID method,
            jint code_size,
            const void* code_addr,
            jint map_length,
            const jvmtiAddrLocationMap* map,
            const void* compile_info) {
    if (unfold_inlined_methods)
        generate_unfolded_entries(jvmti, method, code_size, code_addr, map_length, map, compile_info); 
    else
        generate_single_entry(jvmti, method, code_addr, code_size);
}

void JNICALL
cbDynamicCodeGenerated(jvmtiEnv *jvmti,
            const char* name,
            const void* address,
            jint length) {
    perf_map_write_entry(method_file, address, length, name);
}

void set_notification_mode(jvmtiEnv *jvmti, jvmtiEventMode mode) {
    (*jvmti)->SetEventNotificationMode(jvmti, mode,
          JVMTI_EVENT_COMPILED_METHOD_LOAD, (jthread)NULL);
    (*jvmti)->SetEventNotificationMode(jvmti, mode,
          JVMTI_EVENT_DYNAMIC_CODE_GENERATED, (jthread)NULL);
}

jvmtiError enable_capabilities(jvmtiEnv *jvmti) {
    jvmtiCapabilities capabilities;

    memset(&capabilities,0, sizeof(capabilities));
    capabilities.can_generate_all_class_hook_events  = 1;
    capabilities.can_tag_objects                     = 1;
    capabilities.can_generate_object_free_events     = 1;
    capabilities.can_get_source_file_name            = 1;
    capabilities.can_get_line_numbers                = 1;
    capabilities.can_generate_vm_object_alloc_events = 1;
    capabilities.can_generate_compiled_method_load_events = 1;

    // Request these capabilities for this JVM TI environment.
    return (*jvmti)->AddCapabilities(jvmti, &capabilities);
}

jvmtiError set_callbacks(jvmtiEnv *jvmti) {
    jvmtiEventCallbacks callbacks;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.CompiledMethodLoad  = &cbCompiledMethodLoad;
    callbacks.DynamicCodeGenerated = &cbDynamicCodeGenerated;
    return (*jvmti)->SetEventCallbacks(jvmti, &callbacks, (jint)sizeof(callbacks));
}

JNIEXPORT jint JNICALL
Agent_OnAttach(JavaVM *vm, char *options, void *reserved) {
    open_map_file();

    unfold_simple = strstr(options, "unfoldsimple") != NULL;
    unfold_inlined_methods = strstr(options, "unfold") != NULL || unfold_simple;
    print_method_signatures = strstr(options, "msig") != NULL;
    print_source_loc = strstr(options, "sourcepos") != NULL;
    clean_class_names = strstr(options, "dottedclass") != NULL;

    jvmtiEnv *jvmti;
    (*vm)->GetEnv(vm, (void **)&jvmti, JVMTI_VERSION_1);
    enable_capabilities(jvmti);
    set_callbacks(jvmti);
    set_notification_mode(jvmti, JVMTI_ENABLE);
    (*jvmti)->GenerateEvents(jvmti, JVMTI_EVENT_DYNAMIC_CODE_GENERATED);
    (*jvmti)->GenerateEvents(jvmti, JVMTI_EVENT_COMPILED_METHOD_LOAD);
    set_notification_mode(jvmti, JVMTI_DISABLE);
    close_map_file();

    return 0;
}
