/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2018 Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "rd.h"
#include "rdkafka_int.h"
#include "rdkafka_request.h"

/**
 * @name Idempotent Producer logic
 *
 *
 *
 */



static void rd_kafka_idemp_restart_request_pid_tmr (rd_kafka_t *rk);



/**
 * @returns true if we need a PID.
 *
 * @locks rd_kafka_*lock MUST be held
 */
static RD_UNUSED /*FIXME*/rd_bool_t rd_kafka_idemp_need_pid (rd_kafka_t *rk) {
        return rk->rk_eos.idemp_state == RD_KAFKA_IDEMP_STATE_REQ_PID ||
                rk->rk_eos.idemp_state == RD_KAFKA_IDEMP_STATE_WAIT_PID;
}




/**
 * @brief Set the producer's idempotence state.
 * @locks rd_kafka_wrlock() MUST be held
 */
static void
rd_kafka_idemp_set_state (rd_kafka_t *rk,
                          rd_kafka_idemp_state_t new_state) {

        rd_assert(thrd_is_current(rk->rk_thread));

        if (rk->rk_eos.idemp_state == new_state)
                return;

        rd_kafka_dbg(rk, EOS, "IDEMPSTATE",
                     "Idempotent producer state change %s -> %s",
                     rd_kafka_idemp_state2str(rk->rk_eos.
                                              idemp_state),
                     rd_kafka_idemp_state2str(new_state));

        rk->rk_eos.idemp_state = new_state;
        rk->rk_eos.ts_idemp_state = rd_clock();
}







/**
 * @brief Acquire Pid by looking up a suitable broker and then
 *        sending an InitProducerIdRequest to it.
 *
 * @param rkb may be set to specify a broker to use, otherwise a suitable
 *            one is looked up.
 *
 * @returns 1 if a request was enqueued, or 0 if no broker was available.
 *
 * @locality rdkafka main thread
 * @locks none
 */
int rd_kafka_idemp_request_pid (rd_kafka_t *rk, rd_kafka_broker_t *rkb,
                                const char *reason) {

        rd_kafka_resp_err_t err;
        char errstr[128];

        rd_assert(thrd_is_current(rk->rk_thread));

        rd_kafka_wrlock(rk);
        if (rk->rk_eos.idemp_state != RD_KAFKA_IDEMP_STATE_REQ_PID) {
                rd_kafka_wrunlock(rk);
                return 0;
        }

        if (!rkb) {
                rkb = rd_kafka_broker_any_usable(rk, 0, 0/*no-lock*/);
                if (!rkb) {
                        rd_kafka_wrunlock(rk);
                        rd_kafka_idemp_restart_request_pid_tmr(rk);
                        return 0;
                }
        } else {
                /* Increase passed broker's refcount so we don't
                 * have to check if rkb should be destroyed or not below
                 * (broker_any_usable() returns a new reference). */
                rd_kafka_broker_keep(rkb);
        }

        rd_rkb_dbg(rkb, EOS, "GETPID", "Acquiring ProducerId: %s", reason);

        err = rd_kafka_InitProducerIdRequest(
                rkb, NULL, -1,
                errstr, sizeof(errstr),
                RD_KAFKA_REPLYQ(rk->rk_ops, 0),
                rd_kafka_handle_InitProducerId, NULL);

        if (!err) {
                rd_kafka_idemp_set_state(rkb->rkb_rk,
                                         RD_KAFKA_IDEMP_STATE_WAIT_PID);
                rd_kafka_wrunlock(rkb->rkb_rk);
                rd_kafka_broker_destroy(rkb);
                return 1;
        }

        rd_kafka_wrunlock(rkb->rkb_rk);

        rd_rkb_dbg(rkb, EOS, "GETPID",
                   "Can't acquire ProducerId from this broker: %s", errstr);
        rd_kafka_idemp_restart_request_pid_tmr(rk);

        rd_kafka_broker_destroy(rkb);

        return 0;
}


/**
 * @brief Timed PID retrieval timer callback.
 */
static void rd_kafka_idemp_request_pid_tmr_cb (rd_kafka_timers_t *rkts,
                                               void *arg) {
        rd_kafka_t *rk = arg;

        rd_kafka_idemp_request_pid(rk, NULL, "retry timer");
}

/**
 * @brief Restart the pid retrieval timer.
 *
 * @locality rdkafka main thread
 * @locks none
 */
static void rd_kafka_idemp_restart_request_pid_tmr (rd_kafka_t *rk) {
        rd_kafka_timer_start_oneshot(&rk->rk_timers,
                                     &rk->rk_eos.request_pid_tmr,
                                     500, rd_kafka_idemp_request_pid_tmr_cb, rk);
}


