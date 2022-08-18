/*
 * Copyright (c) 2017, 2020, Red Hat, Inc. All rights reserved.
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
 *
 */

/*
 * @test id=one_sec_delay
 * @summary Test that GCTrimNativeHeap works with Shenandoah on glibc platforms
 * @requires vm.gc.Shenandoah
 * @requires (os.family=="linux") & !vm.musl
 * @modules java.base/jdk.internal.misc
 * @library /test/lib
 * @run driver TestTrimNative one_sec_delay
 */

/*
 * @test id=high_delay
 * @summary Test that GCTrimNativeHeap works with Shenandoah on glibc platforms
 * @requires vm.gc.Shenandoah
 * @requires (os.family=="linux") & !vm.musl
 * @modules java.base/jdk.internal.misc
 * @library /test/lib
 * @run driver TestTrimNative high_delay
 */

/*
 * @test id=off
 * @summary Test that GCTrimNativeHeap works with Shenandoah on glibc platforms
 * @requires vm.gc.Shenandoah
 * @requires (os.family=="linux") & !vm.musl
 * @modules java.base/jdk.internal.misc
 * @library /test/lib
 * @run driver TestTrimNative off
 */

import jdk.internal.misc.Unsafe;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

public class TestTrimNative {

    // Actual RSS increase is a lot larger than 16MB. Depends on glibc overhead, and NMT malloc headers in debug VMs.
    // We need small-grained allocations to make sure they actually increase RSS (all touched) and to see the
    // glibc-retaining-memory effect.
    static final int numAllocations = 1024 * 1024;
    static final int szAllocations = 16;

    public static void main(String[] args) throws Exception {
        if (args.length == 0) {
            throw new RuntimeException("Argument error");
        }

        if (args[0].equals("RUN")) {
            long[] ptrs = new long[numAllocations];
            for (int i = 0; i < numAllocations; i++) {
                ptrs[i] = Unsafe.getUnsafe().allocateMemory(szAllocations);
            }
            for (int i = 0; i < numAllocations; i++) {
                Unsafe.getUnsafe().freeMemory(ptrs[i]);
            }
            Thread.sleep(5000); // give GC time to react
            return;

        } else if (args[0].equals("one_sec_delay")) {

            // Trimming should kick in after 1 second. Test lives long enough for that to happen.
            ProcessBuilder pb = ProcessTools.createJavaProcessBuilder("-Xlog:gc", "-XX:+UseShenandoahGC",
                    "-XX:+UnlockExperimentalVMOptions", "-XX:+GCTrimNativeHeap", "-XX:GCTrimNativeHeapDelay=1",
                    "-Xmx128m", "-Xms128m", "-XX:+AlwaysPreTouch", // helps stabilizing RSS
                    "--add-exports=java.base/jdk.internal.misc=ALL-UNNAMED",
                    TestTrimNative.class.getName(), "RUN");
            OutputAnalyzer output = new OutputAnalyzer(pb.start());
            output.shouldHaveExitValue(0);
            output.reportDiagnosticSummary();
            // We want to see an actual reduction in RSS in the megabyte range, e.g.
            // [1.247s][info][gc] Trimming native heap (retain size: 2M): RSS+Swap: 1120M->1072M (-48M)
            // [1.247s][info][gc] Concurrent trim-native 2.884ms
            output.shouldMatch("Concurrent trim-native.*ms");
            String m = output.firstMatch("Trimming native heap.*RSS\\+Swap.*\\((-\\d+)M\\)", 1);
            long l = Long.parseLong(m);
            if (l >= 0) {
                throw new RuntimeException("Expected decrease in RSS");
            }

        } else if (args[0].equals("off")) {

            // Switching the feature off should obviously not cause trimming
            ProcessBuilder pb = ProcessTools.createJavaProcessBuilder("-Xlog:gc", "-XX:+UseShenandoahGC",
                    "-XX:+UnlockExperimentalVMOptions", "-XX:-GCTrimNativeHeap",
                    "-Xmx128m",
                    "--add-exports=java.base/jdk.internal.misc=ALL-UNNAMED",
                    TestTrimNative.class.getName(), "RUN");
            OutputAnalyzer output = new OutputAnalyzer(pb.start());
            output.shouldHaveExitValue(0);
            output.reportDiagnosticSummary();
            output.shouldNotContain("Concurrent trim-native");

        } else if (args[0].equals("high_delay")) {

            // A high delay should cause GC not to attempt trimming (test program finishes under 10 seconds)
            ProcessBuilder pb = ProcessTools.createJavaProcessBuilder("-Xlog:gc", "-XX:+UseShenandoahGC",
                    "-XX:+UnlockExperimentalVMOptions", "-XX:+GCTrimNativeHeap", "-XX:GCTrimNativeHeapDelay=10000",
                    "-Xmx128m",
                    "--add-exports=java.base/jdk.internal.misc=ALL-UNNAMED",
                    TestTrimNative.class.getName(), "RUN");
            OutputAnalyzer output = new OutputAnalyzer(pb.start());
            output.shouldHaveExitValue(0);
            output.reportDiagnosticSummary();
            output.shouldNotContain("Concurrent trim-native");

        } else {
            throw new RuntimeException("Unknown test: " + args[0]);
        }

    }

}
