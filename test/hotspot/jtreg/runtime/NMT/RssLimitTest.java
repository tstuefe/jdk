/*
 * Copyright (c) 2025 Red Hat, Inc. All rights reserved.
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
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
 * @test id=test-absolute-limit-fatal
 * @summary Verify -XX:RssLimit fatal mode
 * @requires vm.flagless
 * @requires os.family != "aix"
 * @modules java.base/jdk.internal.misc
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI RssLimitTest test-absolute-limit-fatal
 */

/*
 * @test id=test-absolute-limit-nonfatal
 * @summary Verify -XX:RssLimit non-fatal mode
 * @requires vm.flagless
 * @requires os.family != "aix"
 * @modules java.base/jdk.internal.misc
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI RssLimitTest test-absolute-limit-nonfatal
 */

/*
 * @test id=test-relative-limit-fatal
 * @summary Verify -XX:RssLimitPercent fatal mode
 * @requires vm.flagless
 * @requires os.family != "aix"
 * @modules java.base/jdk.internal.misc
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI RssLimitTest test-relative-limit-fatal
 */

/*
 * @test id=test-relative-limit-nonfatal
 * @summary Verify -XX:RssLimitPercent non-fatal mode
 * @requires vm.flagless
 * @requires os.family != "aix"
 * @modules java.base/jdk.internal.misc
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI RssLimitTest test-relative-limit-nonfatal
 */

/*
 * @test id=test-high-interval
 * @summary Verify -XX:RssLimit with a very high test interval
 * @requires vm.flagless
 * @requires os.family != "aix"
 * @modules java.base/jdk.internal.misc
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI RssLimitTest test-high-interval
 */

/*
 * @test id=test-default-off
 * @summary Verify -XX:RssLimit is by default disabled
 * @requires vm.flagless
 * @requires os.family != "aix"
 * @modules java.base/jdk.internal.misc
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI RssLimitTest test-default-off
 */

