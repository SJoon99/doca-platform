/*
 * Copyright (c) 2022-2025 NVIDIA
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <doca_argp.h>
#include <doca_flow.h>
#include <doca_log.h>
#include <signal.h>
#include <dpdk_utils.h>

DOCA_LOG_REGISTER(FLOW_RSS_META::MAIN);

// DPDK 헤더
doca_error_t doca(int nb_queues);

// 강제 종료 플래그
volatile bool force_quit = false;

// 시그널 핸들러
static void signal_handler(int signum)
{
	if(signum == SIGINT || signum == SIGTERM){
		DOCA_LOG_INFO("Received signal %d, ....",signum);
		force_quit = true;
	}
}

int main(int argc, char **argv)
{
        doca_error_t result;
        struct doca_log_backend *sdk_log;
        int exit_status = EXIT_FAILURE;

        /* ---------- 호스트, 포트 1개 설정 ---------- */
        struct application_dpdk_config dpdk_config = {
                .port_config.nb_ports          = 1,  /* 한 개의 PF/VF/SF */
                .port_config.nb_queues         = 1,
                .port_config.nb_hairpin_q      = 0,
                .port_config.enable_mbuf_metadata = 1,
        };

        /* 로그 백엔드 */
        result = doca_log_backend_create_standard();
        if (result != DOCA_SUCCESS)
                goto sample_exit;

        result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
        if (result != DOCA_SUCCESS)
                goto sample_exit;
        result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
        if (result != DOCA_SUCCESS)
                goto sample_exit;

        DOCA_LOG_INFO("Starting the RSS-Meta single-port sample");

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

        /* ------------------------------------------------------------------
         * ARGP 초기화
         * ------------------------------------------------------------------ */
        result = doca_argp_init("flow_rss_meta", NULL);  // 단순 appname 
        if (result != DOCA_SUCCESS) {
                DOCA_LOG_ERR("Failed to init ARGP resources: %s",
                             doca_error_get_descr(result));
                goto sample_exit;
        }

        /* DPDK 코어/메모리 옵션을 ARGP에 등록 */
        doca_argp_set_dpdk_program(dpdk_init);

        /* CLI 파싱(EAL 포함) */
        result = doca_argp_start(argc, argv);
        if (result != DOCA_SUCCESS) {
                DOCA_LOG_ERR("Failed to parse sample input: %s",
                             doca_error_get_descr(result));
                goto argp_cleanup;
        }

        /* DPDK 포트·큐 오픈 */
        result = dpdk_queues_and_ports_init(&dpdk_config);
        if (result != DOCA_SUCCESS) {
                DOCA_LOG_ERR("Failed to update ports and queues");
                goto dpdk_cleanup;
        }

        /* 로직 실행 */
        result = doca(dpdk_config.port_config.nb_queues);
        if (result != DOCA_SUCCESS) {
                DOCA_LOG_ERR("flow_rss_meta() error: %s",
                             doca_error_get_descr(result));
                goto dpdk_ports_queues_cleanup;
        }

        exit_status = EXIT_SUCCESS;

/* -------------------- 정리 루틴 -------------------- */
dpdk_ports_queues_cleanup:
        dpdk_queues_and_ports_fini(&dpdk_config);
dpdk_cleanup:
        dpdk_fini();
argp_cleanup:
        doca_argp_destroy();              
sample_exit:
        if (exit_status == EXIT_SUCCESS)
                DOCA_LOG_INFO("Sample finished successfully");
        else
                DOCA_LOG_INFO("Sample finished with errors");

        return exit_status;
}

