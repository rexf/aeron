/*
 * Copyright 2014 - 2016 Real Logic Ltd.
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
package io.aeron.logbuffer;

import org.agrona.DirectBuffer;
import org.agrona.concurrent.UnsafeBuffer;

import static io.aeron.logbuffer.FrameDescriptor.FRAME_ALIGNMENT;
import static io.aeron.protocol.DataHeaderFlyweight.HEADER_LENGTH;
import static org.agrona.BitUtil.CACHE_LINE_LENGTH;
import static org.agrona.BitUtil.SIZE_OF_INT;
import static org.agrona.BitUtil.SIZE_OF_LONG;

/**
 * Layout description for log buffers which contains partitions of terms with associated term meta data,
 * plus ending with overall log meta data.
 *
 * <pre>
 *  +----------------------------+
 *  |           Term 0           |
 *  +----------------------------+
 *  |           Term 1           |
 *  +----------------------------+
 *  |           Term 2           |
 *  +----------------------------+
 *  |        Log Meta Data       |
 *  +----------------------------+
 * </pre>
 */
public class LogBufferDescriptor
{
    /**
     * The number of partitions the log is divided into.
     */
    public static final int PARTITION_COUNT = 3;

    /**
     * Section index for which buffer contains the log meta data.
     */
    public static final int LOG_META_DATA_SECTION_INDEX = PARTITION_COUNT;

    /**
     * Minimum buffer length for a log term
     */
    public static final int TERM_MIN_LENGTH = 64 * 1024;

    // *******************************
    // *** Log Meta Data Constants ***
    // *******************************

    /**
     * Offset within the meta data where the tail values are stored.
     */
    public static final int TERM_TAIL_COUNTERS_OFFSET;

    /**
     * Offset within the log meta data where the active partition index is stored.
     */
    public static final int LOG_ACTIVE_PARTITION_INDEX_OFFSET;

    /**
     * Offset within the log meta data where the time of last SM is stored.
     */
    public static final int LOG_TIME_OF_LAST_SM_OFFSET;

    /**
     * Offset within the log meta data where the active term id is stored.
     */
    public static final int LOG_INITIAL_TERM_ID_OFFSET;

    /**
     * Offset within the log meta data which the length field for the frame header is stored.
     */
    public static final int LOG_DEFAULT_FRAME_HEADER_LENGTH_OFFSET;

    /**
     * Offset within the log meta data which the MTU length is stored;
     */
    public static final int LOG_MTU_LENGTH_OFFSET;

    /**
     * Offset within the log meta data which the
     */
    public static final int LOG_CORRELATION_ID_OFFSET;

    /**
     * Offset at which the default frame headers begin.
     */
    public static final int LOG_DEFAULT_FRAME_HEADER_OFFSET;

    /**
     * Maximum length of a frame header.
     */
    public static final int LOG_DEFAULT_FRAME_HEADER_MAX_LENGTH = CACHE_LINE_LENGTH * 2;

    static
    {
        int offset = 0;
        TERM_TAIL_COUNTERS_OFFSET = offset;

        offset += (SIZE_OF_LONG * PARTITION_COUNT);
        LOG_ACTIVE_PARTITION_INDEX_OFFSET = offset;

        offset = (CACHE_LINE_LENGTH * 2);
        LOG_TIME_OF_LAST_SM_OFFSET = offset;

        offset += (CACHE_LINE_LENGTH * 2);
        LOG_CORRELATION_ID_OFFSET = offset;
        LOG_INITIAL_TERM_ID_OFFSET = LOG_CORRELATION_ID_OFFSET + SIZE_OF_LONG;
        LOG_DEFAULT_FRAME_HEADER_LENGTH_OFFSET = LOG_INITIAL_TERM_ID_OFFSET + SIZE_OF_INT;
        LOG_MTU_LENGTH_OFFSET = LOG_DEFAULT_FRAME_HEADER_LENGTH_OFFSET + SIZE_OF_INT;

        offset += CACHE_LINE_LENGTH;
        LOG_DEFAULT_FRAME_HEADER_OFFSET = offset;

        LOG_META_DATA_LENGTH = offset + LOG_DEFAULT_FRAME_HEADER_MAX_LENGTH;
    }

