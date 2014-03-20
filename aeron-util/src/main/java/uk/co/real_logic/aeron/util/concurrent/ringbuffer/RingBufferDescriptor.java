/*
 * Copyright 2014 Real Logic Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package uk.co.real_logic.aeron.util.concurrent.ringbuffer;

import uk.co.real_logic.aeron.util.BitUtil;

public class RingBufferDescriptor
{
    /** Offset within the trailer for where the tail value is stored. */
    public static final int TAIL_COUNTER_OFFSET;

    /** Offset within the trailer for where the head value is stored. */
    public static final int HEAD_COUNTER_OFFSET;

    /** Offset within the trailer for where the head value is stored. */
    public static final int CORRELATION_COUNTER_OFFSET;

    /** Total size of the trailer */
    public static final int TRAILER_SIZE;

    static
    {
        int offset = 0;
        TAIL_COUNTER_OFFSET = offset;

        offset += BitUtil.CACHE_LINE_SIZE;
        HEAD_COUNTER_OFFSET = offset;

        offset += BitUtil.CACHE_LINE_SIZE;
        CORRELATION_COUNTER_OFFSET = offset;

        offset += BitUtil.CACHE_LINE_SIZE;
        TRAILER_SIZE = offset;
    }
}
