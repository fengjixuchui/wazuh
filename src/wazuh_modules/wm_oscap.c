/*
 * Wazuh Module for OpenSCAP
 * Copyright (C) 2016 Wazuh Inc.
 * April 25, 2016.
 *
 * This program is a free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

#include "wmodules.h"

static wm_oscap *oscap;                             // Pointer to configuration
static int queue_fd;                                // Output queue file descriptor

static void* wm_oscap_main(wm_oscap *oscap);        // Module main function. It won't return
static void wm_oscap_setup(wm_oscap *_oscap);       // Setup module
static void wm_oscap_cleanup();                     // Cleanup function, doesn't overwrite wm_cleanup
static void wm_oscap_check();                       // Check configuration, disable flag
static void wm_oscap_run(wm_oscap_eval *eval);      // Run an OpenSCAP policy
static void wm_oscap_info();                        // Show module info
static void wm_oscap_destroy(wm_oscap *oscap);      // Destroy data

const char *WM_OSCAP_LOCATION = "wodle_open-scap";  // Location field for event sending

// OpenSCAP module context definition

const wm_context WM_OSCAP_CONTEXT = {
    "open-scap",
    (wm_routine)wm_oscap_main,
    (wm_routine)wm_oscap_destroy
};

// OpenSCAP module main function. It won't return.

void* wm_oscap_main(wm_oscap *oscap) {
    wm_oscap_eval *eval;
    time_t time_start = 0;
    time_t time_sleep = 0;

    // Check configuration and show debug information

    wm_oscap_setup(oscap);
    mtinfo(WM_OSCAP_LOGTAG, "Module started.");

    // First sleeping

    if (!oscap->flags.scan_on_start) {
        time_start = time(NULL);

        if (oscap->state.next_time > time_start) {
            mtinfo(WM_OSCAP_LOGTAG, "Waiting for turn to evaluate.");
            sleep(oscap->state.next_time - time_start);
        }
    }

    // Main loop

    while (1) {

        mtinfo(WM_OSCAP_LOGTAG, "Starting evaluation.");

        // Get time and execute
        time_start = time(NULL);

        for (eval = oscap->evals; eval; eval = eval->next)
            if (!eval->flags.error)
                wm_oscap_run(eval);

        time_sleep = time(NULL) - time_start;

        mtinfo(WM_OSCAP_LOGTAG, "Evaluation finished.");

        if ((time_t)oscap->interval >= time_sleep) {
            time_sleep = oscap->interval - time_sleep;
            oscap->state.next_time = oscap->interval + time_start;
        } else {
            mterror(WM_OSCAP_LOGTAG, "Interval overtaken.");
            time_sleep = oscap->state.next_time = 0;
        }

        if (wm_state_io(&WM_OSCAP_CONTEXT, WM_IO_WRITE, &oscap->state, sizeof(oscap->state)) < 0)
            mterror(WM_OSCAP_LOGTAG, "Couldn't save running state.");

        // If time_sleep=0, yield CPU
        sleep(time_sleep);
    }

    return NULL;
}

// Setup module

void wm_oscap_setup(wm_oscap *_oscap) {
    int i;

    oscap = _oscap;
    wm_oscap_check();

    // Read running state

    if (wm_state_io(&WM_OSCAP_CONTEXT, WM_IO_READ, &oscap->state, sizeof(oscap->state)) < 0)
        memset(&oscap->state, 0, sizeof(oscap->state));

    if (isDebug())
        wm_oscap_info();

    // Connect to socket

    for (i = 0; (queue_fd = StartMQ(DEFAULTQPATH, WRITE)) < 0 && i < WM_MAX_ATTEMPTS; i++)
        sleep(WM_MAX_WAIT);

    if (i == WM_MAX_ATTEMPTS) {
        mterror(WM_OSCAP_LOGTAG, "Can't connect to queue.");
        pthread_exit(NULL);
    }

    // Cleanup exiting

    atexit(wm_oscap_cleanup);
}

// Cleanup function, doesn't overwrite wm_cleanup

void wm_oscap_cleanup() {
    close(queue_fd);
    mtinfo(WM_OSCAP_LOGTAG, "Module finished.");
}

// Run an OpenSCAP policy

void wm_oscap_run(wm_oscap_eval *eval) {
    char *command = NULL;
    int status;
    char *output = NULL;
    char *line;
    char *arg_profiles = NULL;
    char msg[OS_MAXSTR];
    wm_oscap_profile *profile;

    // Define time to sleep between messages sent

    int usec = 1000000 / wm_max_eps;
    struct timeval timeout = {0, usec};

    // Create arguments

    wm_strcat(&command, WM_OSCAP_SCRIPT_PATH, '\0');

    switch (eval->type) {
    case WM_OSCAP_XCCDF:
        wm_strcat(&command, "--xccdf", ' ');
        break;
    case WM_OSCAP_OVAL:
        wm_strcat(&command, "--oval", ' ');
        break;
    default:
        mterror(WM_OSCAP_LOGTAG, "Unspecified content type for file '%s'. This shouldn't happen.", eval->path);
        pthread_exit(NULL);
    }

    wm_strcat(&command, eval->path, ' ');

    for (profile = eval->profiles; profile; profile = profile->next)
        wm_strcat(&arg_profiles, profile->name, ',');

    if (arg_profiles) {
        wm_strcat(&command, "--profiles", ' ');
        wm_strcat(&command, arg_profiles, ' ');
    }

    if (eval->xccdf_id) {
        wm_strcat(&command, "--xccdf-id", ' ');
        wm_strcat(&command, eval->xccdf_id, ' ');
    }

    if (eval->oval_id) {
        wm_strcat(&command, "--oval-id", ' ');
        wm_strcat(&command, eval->oval_id, ' ');
    }

    if (eval->ds_id) {
        wm_strcat(&command, "--ds-id", ' ');
        wm_strcat(&command, eval->ds_id, ' ');
    }

    if (eval->cpe) {
        wm_strcat(&command, "--cpe", ' ');
        wm_strcat(&command, eval->cpe, ' ');
    }

    // Send rootcheck message

    snprintf(msg, OS_MAXSTR, "Starting OpenSCAP scan. File: %s. ", eval->path);
    SendMSG(queue_fd, msg, "rootcheck", ROOTCHECK_MQ);

    // Execute

    mtdebug1(WM_OSCAP_LOGTAG, "Launching command: %s", command);

    switch (wm_exec(command, &output, &status, eval->timeout)) {
    case 0:
        if (status > 0) {
            mtwarn(WM_OSCAP_LOGTAG, "Ignoring content '%s' due to error (%d).", eval->path, status);
            mtdebug2(WM_OSCAP_LOGTAG, "OUTPUT: %s", output);
            eval->flags.error = 1;
        }

        break;

    case WM_ERROR_TIMEOUT:
        free(output);
        output = NULL;
        wm_strcat(&output, "oscap: ERROR: Timeout expired.", '\0');
        mterror(WM_OSCAP_LOGTAG, "Timeout expired executing '%s'.", eval->path);
        break;

    default:
        mterror(WM_OSCAP_LOGTAG, "Internal calling. Exiting...");
        pthread_exit(NULL);
    }

    for (line = strtok(output, "\n"); line; line = strtok(NULL, "\n")){
        timeout.tv_usec = usec;
        select(0 , NULL, NULL, NULL, &timeout);
        SendMSG(queue_fd, line, WM_OSCAP_LOCATION, LOCALFILE_MQ);
    }

    snprintf(msg, OS_MAXSTR, "Ending OpenSCAP scan. File: %s. ", eval->path);
    timeout.tv_usec = usec;
    select(0 , NULL, NULL, NULL, &timeout);
    SendMSG(queue_fd, msg, "rootcheck", ROOTCHECK_MQ);

    free(output);
    free(command);
    free(arg_profiles);
}

// Check configuration

void wm_oscap_check() {
    wm_oscap_eval *eval;

    // Check if disabled

    if (!oscap->flags.enabled) {
        mtinfo(WM_OSCAP_LOGTAG, "Module disabled. Exiting...");
        pthread_exit(NULL);
    }

    // Check if evals

    if (!oscap->evals) {
        mtwarn(WM_OSCAP_LOGTAG, "No evals defined. Exiting...");
        pthread_exit(NULL);
    }

    // Check if interval

    if (!oscap->interval)
        oscap->interval = WM_OSCAP_DEF_INTERVAL;

    // Check timeout and flags for evals

    for (eval = oscap->evals; eval; eval = eval->next) {
        if (!eval->timeout)
            if (!(eval->timeout = oscap->timeout))
                eval->timeout = WM_OSCAP_DEF_TIMEOUT;
    }
}

// Show module info

void wm_oscap_info() {
    wm_oscap_eval *eval;
    wm_oscap_profile *profile;

    mtinfo(WM_OSCAP_LOGTAG, "SHOW_MODULE_OSCAP: ----");
    mtinfo(WM_OSCAP_LOGTAG, "Timeout: %d", oscap->timeout);
    mtinfo(WM_OSCAP_LOGTAG, "Policies:");

    for (eval = (wm_oscap_eval*)oscap->evals; eval; eval = eval->next){
        mtinfo(WM_OSCAP_LOGTAG, "[%s]", eval->path);
        mtinfo(WM_OSCAP_LOGTAG, "\tProfiles:");

        for (profile = (wm_oscap_profile*)eval->profiles; profile; profile = profile->next)
            mtinfo(WM_OSCAP_LOGTAG, "\t\tName: %s", profile->name);
    }

    mtinfo(WM_OSCAP_LOGTAG, "SHOW_MODULE_OSCAP: ----");
}

// Destroy data

void wm_oscap_destroy(wm_oscap *oscap) {
    wm_oscap_eval *cur_eval;
    wm_oscap_eval *next_eval;
    wm_oscap_profile *cur_profile;
    wm_oscap_profile *next_profile;

    // Delete evals

    for (cur_eval = oscap->evals; cur_eval; cur_eval = next_eval) {

        // Delete profiles

        for (cur_profile = cur_eval->profiles; cur_profile; cur_profile = next_profile) {
            next_profile = cur_profile->next;
            free(cur_profile->name);
            free(cur_profile);
        }

        next_eval = cur_eval->next;
        free(cur_eval->path);
        free(cur_eval->xccdf_id);
        free(cur_eval->oval_id);
        free(cur_eval->ds_id);
        free(cur_eval->cpe);
        free(cur_eval);
    }

    free(oscap);
}