    /**
     * Total length of the log meta data buffer in bytes.
     *
     * <pre>
     *   0                   1                   2                   3
     *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |                       Tail Counter 0                          |
     *  |                                                               |
     *  +---------------------------------------------------------------+
     *  |                       Tail Counter 1                          |
     *  |                                                               |
     *  +---------------------------------------------------------------+
     *  |                       Tail Counter 2                          |
     *  |                                                               |
     *  +---------------------------------------------------------------+
     *  |                   Active Partition Index                      |
     *  +---------------------------------------------------------------+
     *  |                      Cache Line Padding                      ...
     * ...                                                              |
     *  +---------------------------------------------------------------+
     *  |                 Time of Last Status Message                   |
     *  |                                                               |
     *  +---------------------------------------------------------------+
     *  |                      Cache Line Padding                      ...
     * ...                                                              |
     *  +---------------------------------------------------------------+
     *  |                 Registration / Correlation ID                 |
     *  |                                                               |
     *  +---------------------------------------------------------------+
     *  |                        Initial Term Id                        |
     *  +---------------------------------------------------------------+
     *  |                  Default Frame Header Length                  |
     *  +---------------------------------------------------------------+
     *  |                          MTU Length                           |
     *  +---------------------------------------------------------------+
     *  |                      Cache Line Padding                      ...
     * ...                                                              |
     *  +---------------------------------------------------------------+
     *  |                    Default Frame Header                      ...
     * ...                                                              |
     *  +---------------------------------------------------------------+
     * </pre>
     */
    public static final int LOG_META_DATA_LENGTH;

    /**
     * Check that term length is valid and alignment is valid.
     *
     * @param termLength to be checked.
     * @throws IllegalStateException if the length is not as expected.
     */
    public static void checkTermLength(final int termLength)
    {
        if (termLength < TERM_MIN_LENGTH)
        {
            final String s = String.format(
                "Term length less than min length of %d, length=%d",
                TERM_MIN_LENGTH, termLength);
            throw new IllegalStateException(s);
        }

        if ((termLength & (FRAME_ALIGNMENT - 1)) != 0)
        {
            final String s = String.format(
                "Term length not a multiple of %d, length=%d",
                FRAME_ALIGNMENT, termLength);
            throw new IllegalStateException(s);
        }
    }

    /**
     * Get the value of the initial Term id used for this log.
     *
     * @param logMetaDataBuffer containing the meta data.
     * @return the value of the initial Term id used for this log.
     */
    public static int initialTermId(final UnsafeBuffer logMetaDataBuffer)
    {
        return logMetaDataBuffer.getInt(LOG_INITIAL_TERM_ID_OFFSET);
    }

    /**
     * Set the initial term at which this log begins. Initial should be randomised so that stream does not get
     * reused accidentally.
     *
     * @param logMetaDataBuffer containing the meta data.
     * @param initialTermId     value to be set.
     */
    public static void initialTermId(final UnsafeBuffer logMetaDataBuffer, final int initialTermId)
    {
        logMetaDataBuffer.putInt(LOG_INITIAL_TERM_ID_OFFSET, initialTermId);
    }

    /**
     * Get the value of the MTU length used for this log.
     *
     * @param logMetaDataBuffer containing the meta data.
     * @return the value of the MTU length used for this log.
     */
    public static int mtuLength(final UnsafeBuffer logMetaDataBuffer)
    {
        return logMetaDataBuffer.getInt(LOG_MTU_LENGTH_OFFSET);
    }