/**
 * @brief Handle failure to acquire a PID from broker.
 *
 * @locality rdkafka main thread
 * @locks none
 */
void rd_kafka_idemp_request_pid_failed (rd_kafka_broker_t *rkb,
                                    rd_kafka_resp_err_t err) {
        rd_kafka_t *rk = rkb->rkb_rk;

        rd_rkb_dbg(rkb, EOS, "GETPID",
                   "Failed to acquire PID: %s", rd_kafka_err2str(err));

        if (err == RD_KAFKA_RESP_ERR__DESTROY)
                return; /* Ignore */

        rd_assert(thrd_is_current(rk->rk_thread));

        /* FIXME: Handle special errors, maybe raise certain errors
         *        to the application (such as UNSUPPORTED_FEATURE) */

        /* Retry request after a short wait. */
        rd_kafka_idemp_restart_request_pid_tmr(rk);
}


/**
 * @brief Update Producer ID from InitProducerId response.
 *
 * @remark If we've already have a PID the new one is ignored.
 *
 * @locality rdkafka main thread
 * @locks none
 */
void rd_kafka_idemp_pid_update (rd_kafka_broker_t *rkb,
                                const rd_kafka_pid_t pid) {
        rd_kafka_t *rk = rkb->rkb_rk;

        rd_assert(thrd_is_current(rk->rk_thread));

        rd_kafka_wrlock(rk);
        if (rk->rk_eos.idemp_state != RD_KAFKA_IDEMP_STATE_WAIT_PID) {
                rd_rkb_dbg(rkb, EOS, "GETPID",
                           "Ignoring InitProduceId response (%s) "
                           "in state %s",
                           rd_kafka_pid2str(pid),
                           rd_kafka_idemp_state2str(rk->rk_eos.idemp_state));
                rd_kafka_wrunlock(rk);
                return;
        }

        if (!rd_kafka_pid_valid(pid)) {
                rd_kafka_wrunlock(rk);
                rd_rkb_log(rkb, LOG_WARNING, "GETPID",
                           "Acquired invalid PID{%"PRId64",%hd}: ignoring",
                           pid.id, pid.epoch);
                rd_kafka_idemp_request_pid_failed(rkb,
                                                  RD_KAFKA_RESP_ERR__BAD_MSG);
                return;
        }

        if (rd_kafka_pid_valid(rk->rk_eos.pid))
                rd_kafka_dbg(rk, EOS, "GETPID",
                             "Acquired %s (previous %s)",
                             rd_kafka_pid2str(pid),
                             rd_kafka_pid2str(rk->rk_eos.pid));
        else
                rd_kafka_dbg(rk, EOS, "GETPID",
                             "Acquired %s", rd_kafka_pid2str(pid));
        rk->rk_eos.pid = pid;

        rd_kafka_idemp_set_state(rk, RD_KAFKA_IDEMP_STATE_ASSIGNED);

        rd_kafka_wrunlock(rk);

        /* Wake up all broker threads that may have messages to send
         * that were waiting for a Producer ID. */
        rd_kafka_all_brokers_wakeup(rk, RD_KAFKA_BROKER_STATE_UP);
}



/**
 * @brief Initialize the idempotent producer.
 *
 * @remark Must be called from rd_kafka_new() and only once.
 * @locality application thread
 * @locks none / not needed from rd_kafka_new()
 */
void rd_kafka_idemp_init (rd_kafka_t *rk) {
        rd_assert(thrd_is_current(rk->rk_thread));

        rd_kafka_wrlock(rk);
        rd_kafka_pid_reset(&rk->rk_eos.pid);

        /* There are no available brokers this early, so just set
         * the state to indicate that we want to acquire a PID as soon
         * as possible and start the timer. */
        rd_kafka_idemp_set_state(rk, RD_KAFKA_IDEMP_STATE_REQ_PID);
        rd_kafka_wrunlock(rk);

        rd_kafka_idemp_restart_request_pid_tmr(rk);
}


/**
 * @brief Terminate and clean up idempotent producer
 *
 * @locality rdkafka main thread
 * @locks rd_kafka_wrlock() MUST be held
 */
void rd_kafka_idemp_term (rd_kafka_t *rk) {
        rd_assert(thrd_is_current(rk->rk_thread));

        rd_kafka_wrlock(rk);
        rd_kafka_idemp_set_state(rk, RD_KAFKA_IDEMP_STATE_TERM);
        rd_kafka_wrunlock(rk);
        rd_kafka_timer_stop(&rk->rk_timers, &rk->rk_eos.request_pid_tmr, 1);
}


