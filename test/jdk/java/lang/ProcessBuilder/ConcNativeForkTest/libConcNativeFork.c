/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2026, IBM Corp.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include <assert.h>

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <jni.h>

#define TRC(s) { printf("[%d]: ", getpid()); printf s; printf("\n"); fflush(stdout); }

JNIEXPORT jlong JNICALL
Java_ConcNativeForkTest_doFork(JNIEnv* env, jclass cls)
{
    pid_t pid = fork();
    if (pid == 0) {
        TRC(("Child alive, will exec now"));
        /* exec sleep. Instead of calling sleep, we call the shell, since it has to be in /bin. It may
         * or may not execute sleep as builtin; either works for us. */
        char* env[] = { "PATH=/usr/bin:/bin", NULL };
        char* argv[] = { "sh", "-c", "sleep 30", NULL };
        execve("/bin/sh", argv, env); /* CLOEXEC fds will be closed here */
        TRC(("Child did not exec? %d", errno));
        /* The simplest way to handle this is to just wait here; this *will* cause the test to fail. */
        sleep(30);
        exit(-1);
    }
    TRC(("Created Child %d", pid));
    return (jlong) pid;
}

JNIEXPORT void JNICALL
Java_ConcNativeForkTest_doCleanup(JNIEnv *env, jclass cls, jlong jpid) {
    pid_t pid = (pid_t) jpid;
    TRC(("Kill Child %d", pid));
    kill((pid_t)pid, SIGKILL);
    TRC(("Reap Child %d", pid));
    waitpid((pid_t)pid, NULL, 0);
}

JNIEXPORT void JNICALL
Java_ConcNativeForkTest_makeProcessCreationSlow(JNIEnv *env, jclass cls) {
    TRC(("JTREG_JSPAWNHELPER_DELAY_TEST=1"));
    setenv("JTREG_JSPAWNHELPER_DELAY_TEST", "1", 1); // set for current process
}