    /**
     * Set the MTU length used for this log.
     *
     * @param logMetaDataBuffer containing the meta data.
     * @param mtuLength         value to be set.
     */
    public static void mtuLength(final UnsafeBuffer logMetaDataBuffer, final int mtuLength)
    {
        logMetaDataBuffer.putInt(LOG_MTU_LENGTH_OFFSET, mtuLength);
    }

    /**
     * Get the value of the correlation ID for this log relating to the command which created it.
     *
     * @param logMetaDataBuffer containing the meta data.
     * @return the value of the correlation ID used for this log.
     */
    public static long correlationId(final UnsafeBuffer logMetaDataBuffer)
    {
        return logMetaDataBuffer.getLong(LOG_CORRELATION_ID_OFFSET);
    }

    /**
     * Set the correlation ID used for this log relating to the command which created it.
     *
     * @param logMetaDataBuffer containing the meta data.
     * @param id                value to be set.
     */
    public static void correlationId(final UnsafeBuffer logMetaDataBuffer, final long id)
    {
        logMetaDataBuffer.putLong(LOG_CORRELATION_ID_OFFSET, id);
    }

    /**
     * Get the value of the time of last SM in {@link System#currentTimeMillis()}.
     *
     * @param logMetaDataBuffer containing the meta data.
     * @return the value of time of last SM
     */
    public static long timeOfLastStatusMessage(final UnsafeBuffer logMetaDataBuffer)
    {
        return logMetaDataBuffer.getLongVolatile(LOG_TIME_OF_LAST_SM_OFFSET);
    }

    /**
     * Set the value of the time of last SM used by the producer of this log.
     *
     * @param logMetaDataBuffer containing the meta data.
     * @param timeInMillis      value of the time of last SM in {@link System#currentTimeMillis()}
     */
    public static void timeOfLastStatusMessage(final UnsafeBuffer logMetaDataBuffer, final long timeInMillis)
    {
        logMetaDataBuffer.putLongOrdered(LOG_TIME_OF_LAST_SM_OFFSET, timeInMillis);
    }

    /**
     * Get the value of the active partition index used by the producer of this log. Consumers may have a different
     * active index if they are running behind. The read is done with volatile semantics.
     *
     * @param logMetaDataBuffer containing the meta data.
     * @return the value of the active partition index used by the producer of this log.
     */
    public static int activePartitionIndex(final UnsafeBuffer logMetaDataBuffer)
    {
        return logMetaDataBuffer.getIntVolatile(LOG_ACTIVE_PARTITION_INDEX_OFFSET);
    }

    /**
     * Set the value of the current active partition index for the producer using memory ordered semantics.
     *
     * @param logMetaDataBuffer    containing the meta data.
     * @param activePartitionIndex value of the active partition index used by the producer of this log.
     */
    public static void activePartitionIndex(final UnsafeBuffer logMetaDataBuffer, final int activePartitionIndex)
    {
        logMetaDataBuffer.putIntOrdered(LOG_ACTIVE_PARTITION_INDEX_OFFSET, activePartitionIndex);
    }

    /**
     * Rotate to the next partition in sequence for the term id.
     *
     * @param currentIndex partition index
     * @return the next partition index
     */
    public static int nextPartitionIndex(final int currentIndex)
    {
        return (currentIndex + 1) % PARTITION_COUNT;
    }

    /**
     * Determine the partition index to be used given the initial term and active term ids.
     *
     * @param initialTermId at which the log buffer usage began
     * @param activeTermId  that is in current usage
     * @return the index of which buffer should be used
     */
    public static int indexByTerm(final int initialTermId, final int activeTermId)
    {
        return (activeTermId - initialTermId) % PARTITION_COUNT;
    }

    /**
     * Determine the partition index based on number of terms that have passed.
     *
     * @param termCount for the number of terms that have passed.
     * @return the partition index for the term count.
     */
    public static int indexByTermCount(final long termCount)
    {
        return (int)(termCount % PARTITION_COUNT);
    }

