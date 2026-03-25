// Include required headers
#include <stdlib.h>

#include <doca_error.h>
#include <doca_log.h>

// Register logger backend
DOCA_LOG_REGISTER(LAB5::MAIN);

// Declare the programs logic function
doca_error_t run_lab_logic(void);

// Define the main function
int main(int argc, char *argv[]) {
    (void)(argc);
    (void)(argv);

    doca_error_t result;
    struct doca_log_backend *sdk_log;
    int exit_status = EXIT_FAILURE;

    result = doca_log_backend_create_standard();
    if (result != DOCA_SUCCESS)
        return EXIT_FAILURE;

    result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
    if (result != DOCA_SUCCESS)
        goto lab_exit;
    result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
    if (result != DOCA_SUCCESS)
        goto lab_exit;

    DOCA_LOG_INFO("Starting the program LAB 5");

    result = run_lab_logic();
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("run_lab_logic() encountered an error: %s", 
            doca_error_get_descr(result));
        goto lab_exit;
    }

    exit_status = EXIT_SUCCESS;

    lab_exit:
        if (exit_status == EXIT_SUCCESS)
            DOCA_LOG_INFO("LAB 5 finished successfully");
        else
            DOCA_LOG_INFO("LAB 5 finished with errors");
        return exit_status;
}

