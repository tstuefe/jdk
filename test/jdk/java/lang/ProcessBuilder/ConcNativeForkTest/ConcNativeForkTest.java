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

/*
 * @test id=POSIX_SPAWN
 * @requires os.family != "windows"
 * @requires vm.flagless
 * @library /test/lib
 * @run main/othervm/timeout=50 -Djdk.lang.Process.launchMechanism=POSIX_SPAWN ConcNativeForkTest
 */

/*
 * @test id=FORK
 * @requires os.family != "windows"
 * @requires vm.flagless
 * @library /test/lib
 * @run main/othervm/timeout=50 -Djdk.lang.Process.launchMechanism=FORK ConcNativeForkTest
 */

/*
 * @test id=VFORK
 * @requires os.family == "linux"
 * @requires vm.flagless
 * @library /test/lib
 * @run main/othervm/timeout=50 -Djdk.lang.Process.launchMechanism=VFORK ConcNativeForkTest
 */

import java.util.Stack;

public class ConcNativeForkTest {

    static boolean stopNativeForking = false;
    native static long doFork();
    native static void doCleanup(long pid);
    native static void makeProcessCreationSlow();

    static Stack<Long> nativChildrenPIDs = new Stack<>();

    public static void main(String[] args) throws Exception {

        System.out.println("jdk.lang.Process.launchMechanism=" + System.getProperty("jdk.lang.Process.launchMechanism"));

        System.loadLibrary("ConcNativeFork");

        // How this works:
        // - We start a child process A. Does not matter what, we just call "/bin/true".
        // - We introduce an artificial (5 seconds) delay between pipe creation and pipe usage in ProcessBuilder.start()
        //    (see JTREG_JSPAWNHELPER_DELAY_TEST);
        // - Concurrently to that, we will continuously - in 1 second intervals - call fork()+exec() in native code
        //    to start "native" child processes B+; these processes all run "sleep" for 30 seconds.
        // - Since the fork interval of child processes B+ is smaller than the time we need to start child process A
        //    (see JTREG_JSPAWNHELPER_DELAY_TEST), it is certain that in that time window several child processes B+
        //    will have been started. These child processes carry copies of the pipe file descriptors created for A,
        //    and if those were incorrectly created (without CLOEXEC), will keep them open for the full length of the
        //    30 second sleep interval.
        // - This means that the ProcessBuilder.start() call to create A will take 5 (start delay) + 30 (runtime of last
        //    "intersecting" process B) seconds, since ProcessBuilder.start() will wait for the fail pipe to close, and
        //    the last intersecting child B will keep that open as long as it lives.
        // - But if all goes well, the ProcessBuilder.start() should just take 5 seconds (start delay).

        makeProcessCreationSlow();

        String s = System.getenv("JTREG_JSPAWNHELPER_DELAY_TEST");
        if (s == null || !s.equals("1")) {
            throw new RuntimeException("JTREG_JSPAWNHELPER_DELAY_TEST should be set now");
        }

        Thread nativeForkerThread = new Thread(() -> {
            int safety = 15;
            while (!stopNativeForking && safety-- > 0) {
                long pid = doFork();
                if (pid == -1) {
                    throw new RuntimeException("Native Fork Error");
                }
                nativChildrenPIDs.add(pid);
                try {
                    Thread.sleep(1000); // wait 1 second
                } catch (InterruptedException e) {
                    return;
                }
            }
        });

        // Start to fork natively, in 1 second intervals
        nativeForkerThread.start();

        ProcessBuilder pb = new ProcessBuilder("true");

        long t1 = System.currentTimeMillis();

        // Start /bin/true.
        try (Process p = pb.start()) { // delay 5 seconds

            // If ProcessBuilder.start() returns after 5ish seconds, all is well.
            // If ProcessBuilder.start() returns after 35ish seconds, it means it had to wait for all
            // native children that forked off in its 5 second delay time window.
            long t2 = System.currentTimeMillis();

            // Wait for child (/bin/true runs quick)
            p.waitFor();

            // Stop creating native children; reap native children
            stopNativeForking = true;
            nativeForkerThread.join();
            while (!nativChildrenPIDs.isEmpty()) {
                doCleanup(nativChildrenPIDs.pop());
            }

            // child run should have been successfully
            if (p.exitValue() != 0) { // true returns 0
                throw new RuntimeException("Failed");
            }

            // Examine time it took to spawn off the child. Too long? Suspicious.
            long forkTime = t2 - t1;
            System.out.println("Took " + forkTime + "ms");

            final long delayTime = 5000; // JTREG_JSPAWNHELPER_DELAY_TEST

            // program startup time is usually very quick, but test machine may be slow,
            // so allow for a generous time here:
            final long maxProgramStartupTime = 5000;

            final long maxForkTimeExpected = delayTime + maxProgramStartupTime;
            if (forkTime >= maxForkTimeExpected) {
                throw new RuntimeException("Took too long => suspicious");
            }
        }
    }
}