    /**
     * Determine the partition index given a stream position.
     *
     * @param position            in the stream in bytes.
     * @param positionBitsToShift number of times to right shift the position for term count
     * @return the partition index for the position
     */
    public static int indexByPosition(final long position, final int positionBitsToShift)
    {
        return (int)((position >>> positionBitsToShift) % PARTITION_COUNT);
    }

    /**
     * Compute the current position in absolute number of bytes.
     *
     * @param activeTermId        active term id.
     * @param termOffset          in the term.
     * @param positionBitsToShift number of times to left shift the term count
     * @param initialTermId       the initial term id that this stream started on
     * @return the absolute position in bytes
     */
    public static long computePosition(
        final int activeTermId, final int termOffset, final int positionBitsToShift, final int initialTermId)
    {
        final long termCount = activeTermId - initialTermId; // copes with negative activeTermId on rollover

        return (termCount << positionBitsToShift) + termOffset;
    }

    /**
     * Compute the current position in absolute number of bytes for the beginning of a term.
     *
     * @param activeTermId        active term id.
     * @param positionBitsToShift number of times to left shift the term count
     * @param initialTermId       the initial term id that this stream started on
     * @return the absolute position in bytes
     */
    public static long computeTermBeginPosition(
        final int activeTermId, final int positionBitsToShift, final int initialTermId)
    {
        final long termCount = activeTermId - initialTermId; // copes with negative activeTermId on rollover

        return termCount << positionBitsToShift;
    }

    /**
     * Compute the term id from a position.
     *
     * @param position            to calculate from
     * @param positionBitsToShift number of times to right shift the position
     * @param initialTermId       the initial term id that this stream started on
     * @return the term id according to the position
     */
    public static int computeTermIdFromPosition(
        final long position, final int positionBitsToShift, final int initialTermId)
    {
        return ((int)(position >>> positionBitsToShift) + initialTermId);
    }

    /**
     * Compute the term offset from a given position.
     *
     * @param position            to calculate from
     * @param positionBitsToShift number of times to right shift the position
     * @return the offset within the term that represents the position
     */
    public static int computeTermOffsetFromPosition(final long position, final int positionBitsToShift)
    {
        final long mask = (1L << positionBitsToShift) - 1L;

        return (int)(position & mask);
    }

    /**
     * Compute the total length of a log file given the term length.
     *
     * @param termLength on which to base the calculation.
     * @return the total length of the log file.
     */
    public static long computeLogLength(final int termLength)
    {
        return (termLength * PARTITION_COUNT) + LOG_META_DATA_LENGTH;
    }

    /**
     * Compute the term length based on the total length of the log.
     *
     * @param logLength the total length of the log.
     * @return length of an individual term buffer in the log.
     */
    public static int computeTermLength(final long logLength)
    {
        return (int)((logLength - LOG_META_DATA_LENGTH) / PARTITION_COUNT);
    }

    /**
     * Store the default frame header to the log meta data buffer.
     *
     * @param logMetaDataBuffer into which the default headers should be stored.
     * @param defaultHeader     to be stored.
     * @throws IllegalArgumentException if the defaultHeader is larger than {@link #LOG_DEFAULT_FRAME_HEADER_MAX_LENGTH}
     */
    public static void storeDefaultFrameHeader(final UnsafeBuffer logMetaDataBuffer, final DirectBuffer defaultHeader)
    {
        if (defaultHeader.capacity() != HEADER_LENGTH)
        {
            throw new IllegalArgumentException(String.format(
                "Default header of %d not equal to %d", defaultHeader.capacity(), HEADER_LENGTH));
        }

        logMetaDataBuffer.putInt(LOG_DEFAULT_FRAME_HEADER_LENGTH_OFFSET, HEADER_LENGTH);
        logMetaDataBuffer.putBytes(LOG_DEFAULT_FRAME_HEADER_OFFSET, defaultHeader, 0, HEADER_LENGTH);
    }