import jdk.test.lib.Platform;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;
import jdk.test.whitebox.WhiteBox;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public class RssLimitTest {

    final static long heapsize = 64 * 1024 * 1024;

    // A limit size that is guaranteed to fire (since we touch the whole heap, and the JVM allocates
    // more than just the heap)
    final static long guaranteedTriggerSize = heapsize + (1024 * 1024);

    final static int limitCheckInterval = 400; // milliseconds

    private static long queryPhysicalMemory() {
        return WhiteBox.getWhiteBox().hostPhysicalMemory();
    }
    private static long queryPageSize() {
        return WhiteBox.getWhiteBox().getVMPageSize();
    }
    private static long pagesize_aligned(long l) {
        long pagesize = queryPageSize();
        return (l / pagesize) * pagesize;
    }
    private static long calculateWarningThreshold(long limitThreshold) {
        return pagesize_aligned((long)(limitThreshold * .8));
    }

    private static void testExpectedSettingsAbsLimit(OutputAnalyzer output,
                                                     long absolute_limit,
                                                     boolean is_fatal_flag_set)
    {
        long physical = queryPhysicalMemory();
        // Keep in sync with RssWatcher::update_limit_thresholds
        long expected_limit_threshold = pagesize_aligned(absolute_limit);
        long expected_warning_threshold = calculateWarningThreshold(expected_limit_threshold);

        output.shouldMatch("physical memory: *" + physical);
        output.shouldMatch("limit threshold: *" + absolute_limit);
        output.shouldMatch("warning threshold: *" + expected_warning_threshold);
        output.shouldMatch("threshold aborts VM: *" + (is_fatal_flag_set ? "yes" : "no"));
        output.shouldMatch("RssLimitCheckInterval: *" + limitCheckInterval + "ms");
    }

    private static void testExpectedSettingsRelLimit(OutputAnalyzer output,
                                                     double rel_limit_percent,
                                                     boolean is_fatal_flag_set)
    {
        long physical = queryPhysicalMemory();
        // Keep in sync with RssWatcher::update_limit_thresholds
        long expected_limit_threshold = pagesize_aligned((long) ((rel_limit_percent * physical) / 100.0));
        long expected_warning_threshold = calculateWarningThreshold(expected_limit_threshold);

        output.shouldMatch("limit threshold: *" + expected_limit_threshold);
        output.shouldMatch("warning threshold: *" + expected_warning_threshold);
        output.shouldMatch("threshold aborts VM: *" + (is_fatal_flag_set ? "yes" : "no"));
        output.shouldMatch("RssLimitCheckInterval: *" + limitCheckInterval + "ms");
    }

    private static void testReport(OutputAnalyzer output) {
        output.shouldMatch("\\*\\*\\* Error: rss \\(\\d+\\) over limit threshold \\(\\d+\\) \\*\\*\\*");
        long rss = Long.parseLong(output.firstMatch("rss \\((\\d+)\\) over limit", 1));
        long threshold = Long.parseLong(output.firstMatch("over limit threshold \\((\\d+)\\)", 1));

        if (rss == 0 || rss < threshold) {
            throw new RuntimeException("Strangeness");
        }

        output.shouldContain("RSS History:");
        output.shouldMatch("\\d+ seconds.*: \\d+");

        output.shouldContain("Process Memory Info:");
        if (Platform.isLinux() || Platform.isOSX()) {
            output.shouldMatch("Resident Set Size: \\d+K");
        } else if (Platform.isWindows()) {
            output.shouldContain("WorkingSet");
        }
        // AIX has not yet implemented os::rss()

        output.shouldContain("Native Memory Tracking:");
        output.shouldContain("Java Heap (reserved=" + heapsize/1024 + "KB, committed=" + heapsize/1024 + "KB)");

        output.shouldContain("Compilation Memory History:");
        output.shouldMatch("c[12] *\\d+ *\\d+");

        output.shouldContain("Memory Map:");
        if (Platform.isLinux()) {
            output.shouldMatch("0x[0-9a-f]*-0x[0-9a-f]* .*JAVAHEAP");
        }
    }

    private static OutputAnalyzer runWithSettings(String... extraSettings) throws IOException {
        List<String> args = new ArrayList<>();
        args.add("-XX:+UnlockDiagnosticVMOptions"); // RssLimit is diagnostic
        args.add("-XX:-CreateCoredumpOnCrash");
        args.add("-Xlog:os+rss");
        args.add("-XX:+UseG1GC");
        args.add("-Xmx" + heapsize);
        args.add("-Xms" + heapsize);
        args.add("-XX:+AlwaysPreTouch");
        args.add("-XX:+UseG1GC");
        // We enable NMT
        args.add("-XX:NativeMemoryTracking=summary");
        // And also the compilation memory statistic
        args.add("-XX:CompileCommand=memstat,*.*");
        args.add("-XX:RssLimitCheckInterval=" + limitCheckInterval);

        args.addAll(Arrays.asList(extraSettings));
        args.add(RssLimitTest.class.getName());
        args.add("sleep");
        ProcessBuilder pb = ProcessTools.createLimitedTestJavaProcessBuilder(args);
        OutputAnalyzer o = new OutputAnalyzer(pb.start());
        return o;
    }

    private static void testFatalError(OutputAnalyzer output) {
        output.shouldNotHaveExitValue(0);
        output.shouldMatch("fatal error: \\*\\*\\* Error: rss \\(\\d+\\) over limit threshold \\(\\d+\\) \\*\\*\\*");
    }

    private static void testAbsoluteLimit(boolean fatal) throws IOException {
        String limitoption = "-XX:RssLimit=" + guaranteedTriggerSize;
        if (fatal) {
            limitoption += ":fatal";
        }
        OutputAnalyzer o = runWithSettings(limitoption);
        o.shouldContain("RssLimit watcher enabled");
        testExpectedSettingsAbsLimit(o, guaranteedTriggerSize, fatal);
        testReport(o);
        if (fatal) {
            testFatalError(o);
        } else {
            o.shouldHaveExitValue(0);
        }
    }

    private static void testRelativeLimit(boolean fatal) throws IOException {
        long physical = queryPhysicalMemory();
        double percent = (double) guaranteedTriggerSize * 100.0 / physical;
        String limitoption = "-XX:RssLimitPercent=" + percent;
        if (fatal) {
            limitoption += ":fatal";
        }
        OutputAnalyzer o = runWithSettings(limitoption);
        o.shouldContain("RssLimit watcher enabled");
        testExpectedSettingsRelLimit(o, percent, fatal);
        testReport(o);
        if (fatal) {
            testFatalError(o);
        } else {
            o.shouldHaveExitValue(0);
        }
    }

    private static void testLimitWithVeryHighInterval() throws IOException {
        // Limit should be enabled but nothing should happen since it should not fire.
        // (to be really sure that the limit would fire were the check done timely, set
        // the limit super-low).
        OutputAnalyzer o = runWithSettings(
                "-XX:RssLimit=1m", "-XX:RssLimitCheckInterval=120000",
                "-Xmx100m");
        o.shouldContain("RssLimit watcher enabled");
        o.shouldMatch("RssLimitCheckInterval: *120000ms");
        o.shouldNotContain("Error");
        o.shouldHaveExitValue(0);
    }

    private static void testDefaultOff() throws IOException {
        // Do not specify RssLimit; should by default be off.
        OutputAnalyzer o = runWithSettings("-Xmx100m");
        o.shouldNotContain("RssWatcher");
        o.shouldNotContain("Error");
        o.shouldHaveExitValue(0);
    }

    public static void main(String[] args) throws Exception {
        switch (args[0]) {
            case "sleep" ->
                // long enough to let the rss watcher fire at least once
                Thread.sleep(2000);
            case "test-absolute-limit-fatal" -> testAbsoluteLimit(true);
            case "test-absolute-limit-nonfatal" -> testAbsoluteLimit(false);
            case "test-relative-limit-fatal" -> testRelativeLimit(true);
            case "test-relative-limit-nonfatal" -> testRelativeLimit(false);
            case "test-high-interval" -> testLimitWithVeryHighInterval();
            case "test-default-off" -> testDefaultOff();
            default -> throw new RuntimeException("Invalid argument (" + args[0] + ")");
        }
    }
}
