/*
 * Copyright 2014-2020 Real Logic Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cinttypes>
#include <array>
#include <cstdint>
#include <thread>
#include <exception>
#include <functional>

#include <gtest/gtest.h>

extern "C"
{
#include "aeronc.h"
#include "aeron_context.h"
#include "aeron_cnc_file_descriptor.h"
#include "concurrent/aeron_mpsc_rb.h"
#include "concurrent/aeron_broadcast_transmitter.h"
#include "concurrent/aeron_counters_manager.h"
#include "aeron_client_conductor.h"
#include "util/aeron_fileutil.h"
}

#define CAPACITY (1024)
#define TO_DRIVER_RING_BUFFER_LENGTH (CAPACITY + AERON_RB_TRAILER_LENGTH)
#define TO_CLIENTS_BUFFER_LENGTH (CAPACITY + AERON_BROADCAST_BUFFER_TRAILER_LENGTH)
#define COUNTER_VALUES_BUFFER_LENGTH (1024 * 1024)
#define COUNTER_METADATA_BUFFER_LENGTH (AERON_COUNTERS_METADATA_BUFFER_LENGTH(COUNTER_VALUES_BUFFER_LENGTH))
#define ERROR_BUFFER_LENGTH (CAPACITY)
#define FILE_PAGE_SIZE (4 * 1024)

#define CLIENT_LIVENESS_TIMEOUT (5 * 1000 * 1000 * 1000LL)

#define TIME_ADVANCE_INTERVAL_NS (1000 * 1000LL)

#define PUB_URI "aeron:udp?endpoint=localhost:24567"
#define SUB_URI "aeron:udp?endpoint=localhost:24567"
#define STREAM_ID (101)
#define SESSION_ID (110)
#define COUNTER_TYPE_ID (102)

static int64_t now_ms = 0;
static int64_t now_ns = 0;

static int64_t test_epoch_clock()
{
    return now_ms;
}

static int64_t test_nano_clock()
{
    return now_ns;
}

class ClientConductorTest : public testing::Test
{
public:

    ClientConductorTest() :
        m_logFileName(tempFileName())
    {
        now_ns = 0;
        now_ms = 0;

        if (aeron_context_init(&m_context) < 0)
        {
            throw std::runtime_error("could not init context: " + std::string(aeron_errmsg()));
        }

        m_context->cnc_map.length = aeron_cnc_computed_length(
            TO_DRIVER_RING_BUFFER_LENGTH +
            TO_CLIENTS_BUFFER_LENGTH +
            COUNTER_VALUES_BUFFER_LENGTH +
            COUNTER_METADATA_BUFFER_LENGTH +
            ERROR_BUFFER_LENGTH,
            FILE_PAGE_SIZE);
        m_cnc = std::unique_ptr<uint8_t[]>(new uint8_t[m_context->cnc_map.length]);
        m_context->cnc_map.addr = m_cnc.get();
        memset(m_context->cnc_map.addr, 0, m_context->cnc_map.length);

        m_context->epoch_clock = test_epoch_clock;
        m_context->nano_clock = test_nano_clock;
        m_context->use_conductor_agent_invoker = true;

        aeron_cnc_metadata_t *metadata = (aeron_cnc_metadata_t *)m_context->cnc_map.addr;
        metadata->to_driver_buffer_length = (int32_t)TO_DRIVER_RING_BUFFER_LENGTH;
        metadata->to_clients_buffer_length = (int32_t)TO_CLIENTS_BUFFER_LENGTH;
        metadata->counter_metadata_buffer_length = (int32_t)COUNTER_METADATA_BUFFER_LENGTH;
        metadata->counter_values_buffer_length = (int32_t)COUNTER_VALUES_BUFFER_LENGTH;
        metadata->error_log_buffer_length = (int32_t)ERROR_BUFFER_LENGTH;
        metadata->client_liveness_timeout = (int64_t)CLIENT_LIVENESS_TIMEOUT;
        metadata->start_timestamp = test_epoch_clock();
        metadata->pid = 101;
        AERON_PUT_VOLATILE(metadata->cnc_version, AERON_CNC_VERSION);

        if (aeron_mpsc_rb_init(
            &m_to_driver, aeron_cnc_to_driver_buffer(metadata), TO_DRIVER_RING_BUFFER_LENGTH) < 0)
        {
            throw std::runtime_error("could not init to_driver: " + std::string(aeron_errmsg()));
        }

        if (aeron_broadcast_transmitter_init(
            &m_to_clients, aeron_cnc_to_clients_buffer(metadata), TO_CLIENTS_BUFFER_LENGTH) < 0)
        {
            throw std::runtime_error("could not init to_clients: " + std::string(aeron_errmsg()));
        }

        if (aeron_client_conductor_init(&m_conductor, m_context) < 0)
        {
            throw std::runtime_error("could not init conductor: " + std::string(aeron_errmsg()));
        }
    }

    virtual ~ClientConductorTest()
    {
        aeron_client_conductor_on_close(&m_conductor);
        m_context->cnc_map.addr = NULL;
        aeron_context_close(m_context);

        ::unlink(m_logFileName.c_str());
    }

    static std::string tempFileName()
    {
        char filename[AERON_MAX_PATH];

        aeron_temp_filename(filename, sizeof(filename));
        return std::string(filename);
    }

    static void createLogFile(std::string &filename)
    {
        aeron_mapped_file_t mappedFile = {
            NULL,
            AERON_LOGBUFFER_TERM_MIN_LENGTH * 3 + AERON_LOGBUFFER_META_DATA_LENGTH };

        if (aeron_map_new_file(&mappedFile, filename.c_str(), false) < 0)
        {
            throw std::runtime_error("could not create log file: " + std::string(aeron_errmsg()));
        }

        auto metadata = reinterpret_cast<aeron_logbuffer_metadata_t *>((uint8_t *)mappedFile.addr +
            (mappedFile.length - AERON_LOGBUFFER_META_DATA_LENGTH));

        metadata->term_length = AERON_LOGBUFFER_TERM_MIN_LENGTH;
        metadata->page_size = FILE_PAGE_SIZE;

        aeron_unmap(&mappedFile);
    }

    static void ToDriverHandler(int32_t type_id, const void *buffer, size_t length, void *clientd)
    {
        auto conductorTest = reinterpret_cast<ClientConductorTest *>(clientd);

        conductorTest->m_to_driver_handler(type_id, buffer, length);
    }

    size_t readToDriver(std::function<void(int32_t, const void *, size_t)>& handler)
    {
        m_to_driver_handler = handler;
        return aeron_mpsc_rb_read(&m_to_driver, ToDriverHandler, this, 1);
    }

    int doWork(bool updateDriverHeartbeat = true)
    {
        int work_count;

        if (updateDriverHeartbeat)
        {
            aeron_mpsc_rb_consumer_heartbeat_time(&m_to_driver, test_epoch_clock());
        }

        if ((work_count = aeron_client_conductor_do_work(&m_conductor)) < 0)
        {
            throw std::runtime_error("error from do_work: " + std::string(aeron_errmsg()));
        }

        return work_count;
    }

    int doWorkForNs(
        int64_t interval_ns, bool updateDriverHeatbeat= true, int64_t advance_interval_ns = TIME_ADVANCE_INTERVAL_NS)
    {
        int work_count = 0;
        int64_t target_ns = now_ns + interval_ns;

        do
        {
            now_ns += advance_interval_ns;
            now_ms = now_ns / 1000000LL;
            work_count += doWork(updateDriverHeatbeat);
        }
        while (now_ns < target_ns);

        return work_count;
    }

    void transmitOnPublicationReady(aeron_async_add_publication_t *async, const std::string &logFile, bool isExclusive)
    {
        char response_buffer[sizeof(aeron_publication_buffers_ready_t) + AERON_MAX_PATH];
        auto response = reinterpret_cast<aeron_publication_buffers_ready_t *>(response_buffer);
        int32_t position_limit_counter_id = 10, channel_status_indicator_id = 11;

        response->correlation_id = async->registration_id;
        response->registration_id = async->registration_id;
        response->stream_id = async->stream_id;
        response->session_id = SESSION_ID;
        response->position_limit_counter_id = position_limit_counter_id;
        response->channel_status_indicator_id = channel_status_indicator_id;
        response->log_file_length = static_cast<int32_t>(logFile.length());
        memcpy(response_buffer + sizeof(aeron_publication_buffers_ready_t), logFile.c_str(), logFile.length());

        if (aeron_broadcast_transmitter_transmit(
            &m_to_clients,
            isExclusive ? AERON_RESPONSE_ON_EXCLUSIVE_PUBLICATION_READY : AERON_RESPONSE_ON_PUBLICATION_READY,
            response_buffer,
            sizeof(aeron_publication_buffers_ready_t) + logFile.length()) < 0)
        {
            throw std::runtime_error("error transmitting ON_PUBLICATION_READY: " + std::string(aeron_errmsg()));
        }
    }

    void transmitOnError(aeron_async_add_publication_t *async, int32_t errorCode, const std::string &errorMessage)
    {
        char response_buffer[sizeof(aeron_error_response_t) + AERON_MAX_PATH];
        auto response = reinterpret_cast<aeron_error_response_t *>(response_buffer);

        response->offending_command_correlation_id = async->registration_id;
        response->error_code = errorCode;
        response->error_message_length = static_cast<int32_t>(errorMessage.length());
        memcpy(response_buffer + sizeof(aeron_error_response_t), errorMessage.c_str(), errorMessage.length());

        if (aeron_broadcast_transmitter_transmit(
            &m_to_clients,
            AERON_RESPONSE_ON_ERROR,
            response_buffer,
            sizeof(aeron_error_response_t) + errorMessage.length()) < 0)
        {
            throw std::runtime_error("error transmitting ON_ERROR: " + std::string(aeron_errmsg()));
        }
    }

    void transmitOnSubscriptionReady(aeron_async_add_subscription_t *async)
    {
        char response_buffer[sizeof(aeron_subscription_ready_t)];
        auto response = reinterpret_cast<aeron_subscription_ready_t *>(response_buffer);
        int32_t channel_status_indicator_id = 11;

        response->correlation_id = async->registration_id;
        response->channel_status_indicator_id = channel_status_indicator_id;

        if (aeron_broadcast_transmitter_transmit(
            &m_to_clients,
            AERON_RESPONSE_ON_SUBSCRIPTION_READY,
            response_buffer,
            sizeof(aeron_subscription_ready_t)) < 0)
        {
            throw std::runtime_error("error transmitting ON_SUBSCRIPTION_READY: " + std::string(aeron_errmsg()));
        }
    }

    void transmitOnCounterReady(aeron_async_add_counter_t *async)
    {
        char response_buffer[sizeof(aeron_counter_update_t)];
        auto response = reinterpret_cast<aeron_counter_update_t *>(response_buffer);
        int32_t counter_id = 11;

        response->correlation_id = async->registration_id;
        response->counter_id = counter_id;

        if (aeron_broadcast_transmitter_transmit(
            &m_to_clients,
            AERON_RESPONSE_ON_COUNTER_READY,
            response_buffer,
            sizeof(aeron_counter_update_t)) < 0)
        {
            throw std::runtime_error("error transmitting ON_COUNTER_READY: " + std::string(aeron_errmsg()));
        }
    }

protected:
    aeron_context_t *m_context = NULL;
    aeron_client_conductor_t m_conductor;
    std::unique_ptr<uint8_t[]> m_cnc;
    aeron_mpsc_rb_t m_to_driver;
    aeron_broadcast_transmitter_t m_to_clients;
    std::string m_logFileName;

    std::function<void(int32_t, const void *, size_t)> m_to_driver_handler;
};

TEST_F(ClientConductorTest, shouldInitAndClose)
{
    // nothing to do
}

TEST_F(ClientConductorTest, shouldAddPublicationSuccessfully)
{
    aeron_async_add_publication_t *async = NULL;
    aeron_publication_t *publication = NULL;

    ASSERT_EQ(aeron_client_conductor_async_add_publication(&async, &m_conductor, PUB_URI, STREAM_ID), 0);
    doWork();

    // poll unsuccessfully
    ASSERT_EQ(aeron_async_add_publication_poll(&publication, async), 0) << aeron_errmsg();
    ASSERT_TRUE(NULL == publication);

    // send buffers ready
    transmitOnPublicationReady(async, m_logFileName, false);
    createLogFile(m_logFileName);
    doWork();

    // poll successfully
    ASSERT_GT(aeron_async_add_publication_poll(&publication, async), 0) << aeron_errmsg();
    ASSERT_TRUE(NULL != publication);

    // close
    ASSERT_EQ(aeron_publication_close(publication), 0);
    doWork();
}

TEST_F(ClientConductorTest, shouldErrorOnAddPublicationFromDriverError)
{
    aeron_async_add_publication_t *async = NULL;
    aeron_publication_t *publication = NULL;

    ASSERT_EQ(aeron_client_conductor_async_add_publication(&async, &m_conductor, PUB_URI, STREAM_ID), 0);
    doWork();

    // poll unsuccessfully
    ASSERT_EQ(aeron_async_add_publication_poll(&publication, async), 0) << aeron_errmsg();
    ASSERT_TRUE(NULL == publication);

    // error from driver.
    transmitOnError(async, AERON_ERROR_CODE_INVALID_CHANNEL, "invalid channel");
    doWork();

    ASSERT_EQ(aeron_async_add_publication_poll(&publication, async), -1);
}

TEST_F(ClientConductorTest, shouldErrorOnAddPublicationFromDriverTimeout)
{
    aeron_async_add_publication_t *async = NULL;
    aeron_publication_t *publication = NULL;

    ASSERT_EQ(aeron_client_conductor_async_add_publication(&async, &m_conductor, PUB_URI, STREAM_ID), 0);
    doWork();

    // poll unsuccessfully
    ASSERT_EQ(aeron_async_add_publication_poll(&publication, async), 0) << aeron_errmsg();
    ASSERT_TRUE(NULL == publication);

    doWorkForNs((m_context->driver_timeout_ms + 1000) * 1000000LL);

    ASSERT_EQ(aeron_async_add_publication_poll(&publication, async), -1);
}

TEST_F(ClientConductorTest, shouldAddExclusivePublicationSuccessfully)
{
    aeron_async_add_exclusive_publication_t *async = NULL;
    aeron_exclusive_publication_t *publication = NULL;

    ASSERT_EQ(aeron_client_conductor_async_add_exclusive_publication(&async, &m_conductor, PUB_URI, STREAM_ID), 0);
    doWork();

    // poll unsuccessfully
    ASSERT_EQ(aeron_async_add_exclusive_publication_poll(&publication, async), 0) << aeron_errmsg();
    ASSERT_TRUE(NULL == publication);

    // send buffers ready
    transmitOnPublicationReady(async, m_logFileName, true);
    createLogFile(m_logFileName);
    doWork();

    // poll successfully
    ASSERT_GT(aeron_async_add_exclusive_publication_poll(&publication, async), 0) << aeron_errmsg();
    ASSERT_TRUE(NULL != publication);

    // close
    ASSERT_EQ(aeron_exclusive_publication_close(publication), 0);
    doWork();
}

TEST_F(ClientConductorTest, shouldErrorOnAddExclusivePublicationFromDriverError)
{
    aeron_async_add_exclusive_publication_t *async = NULL;
    aeron_exclusive_publication_t *publication = NULL;

    ASSERT_EQ(aeron_client_conductor_async_add_exclusive_publication(&async, &m_conductor, PUB_URI, STREAM_ID), 0);
    doWork();

    // poll unsuccessfully
    ASSERT_EQ(aeron_async_add_exclusive_publication_poll(&publication, async), 0) << aeron_errmsg();
    ASSERT_TRUE(NULL == publication);

    // error from driver.
    transmitOnError(async, AERON_ERROR_CODE_INVALID_CHANNEL, "invalid channel");
    doWork();

    ASSERT_EQ(aeron_async_add_exclusive_publication_poll(&publication, async), -1);
}

TEST_F(ClientConductorTest, shouldErrorOnAddExclusivePublicationFromDriverTimeout)
{
    aeron_async_add_exclusive_publication_t *async = NULL;
    aeron_exclusive_publication_t *publication = NULL;

    ASSERT_EQ(aeron_client_conductor_async_add_exclusive_publication(&async, &m_conductor, PUB_URI, STREAM_ID), 0);
    doWork();

    // poll unsuccessfully
    ASSERT_EQ(aeron_async_add_exclusive_publication_poll(&publication, async), 0) << aeron_errmsg();
    ASSERT_TRUE(NULL == publication);

    doWorkForNs((m_context->driver_timeout_ms + 1000) * 1000000LL);

    ASSERT_EQ(aeron_async_add_exclusive_publication_poll(&publication, async), -1);
}

TEST_F(ClientConductorTest, shouldAddSubscriptionSuccessfully)
{
    aeron_async_add_subscription_t *async = NULL;
    aeron_subscription_t *subscription = NULL;

    ASSERT_EQ(aeron_client_conductor_async_add_subscription(
        &async, &m_conductor, SUB_URI, STREAM_ID, NULL, NULL, NULL, NULL), 0);
    doWork();

    // poll unsuccessfully
    ASSERT_EQ(aeron_async_add_subscription_poll(&subscription, async), 0) << aeron_errmsg();
    ASSERT_TRUE(NULL == subscription);

    // send subscription ready
    transmitOnSubscriptionReady(async);
    doWork();

    // poll successfully
    ASSERT_GT(aeron_async_add_subscription_poll(&subscription, async), 0) << aeron_errmsg();
    ASSERT_TRUE(NULL != subscription);

    // close
    ASSERT_EQ(aeron_subscription_close(subscription), 0);
    doWork();
}

TEST_F(ClientConductorTest, shouldErrorOnAddSubscriptionFromDriverError)
{
    aeron_async_add_subscription_t *async = NULL;
    aeron_subscription_t *subscription = NULL;

    ASSERT_EQ(aeron_client_conductor_async_add_subscription(
        &async, &m_conductor, SUB_URI, STREAM_ID, NULL, NULL, NULL, NULL), 0);
    doWork();

    // poll unsuccessfully
    ASSERT_EQ(aeron_async_add_subscription_poll(&subscription, async), 0) << aeron_errmsg();
    ASSERT_TRUE(NULL == subscription);

    // error from driver.
    transmitOnError(async, AERON_ERROR_CODE_INVALID_CHANNEL, "invalid channel");
    doWork();

    ASSERT_EQ(aeron_async_add_subscription_poll(&subscription, async), -1);
}

TEST_F(ClientConductorTest, shouldErrorOnAddSubscriptionFromDriverTimeout)
{
    aeron_async_add_subscription_t *async = NULL;
    aeron_subscription_t *subscription = NULL;

    ASSERT_EQ(aeron_client_conductor_async_add_subscription(
        &async, &m_conductor, SUB_URI, STREAM_ID, NULL, NULL, NULL, NULL), 0);
    doWork();

    // poll unsuccessfully
    ASSERT_EQ(aeron_async_add_subscription_poll(&subscription, async), 0) << aeron_errmsg();
    ASSERT_TRUE(NULL == subscription);

    doWorkForNs((m_context->driver_timeout_ms + 1000) * 1000000LL);

    ASSERT_EQ(aeron_async_add_subscription_poll(&subscription, async), -1);
}

TEST_F(ClientConductorTest, shouldAddCounterSuccessfully)
{
    aeron_async_add_counter_t *async = NULL;
    aeron_counter_t *counter = NULL;

    ASSERT_EQ(aeron_client_conductor_async_add_counter(
        &async, &m_conductor, COUNTER_TYPE_ID, NULL, 0, NULL, 0), 0);
    doWork();

    // poll unsuccessfully
    ASSERT_EQ(aeron_async_add_counter_poll(&counter, async), 0) << aeron_errmsg();
    ASSERT_TRUE(NULL == counter);

    // send counter ready
    transmitOnCounterReady(async);
    doWork();

    // poll successfully
    ASSERT_GT(aeron_async_add_counter_poll(&counter, async), 0) << aeron_errmsg();
    ASSERT_TRUE(NULL != counter);

    // close
    ASSERT_EQ(aeron_counter_close(counter), 0);
    doWork();
}

TEST_F(ClientConductorTest, shouldErrorOnAddCounterFromDriverError)
{
    aeron_async_add_counter_t *async = NULL;
    aeron_counter_t *counter = NULL;

    ASSERT_EQ(aeron_client_conductor_async_add_counter(
        &async, &m_conductor, COUNTER_TYPE_ID, NULL, 0, NULL, 0), 0);
    doWork();

    // poll unsuccessfully
    ASSERT_EQ(aeron_async_add_counter_poll(&counter, async), 0) << aeron_errmsg();
    ASSERT_TRUE(NULL == counter);

    // error from driver.
    transmitOnError(async, AERON_ERROR_CODE_GENERIC_ERROR, "can not add counter");
    doWork();

    ASSERT_EQ(aeron_async_add_counter_poll(&counter, async), -1);
}

TEST_F(ClientConductorTest, shouldErrorOnAddCounterFromDriverTimeout)
{
    aeron_async_add_counter_t *async = NULL;
    aeron_counter_t *counter = NULL;

    ASSERT_EQ(aeron_client_conductor_async_add_counter(
        &async, &m_conductor, COUNTER_TYPE_ID, NULL, 0, NULL, 0), 0);
    doWork();

    // poll unsuccessfully
    ASSERT_EQ(aeron_async_add_counter_poll(&counter, async), 0) << aeron_errmsg();
    ASSERT_TRUE(NULL == counter);

    doWorkForNs((m_context->driver_timeout_ms + 1000) * 1000000LL);

    ASSERT_EQ(aeron_async_add_counter_poll(&counter, async), -1);
}