    /**
     * Get a wrapper around the default frame header from the log meta data.
     *
     * @param logMetaDataBuffer containing the raw bytes for the default frame header.
     * @return a buffer wrapping the raw bytes.
     */
    public static UnsafeBuffer defaultFrameHeader(final UnsafeBuffer logMetaDataBuffer)
    {
        return new UnsafeBuffer(logMetaDataBuffer, LOG_DEFAULT_FRAME_HEADER_OFFSET, HEADER_LENGTH);
    }

    /**
     * Apply the default header for a message in a term.
     *
     * @param logMetaDataBuffer containing the default headers.
     * @param termBuffer        to which the default header should be applied.
     * @param termOffset        at which the default should be applied.
     */
    public static void applyDefaultHeader(
        final UnsafeBuffer logMetaDataBuffer, final UnsafeBuffer termBuffer, final int termOffset)
    {
        termBuffer.putBytes(termOffset, logMetaDataBuffer, LOG_DEFAULT_FRAME_HEADER_OFFSET, HEADER_LENGTH);
    }

    /**
     * Rotate the log and update the default headers for the new term.
     *
     * @param logMetaDataBuffer    for the meta data.
     * @param activePartitionIndex current active index.
     * @param termId               to be used in the default headers.
     */
    public static void rotateLog(final UnsafeBuffer logMetaDataBuffer, final int activePartitionIndex, final int termId)
    {
        final int nextIndex = nextPartitionIndex(activePartitionIndex);
        initialiseTailWithTermId(logMetaDataBuffer, nextIndex, termId);
        activePartitionIndex(logMetaDataBuffer, nextIndex);
    }

    /**
     * Set the initial value for the termId in the upper bits of the tail counter.
     *
     * @param logMetaData    contain the tail counter.
     * @param partitionIndex to be initialised.
     * @param termId         to be set.
     */
    public static void initialiseTailWithTermId(
        final UnsafeBuffer logMetaData, final int partitionIndex, final int termId)
    {
        logMetaData.putLong(TERM_TAIL_COUNTERS_OFFSET + (partitionIndex * SIZE_OF_LONG), ((long)termId) << 32);
    }

    /**
     * Get the termId from a packed raw tail value.
     *
     * @param rawTail containing the termId
     * @return the termId from a packed raw tail value.
     */
    public static int termId(final long rawTail)
    {
        return (int)(rawTail >>> 32);
    }

    /**
     * Read the termOffset from a packed raw tail value.
     *
     * @param rawTail    containing the termOffset.
     * @param termLength that the offset cannot exceed.
     * @return the termOffset value.
     */
    public static int termOffset(final long rawTail, final long termLength)
    {
        final long tail = rawTail & 0xFFFF_FFFFL;

        return (int)Math.min(tail, termLength);
    }

    /**
     * Get the raw value of the tail for the given partition.
     *
     * @param logMetaDataBuffer containing the tail counters.
     * @param partitionIndex    for the tail counter.
     * @return the raw value of the tail for the current active partition.
     */
    public static long rawTailVolatile(final UnsafeBuffer logMetaDataBuffer, final int partitionIndex)
    {
        return logMetaDataBuffer.getLongVolatile(TERM_TAIL_COUNTERS_OFFSET + (SIZE_OF_LONG * partitionIndex));
    }

    /**
     * Get the raw value of the tail for the current active partition.
     *
     * @param logMetaDataBuffer containing the tail counters.
     * @return the raw value of the tail for the current active partition.
     */
    public static long rawTailVolatile(final UnsafeBuffer logMetaDataBuffer)
    {
        final int partitionIndex = activePartitionIndex(logMetaDataBuffer);
        return logMetaDataBuffer.getLongVolatile(TERM_TAIL_COUNTERS_OFFSET + (SIZE_OF_LONG * partitionIndex));
    }
}
