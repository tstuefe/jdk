/*
 * Copyright (c) 2015, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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
package jdk.tools.jlink.internal.plugins;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.Map;
import java.util.function.Predicate;
import java.util.zip.Deflater;
import jdk.tools.jlink.internal.ResourcePoolManager;
import jdk.tools.jlink.internal.ResourcePoolManager.ResourcePoolImpl;
import jdk.tools.jlink.plugin.ResourcePool;
import jdk.tools.jlink.plugin.ResourcePoolBuilder;
import jdk.tools.jlink.plugin.ResourcePoolEntry;

/**
 *
 * ZIP Compression plugin
 */
public final class ZipPlugin extends AbstractPlugin {

    private Predicate<String> predicate;

    private static final int DEFAULT_COMPRESSION = 6;
    private final int compressionLevel;

    public ZipPlugin() {
        this((Predicate<String>) null);
    }

    ZipPlugin(String[] patterns) {
        this(ResourceFilter.includeFilter(Arrays.asList(patterns)));
    }

    ZipPlugin(Predicate<String> predicate) {
        this(predicate, DEFAULT_COMPRESSION);
    }

    ZipPlugin(Predicate<String> predicate, int compressionLevel) {
        super("zip");
        this.predicate = predicate;
        this.compressionLevel = compressionLevel;
    }

    @Override
    public Category getType() {
        return Category.COMPRESSOR;
    }

    @Override
    public boolean hasArguments() {
        return false;
    }

    @Override
    public void configure(Map<String, String> config) {
        predicate = ResourceFilter.includeFilter(config.get(getName()));
    }

    static byte[] compress(byte[] bytesIn, int compressionLevel) {
        try (Deflater deflater = new Deflater(compressionLevel);
             ByteArrayOutputStream stream = new ByteArrayOutputStream(bytesIn.length)) {

            deflater.setInput(bytesIn);

            byte[] buffer = new byte[1024];

            deflater.finish();
            while (!deflater.finished()) {
                int count = deflater.deflate(buffer);
                stream.write(buffer, 0, count);
            }
            return stream.toByteArray(); // the compressed output
        } catch (IOException e) {
            // the IOException is only declared by ByteArrayOutputStream.close()
            // but the impl of ByteArrayOutputStream.close() is a no-op, so for
            // all practical purposes there should never be an IOException thrown
            assert false : "unexpected " + e;
            // don't propagate the exception, instead return the original uncompressed input
            return bytesIn;
        }
    }

    @Override
    public ResourcePool transform(ResourcePool in, ResourcePoolBuilder out) {
        in.transformAndCopy((resource) -> {
            ResourcePoolEntry res = resource;
            if (resource.type().equals(ResourcePoolEntry.Type.CLASS_OR_RESOURCE)
                    && predicate.test(resource.path())) {
                byte[] compressed;
                compressed = compress(resource.contentBytes(), this.compressionLevel);
                res = ResourcePoolManager.newCompressedResource(resource,
                        ByteBuffer.wrap(compressed), getName(),
                        ((ResourcePoolImpl)in).getStringTable(), in.byteOrder());
            }
            return res;
        }, out);

        return out.build();
    }
}
