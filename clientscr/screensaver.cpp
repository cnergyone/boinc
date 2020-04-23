// This file is part of BOINC.
// http://boinc.berkeley.edu
// Copyright (C) 2020 University of California
//
// BOINC is free software; you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation,
// either version 3 of the License, or (at your option) any later version.
//
// BOINC is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with BOINC.  If not, see <http://www.gnu.org/licenses/>.

// Screensaver coordinator.
// Alternates between a "default screensaver"
// and application graphics for running jobs.
// Periods are configurable via config file "ss_config.xml".
// See http://boinc.berkeley.edu/trac/wiki/ScreensaverEnhancements

#ifdef _WIN32
#include "boinc_win.h"
#endif

#ifdef __APPLE__
#include <Carbon/Carbon.h>
#include <sys/wait.h>
#include <app_ipc.h>
#include <malloc/malloc.h>
#include <pthread.h>
#include <sys/stat.h>

extern pthread_mutex_t saver_mutex;
#endif

// Common application includes
//
#include "diagnostics.h"
#include "common_defs.h"
#include "util.h"
#include "common_defs.h"
#include "filesys.h"
#include "error_numbers.h"
#include "gui_rpc_client.h"
#include "str_util.h"
#include "str_replace.h"
#include "screensaver.h"

// Platform specific application includes
//
#if   defined(_WIN32)
#include "screensaver_win.h"
typedef HANDLE GFXAPP_ID;
#define DataMgmtProcType DWORD WINAPI
#elif defined(__APPLE__)
#include "Mac_Saver_Module.h"
#include "shmem.h"
typedef int GFXAPP_ID;
#define DataMgmtProcType void*
#endif


#ifdef _WIN32
// Allow for Unicode wide characters
#define PATH_SEPARATOR (_T("\\"))
#define THE_DEFAULT_SS_EXECUTABLE (_T(DEFAULT_SS_EXECUTABLE))
#define THE_SS_CONFIG_FILE (_T(SS_CONFIG_FILE))
#define DEFAULT_GFX_CANT_CONNECT ERR_CONNECT
#else
// Using (_T()) here causes compiler errors on Mac
#define PATH_SEPARATOR "/"
#define THE_DEFAULT_SS_EXECUTABLE DEFAULT_SS_EXECUTABLE
#define THE_SS_CONFIG_FILE SS_CONFIG_FILE
#define DEFAULT_GFX_CANT_CONNECT (ERR_CONNECT & 0xff)
#endif


// Flags for testing & debugging
#define SIMULATE_NO_GRAPHICS 0

RESULT* graphics_app_result_ptr = NULL;

#ifdef __APPLE__
pid_t* pid_from_shmem = NULL;
#endif

bool CScreensaver::is_same_task(RESULT* taska, RESULT* taskb) {
    if ((taska == NULL) || (taskb == NULL)) return false;
    if (strcmp(taska->name, taskb->name)) return false;
    if (strcmp(taska->project_url, taskb->project_url)) return false;
    return true;
}

int CScreensaver::count_active_graphic_apps(RESULTS& res, RESULT* exclude) {
    int i = 0;
    unsigned int graphics_app_count = 0;

    // Count the number of active graphics-capable apps excluding the specified result.
    // If exclude is NULL, don't exclude any results.
    //
    for (i = res.results.size()-1; i >=0 ; i--) {
        BOINCTRACE(_T("count_active_graphic_apps -- active task detected\n"));
        BOINCTRACE(
            _T("count_active_graphic_apps -- name = '%s', path = '%s'\n"),
            res.results[i]->name, res.results[i]->graphics_exec_path
        );

        if (!strlen(res.results[i]->graphics_exec_path)) continue;
        if (is_same_task(res.results[i], exclude)) continue;
#ifdef __APPLE__
        // Remove it from the vector if incompatible with current version of OS X
        if (isIncompatible(res.results[i]->graphics_exec_path)) {
            BOINCTRACE(
                _T("count_active_graphic_apps -- removing incompatible name = '%s', path = '%s'\n"),
                res.results[i]->name, res.results[i]->graphics_exec_path
            );
            RESULT *rp = res.results[i];
            res.results.erase(res.results.begin()+i);
            delete rp;
            continue;
        }
#endif
        BOINCTRACE(_T("count_active_graphic_apps -- active task detected w/graphics\n"));

        graphics_app_count++;
    }
    return graphics_app_count;
}


// Choose a random graphics application out of the vector.
// Exclude the specified result unless it is the only candidate.
// If exclude is NULL or an empty string, don't exclude any results.
//
RESULT* CScreensaver::get_random_graphics_app(
    RESULTS& res, RESULT* exclude
) {
    RESULT*      rp = NULL;
    unsigned int i = 0;
    unsigned int graphics_app_count = 0;
    unsigned int random_selection = 0;
    unsigned int current_counter = 0;
    RESULT *avoid = exclude;

    BOINCTRACE(_T("get_random_graphics_app -- Function Start\n"));

    graphics_app_count = count_active_graphic_apps(res, avoid);
    BOINCTRACE(_T("get_random_graphics_app -- graphics_app_count = '%d'\n"), graphics_app_count);

    // If no graphics app found other than the one excluded, count again without excluding any
    if ((0 == graphics_app_count) && (avoid != NULL)) {
        avoid = NULL;
        graphics_app_count = count_active_graphic_apps(res, avoid);
    }
        
    // If no graphics app was found, return NULL
    if (0 == graphics_app_count) {
        goto CLEANUP;
    }

    // Choose which application to display.
    //
    random_selection = (rand() % graphics_app_count) + 1;
    BOINCTRACE(_T("get_random_graphics_app -- random_selection = '%d'\n"), random_selection);

    // find the chosen graphics application.
    //
    for (i = 0; i < res.results.size(); i++) {
        if (!strlen(res.results[i]->graphics_exec_path)) continue;
        if (is_same_task(res.results[i], avoid)) continue;

        current_counter++;
        if (current_counter == random_selection) {
            rp = res.results[i];
            break;
        }
    }

CLEANUP:
    BOINCTRACE(_T("get_random_graphics_app -- Function End\n"));

    return rp;
}


#ifdef __APPLE__
void CScreensaver::markAsIncompatible(char *gfxAppPath) {
    char *buf = (char *)malloc(strlen(gfxAppPath)+1);
    if (buf) {
        strlcpy(buf, gfxAppPath, malloc_size(buf));
        m_vIncompatibleGfxApps.push_back(buf);
       BOINCTRACE(_T("markAsIncompatible -- path = '%s'\n"), gfxAppPath);
    }
}

bool CScreensaver::isIncompatible(char *appPath) {
    unsigned int i = 0;
    for (i = 0; i < m_vIncompatibleGfxApps.size(); i++) {
        BOINCTRACE(
            _T("isIncompatible -- comparing incompatible path '%s' to candidate path %s\n"),
            m_vIncompatibleGfxApps[i], appPath
        );
        if (strcmp(m_vIncompatibleGfxApps[i], appPath) == 0) {
            return true;
        }
    }
    return false;
}

#endif


// Launch a project (science) graphics application
//
int CScreensaver::launch_screensaver(RESULT* rp, GFXAPP_ID& graphics_application) {
    int retval = 0;
    
    if (strlen(rp->graphics_exec_path)) {
        // V6 Graphics
#ifdef __APPLE__
        if (gIsCatalina) {
            // As of OS 10.15 (Catalina) screensavers can no longer:
            //  - launch apps that run setuid or setgid
            //  - launch apps downloaded from the Internet which have not been 
            //    specifically approved by the  user via Gatekeeper.
            // So instead of launching graphics apps via gfx_switcher, we send an 
            // RPC to the client asking the client to launch them via gfx_switcher.
            // See comments in gfx_switcher.cpp for a more detailed explanation.
            // We have tested this on OS 10.13 High Sierra and it works there, too
            //
            retval = rpc->run_graphics_app("runfullscreen", rp->slot, gUserName);
            for (int i=0; i<800; i++) {
                boinc_sleep(0.01);      // Wait 8 seconds max
                if (*pid_from_shmem != 0) {
                    graphics_application = *pid_from_shmem;
                    break;
                }
            }
            // fprintf(stderr, "launch_screensaver got pid %d\n", graphics_application);
            // Inform our helper app what we launched 
            fprintf(m_gfx_Cleanup_IPC, "%d\n", graphics_application);
            fflush(m_gfx_Cleanup_IPC);
        } else {
            // For sandbox security, use gfx_switcher to launch gfx app 
            // as user boinc_project and group boinc_project.
            //
            // For unknown reasons, the graphics application exits with 
            // "RegisterProcess failed (error = -50)" unless we pass its 
            // full path twice in the argument list to execv.
            char* argv[5];
            argv[0] = "gfx_Switcher";
            argv[1] = "-launch_gfx";
            argv[2] = strrchr(rp->slot_path, '/');
            if (*argv[2]) argv[2]++;    // Point to the slot number in ascii
            
            argv[3] = "--fullscreen";
            argv[4] = 0;

           retval = run_program(
                rp->slot_path,
                m_gfx_Switcher_Path,
                4,
                argv,
                0,
                graphics_application
            );
    }
    
    if (graphics_application) {
        launchedGfxApp(rp->graphics_exec_path, graphics_application, rp->slot);
    }
#else
        char* argv[3];
        argv[0] = rp->graphics_exec_path;
        argv[1] = "--fullscreen";
        argv[2] = 0;
        retval = run_program(
            rp->slot_path,
            rp->graphics_exec_path,
            2,
            argv,
            0,
            graphics_application
        );
#endif
    }
    return retval;
}


// Terminate any screensaver graphics application
//
int CScreensaver::terminate_v6_screensaver(GFXAPP_ID& graphics_application, RESULT* rp) {
    int retval = 0;
    int i;

#ifdef __APPLE__
    pid_t thePID;
    
    if (gIsCatalina) {
        // As of OS 10.15 (Catalina) screensavers can no longer launch apps
        // that run setuid or setgid. So instead of killing graphics apps 
        // via gfx_switcher, we send an RPC to the client asking the client 
        // to kill them via switcher.
        // We have tested this on OS 10.13 High Sierra and it works there, too
        //
        int ignore;

        if (graphics_application == 0) return 0;

        // MUTEX may help prevent crashes when terminating an older gfx app which
        // we were displaying using CGWindowListCreateImage under OS X >= 10.13
        // Also prevents reentry when called from our other thread
        pthread_mutex_lock(&saver_mutex);

        thePID = graphics_application;
        // fprintf(stderr, "stopping pid %d\n", thePID);
        retval = rpc->run_graphics_app("stop", thePID, gUserName);
        //kill_program(graphics_application);

        // Inform our helper app that we have stopped current graphics app 
        fprintf(m_gfx_Cleanup_IPC, "0\n");
        fflush(m_gfx_Cleanup_IPC);

        launchedGfxApp("", 0, -1);

        for (i=0; i<200; i++) {
            boinc_sleep(0.01);      // Wait 2 seconds max
            if (HasProcessExited(graphics_application, ignore)) {
                break;
            }
        }
        pthread_mutex_unlock(&saver_mutex);
    
    } else {
        // Under sandbox security, use gfx_switcher to kill default gfx app 
        // as user boinc_master and group boinc_master (for default gfx app)
        // or user boinc_project and group boinc_project (for project gfx 
        // apps.) The man page for kill() says the user ID of the process 
        // sending the signal must match that of the target process, though 
        // in practice that seems not to be true on the Mac.
        
        char current_dir[PATH_MAX];
        char gfx_pid[16];
        
        if (graphics_application == 0) return 0;
        
        // MUTEX may help prevent crashes when terminating an older gfx app which
        // we were displaying using CGWindowListCreateImage under OS X >= 10.13
        // Also prevents reentry when called from our other thread
        pthread_mutex_lock(&saver_mutex);

        sprintf(gfx_pid, "%d", graphics_application);
        getcwd( current_dir, sizeof(current_dir));

        char* argv[4];
        argv[0] = "gfx_switcher";
        argv[1] = "-kill_gfx";
        argv[2] = gfx_pid;
        argv[3] = 0;

        retval = run_program(
            current_dir,
            m_gfx_Switcher_Path,
            3,
            argv,
            0,
            thePID
        );

        if (graphics_application) {
            launchedGfxApp("", 0, -1);
        }
        
        for (i=0; i<200; i++) {
            boinc_sleep(0.01);      // Wait 2 seconds max
            // Prevent gfx_switcher from becoming a zombie
            if (waitpid(thePID, 0, WNOHANG) == thePID) {
                break;
            }
        }
        pthread_mutex_unlock(&saver_mutex);
    }
    
#endif

#ifdef _WIN32
        HWND hBOINCGraphicsWindow = FindWindow(BOINC_WINDOW_CLASS_NAME, NULL);
        if (hBOINCGraphicsWindow) {
            CloseWindow(hBOINCGraphicsWindow);
            Sleep(1000);
            hBOINCGraphicsWindow = FindWindow(BOINC_WINDOW_CLASS_NAME, NULL);
            if (hBOINCGraphicsWindow) {
                kill_program(graphics_application);
            }
        }
#endif

    // For safety, call kill_program even under Apple sandbox security
    kill_program(graphics_application);
    return retval;
}


// Terminate the project (science) graphics application
//
int CScreensaver::terminate_screensaver(GFXAPP_ID& graphics_application, RESULT* rp) {
    int retval = 0;

    if (graphics_application) {
        // V6 Graphics
        if (m_bScience_gfx_running) {
            terminate_v6_screensaver(graphics_application, rp);
        }
    }
    return retval;
}


// Launch the default graphics application
//
int CScreensaver::launch_default_screensaver(char *dir_path, GFXAPP_ID& graphics_application) {
    int retval = 0;
    int num_args;
    
#ifdef __APPLE__
    if (gIsCatalina) {
        // As of OS 10.15 (Catalina) screensavers can no longer:
        //  - launch apps that run setuid or setgid
        //  - launch apps downloaded from the Internet which have not been 
        //    specifically approved by the  user via Gatekeeper.
        // So instead of launching graphics apps via gfx_switcher, we send an 
        // RPC to the client asking the client to launch them via gfx_switcher.
        // See comments in gfx_switcher.cpp for a more detailed explanation.
        // We have tested this on OS 10.13 High Sierra and it works there, too
        //
        int thePID = -1;
        retval = rpc->run_graphics_app("runfullscreen", thePID, gUserName);
        for (int i=0; i<800; i++) {
            boinc_sleep(0.01);      // Wait 8 seconds max
            if (*pid_from_shmem != 0) {
                graphics_application = *pid_from_shmem;
                break;
            }
        }
        // fprintf(stderr, "launch_screensaver got pid %d\n", graphics_application);
        // Inform our helper app what we launched 
        fprintf(m_gfx_Cleanup_IPC, "%d\n", graphics_application);
        fflush(m_gfx_Cleanup_IPC);
    } else {
        // For sandbox security, use gfx_switcher to launch default 
        // gfx app as user boinc_master and group boinc_master.
        char* argv[6];

        argv[0] = "gfx_switcher";
        argv[1] = "-default_gfx";
        argv[2] = THE_DEFAULT_SS_EXECUTABLE;    // Will be changed by gfx_switcher
        argv[3] = "--fullscreen";
        argv[4] = 0;
        argv[5] = 0;
        if (!m_bConnected) {
            BOINCTRACE(_T("launch_default_screensaver using --retry_connect argument\n"));
            argv[4] = "--retry_connect";
            num_args = 5;
        } else {
            num_args = 4;
        }

        retval = run_program(
            dir_path,
            m_gfx_Switcher_Path,
            num_args,
            argv,
            0,
            graphics_application
        );
    }
    
    if (graphics_application) {
        launchedGfxApp("boincscr", graphics_application, -1);
    }

    BOINCTRACE(_T("launch_default_screensaver returned %d\n"), retval);
    
#else
    char* argv[4];
    char full_path[1024];

    strlcpy(full_path, dir_path, sizeof(full_path));
    strlcat(full_path, PATH_SEPARATOR, sizeof(full_path));
    strlcat(full_path, THE_DEFAULT_SS_EXECUTABLE, sizeof(full_path));

    argv[0] = full_path;
    argv[1] = "--fullscreen";
    argv[2] = 0;
    argv[3] = 0;
    if (!m_bConnected) {
        BOINCTRACE(_T("launch_default_screensaver using --retry_connect argument\n"));
        argv[2] = "--retry_connect";
        num_args = 3;
    } else {
        num_args = 2;
    }
    
    retval = run_program(
        dir_path,
        full_path,
        num_args,
        argv,
        0,
        graphics_application
    );
    
     BOINCTRACE(_T("launch_default_screensaver %s returned %d\n"), full_path, retval);

#endif
     return retval;
}


// Terminate the default graphics application
//
int CScreensaver::terminate_default_screensaver(GFXAPP_ID& graphics_application) {
    int retval = 0;

    if (! graphics_application) return 0;
    retval = terminate_v6_screensaver(graphics_application, NULL);
    return retval;
}


// If we cannot connect to the core client:
//   - we retry connecting every 10 seconds 
//   - we launch the default graphics application with the argument --retry_connect, so 
//     it will continue running and will also retry connecting every 10 seconds.
//
// If we successfully connected to the core client, launch the default graphics application 
// without the argument --retry_connect.  If it can't connect, it will return immediately 
// with the exit code ERR_CONNECT.  In that case, we assume it was blocked by a firewall 
// and so we run only project (science) graphics.

DataMgmtProcType CScreensaver::DataManagementProc() {
    int             retval                      = 0;
    int             suspend_reason              = 0;
    RESULT*         theResult                   = NULL;
    RESULT          previous_result;
    // previous_result_ptr = &previous_result when previous_result is valid, else NULL
    RESULT*         previous_result_ptr         = NULL;
    int             iResultCount                = 0;
    int             iIndex                      = 0;
    double          default_phase_start_time    = 0.0;
    double          science_phase_start_time    = 0.0;
    double          last_change_time            = 0.0;
    // If we run default screensaver during science phase because no science graphics 
    // are available, then shorten next default graphics phase by that much time.
    double          default_saver_start_time_in_science_phase    = 0.0;
    double          default_saver_duration_in_science_phase      = 0.0;

    SS_PHASE        ss_phase                    = DEFAULT_SS_PHASE;
    bool            switch_to_default_gfx       = false;
    bool            killing_default_gfx         = false;
    int             exit_status                 = 0;
    
    char*           default_ss_dir_path         = NULL;
    char            full_path[1024];

    BOINCTRACE(_T("CScreensaver::DataManagementProc - Display screen saver loading message\n"));
    SetError(TRUE, SCRAPPERR_BOINCSCREENSAVERLOADING);  // No GFX App is running: show moving BOINC logo
#ifdef _WIN32
    m_tThreadCreateTime = time(0);

    // Set the starting point for iterating through the results
    m_iLastResultShown = 0;
    m_tLastResultChangeTime = 0;
#endif

    m_bDefault_ss_exists = false;
    m_bScience_gfx_running = false;
    m_bDefault_gfx_running = false;
    m_bShow_default_ss_first = false;
    graphics_app_result_ptr = NULL;

#ifdef __APPLE__
    m_vIncompatibleGfxApps.clear();
    default_ss_dir_path = "/Library/Application Support/BOINC Data";
    if (gIsCatalina) {
        char shmem_name[MAXPATHLEN];
        snprintf(shmem_name, sizeof(shmem_name), "/tmp/boinc_ss_%s", gUserName);
        retval = create_shmem_mmap(shmem_name, sizeof(int), (void**)&pid_from_shmem);
        // make sure user/group RW permissions are set, but not other.
        //
        if (retval == 0) {
            chmod(shmem_name, 0666);
            retval = attach_shmem_mmap(shmem_name, (void**)&pid_from_shmem);
        }
        if (retval == 0) {
            *pid_from_shmem = 0;
        }
    }
#else
    default_ss_dir_path = (char*)m_strBOINCInstallDirectory.c_str();
#endif

    strlcpy(full_path, default_ss_dir_path, sizeof(full_path));
    strlcat(full_path, PATH_SEPARATOR, sizeof(full_path));
    strlcat(full_path, THE_DEFAULT_SS_EXECUTABLE, sizeof(full_path));
        
    if (boinc_file_exists(full_path)) {
        m_bDefault_ss_exists = true;
    } else {
        SetError(TRUE, SCRAPPERR_CANTLAUNCHDEFAULTGFXAPP);  // No GFX App is running: show moving BOINC logo
    }
    
    if (m_bDefault_ss_exists && m_bShow_default_ss_first) {
        ss_phase = DEFAULT_SS_PHASE;
        default_phase_start_time = dtime();
        science_phase_start_time = 0;
        switch_to_default_gfx = true;
    } else {
        ss_phase = SCIENCE_SS_PHASE;
        default_phase_start_time = 0;
        science_phase_start_time = dtime();
    }

    while (true) {
        for (int i = 0; i < 4; i++) {
            // ***
            // *** Things that should be run frequently.
            // ***   4 times per second.
            // ***

            // Are we supposed to exit the screensaver?
            if (m_bQuitDataManagementProc) {     // If main thread has requested we exit
                BOINCTRACE(_T("CScreensaver::DataManagementProc - Thread told to stop\n"));
                if (m_hGraphicsApplication || graphics_app_result_ptr) {
                    if (m_bDefault_gfx_running) {
                        BOINCTRACE(_T("CScreensaver::DataManagementProc - Terminating default screensaver\n"));
                        terminate_default_screensaver(m_hGraphicsApplication);
                    } else {
                        BOINCTRACE(_T("CScreensaver::DataManagementProc - Terminating screensaver\n"));
                        terminate_screensaver(m_hGraphicsApplication, graphics_app_result_ptr);
                    }
                    graphics_app_result_ptr = NULL;
                    previous_result_ptr = NULL;
                    m_hGraphicsApplication = 0;
                }
                BOINCTRACE(_T("CScreensaver::DataManagementProc - Stopping...\n"));
                m_bDataManagementProcStopped = true; // Tell main thread that we exited
                return 0;                       // Exit the thread
            }
            boinc_sleep(0.25);
        }

        // ***
        // *** Things that should be run less frequently.
        // *** 1 time per second.
        // ***

        // Blank screen saver?
        if ((m_dwBlankScreen) && (time(0) > m_dwBlankTime) && (m_dwBlankTime > 0)) {
            BOINCTRACE(_T("CScreensaver::DataManagementProc - Time to blank\n"));
            SetError(FALSE, SCRAPPERR_SCREENSAVERBLANKED);    // Blanked - hide moving BOINC logo
            m_bQuitDataManagementProc = true;
            continue;       // Code above will exit the thread
        }

        BOINCTRACE(_T("CScreensaver::DataManagementProc - ErrorMode = '%d', ErrorCode = '%x'\n"), m_bErrorMode, m_hrError);

        if (!m_bConnected) {
            HandleRPCError();
        }
        
        if (m_bConnected) {
            // Do we need to get the core client state?
            if (m_bResetCoreState) {
                // Try and get the current state of the CC
                retval = rpc->get_state(state);
                if (retval) {
                    // CC may not yet be running
                    HandleRPCError();
                    continue;
                } else {
                    m_bResetCoreState = false;
                }
            }
    
            // Update our task list
            retval = rpc->get_screensaver_tasks(suspend_reason, results);
            if (retval) {
                // rpc call returned error
                HandleRPCError();
                m_bResetCoreState = true;
                continue;
            }
        } else {
            results.clear();
        }
        
        // Time to switch to default graphics phase?
        if (m_bDefault_ss_exists && (ss_phase == SCIENCE_SS_PHASE) && (m_fGFXDefaultPeriod > 0)) {
            if (science_phase_start_time && ((dtime() - science_phase_start_time) > m_fGFXSciencePeriod)) {
                if (!m_bDefault_gfx_running) {
                    switch_to_default_gfx = true;
                }
                ss_phase = DEFAULT_SS_PHASE;
                default_phase_start_time = dtime();
                science_phase_start_time = 0;
                if (m_bDefault_gfx_running && default_saver_start_time_in_science_phase) {
                    // Remember how long default graphics ran during science phase
                    default_saver_duration_in_science_phase += (dtime() - default_saver_start_time_in_science_phase); 
                }
                default_saver_start_time_in_science_phase = 0;
            }
        }
        
        // Time to switch to science graphics phase?
        if ((ss_phase == DEFAULT_SS_PHASE) && m_bConnected && (m_fGFXSciencePeriod > 0)) {
            if (default_phase_start_time && 
                    ((dtime() - default_phase_start_time + default_saver_duration_in_science_phase) 
                    > m_fGFXDefaultPeriod)) {
                // BOINCTRACE(_T("CScreensaver::Ending Default phase: now=%f, default_phase_start_time=%f, default_saver_duration_in_science_phase=%f\n"),
                // dtime(), default_phase_start_time, default_saver_duration_in_science_phase);
                ss_phase = SCIENCE_SS_PHASE;
                default_phase_start_time = 0;
                default_saver_duration_in_science_phase = 0;
                science_phase_start_time = dtime();
                if (m_bDefault_gfx_running) {
                    default_saver_start_time_in_science_phase = science_phase_start_time;
                }
                switch_to_default_gfx = false;
            }
        }

        // Core client suspended?
        // We ignore SUSPEND_REASON_CPU_USAGE in SS coordinator, so it won't kill graphics apps for
        // short-term CPU usage spikes (such as anti-virus.)  Added 9 April 2010
        if (suspend_reason && !(suspend_reason & (SUSPEND_REASON_CPU_THROTTLE | SUSPEND_REASON_CPU_USAGE))) {
            if (!m_bDefault_gfx_running) {
                SetError(TRUE, m_hrError);          // No GFX App is running: show moving BOINC logo
                if (m_bDefault_ss_exists) {
                    switch_to_default_gfx = true;
                }
            }
        }
        
        if (switch_to_default_gfx) {
            if (m_bScience_gfx_running) {
                if (m_hGraphicsApplication || previous_result_ptr) {
                    // use previous_result_ptr because graphics_app_result_ptr may no longer be valid
                    terminate_screensaver(m_hGraphicsApplication, previous_result_ptr);
                    if (m_hGraphicsApplication == 0) {
                        graphics_app_result_ptr = NULL;
                        m_bScience_gfx_running = false;
                    } else {
                        // HasProcessExited() test will clear m_hGraphicsApplication and graphics_app_result_ptr
                    }
                    previous_result_ptr = NULL;
                }
            } else {
                if (!m_bDefault_gfx_running) {
                    switch_to_default_gfx = false;
                    retval = launch_default_screensaver(default_ss_dir_path, m_hGraphicsApplication);
                    if (retval) {
                        m_hGraphicsApplication = 0;
                        previous_result_ptr = NULL;
                        graphics_app_result_ptr = NULL;
                        m_bDefault_gfx_running = false;
                        SetError(TRUE, SCRAPPERR_CANTLAUNCHDEFAULTGFXAPP);  // No GFX App is running: show moving BOINC logo
                   } else {
                        m_bDefault_gfx_running = true;
                        if (ss_phase == SCIENCE_SS_PHASE) {
                            default_saver_start_time_in_science_phase = dtime();
                        }
                        SetError(FALSE, SCRAPPERR_BOINCSCREENSAVERLOADING);    // A GFX App is running: hide moving BOINC logo
                    }
                }
            }
        }

        if ((ss_phase == SCIENCE_SS_PHASE) && !switch_to_default_gfx) {
        
#if SIMULATE_NO_GRAPHICS /* FOR TESTING */

            if (!m_bDefault_gfx_running) {
                SetError(TRUE, m_hrError);          // No GFX App is running: show moving BOINC logo
                if (m_bDefault_ss_exists) {
                    switch_to_default_gfx = true;
                }
            }

#else                   /* NORMAL OPERATION */

            if (m_bScience_gfx_running) {
                // Is the current graphics app's associated task still running?
                
                if ((m_hGraphicsApplication) || (graphics_app_result_ptr)) {
                    iResultCount = (int)results.results.size();
                    graphics_app_result_ptr = NULL;

                    // Find the current task in the new results vector (if it still exists)
                    for (iIndex = 0; iIndex < iResultCount; iIndex++) {
                        theResult = results.results.at(iIndex);

                        if (is_same_task(theResult, previous_result_ptr)) {
                            graphics_app_result_ptr = theResult;
                            previous_result = *theResult;
                            previous_result_ptr = &previous_result;
                            break;
                        }
                    }

                    // V6 graphics only: if worker application has stopped running, terminate_screensaver
                    if ((graphics_app_result_ptr == NULL) && (m_hGraphicsApplication != 0)) {
                        if (previous_result_ptr) {
                            BOINCTRACE(_T("CScreensaver::DataManagementProc - %s finished\n"), 
                                previous_result.graphics_exec_path
                            );
                        }
                        terminate_screensaver(m_hGraphicsApplication, previous_result_ptr);
                        previous_result_ptr = NULL;
                        if (m_hGraphicsApplication == 0) {
                            graphics_app_result_ptr = NULL;
                            m_bScience_gfx_running = false;
                            // Save previous_result and previous_result_ptr for get_random_graphics_app() call
                        } else {
                            // HasProcessExited() test will clear m_hGraphicsApplication and graphics_app_result_ptr
                        }
                    }

                     if (last_change_time && (m_fGFXChangePeriod > 0) && ((dtime() - last_change_time) > m_fGFXChangePeriod) ) {
                        if (count_active_graphic_apps(results, previous_result_ptr) > 0) {
                            if (previous_result_ptr) {
                                BOINCTRACE(_T("CScreensaver::DataManagementProc - time to change: %s / %s\n"), 
                                    previous_result.name, previous_result.graphics_exec_path
                                );
                            }
                            terminate_screensaver(m_hGraphicsApplication, graphics_app_result_ptr);
                            if (m_hGraphicsApplication == 0) {
                                graphics_app_result_ptr = NULL;
                                m_bScience_gfx_running = false;
                                // Save previous_result and previous_result_ptr for get_random_graphics_app() call
                            } else {
                                // HasProcessExited() test will clear m_hGraphicsApplication and graphics_app_result_ptr
                            }
                        }
                        last_change_time = dtime();
                    }
                }
            }       // End if (m_bScience_gfx_running)
        
            // If no current graphics app, pick an active task at random
            // and launch its graphics app
            //
            if ((m_bDefault_gfx_running || (m_hGraphicsApplication == 0)) && (graphics_app_result_ptr == NULL)) {
                if (suspend_reason && !(suspend_reason & (SUSPEND_REASON_CPU_THROTTLE | SUSPEND_REASON_CPU_USAGE))) {
                    graphics_app_result_ptr = NULL;
                } else {
                    graphics_app_result_ptr = get_random_graphics_app(results, previous_result_ptr);
                    previous_result_ptr = NULL;
                }
                
                if (graphics_app_result_ptr) {
                    if (m_bDefault_gfx_running) {
                        terminate_default_screensaver(m_hGraphicsApplication);
                        killing_default_gfx = true;
                        // Remember how long default graphics ran during science phase
                        if (default_saver_start_time_in_science_phase) {
                            default_saver_duration_in_science_phase += (dtime() - default_saver_start_time_in_science_phase); 
                            //BOINCTRACE(_T("CScreensaver::During Science phase: now=%f, default_saver_start_time=%f, default_saver_duration=%f\n"),
                            //    dtime(), default_saver_start_time_in_science_phase, default_saver_duration_in_science_phase);
                        }
                        default_saver_start_time_in_science_phase = 0;
                        // HasProcessExited() test will clear
                        // m_hGraphicsApplication and graphics_app_result_ptr
                     } else {
                        retval = launch_screensaver(graphics_app_result_ptr, m_hGraphicsApplication);
                        if (retval) {
                            m_hGraphicsApplication = 0;
                            previous_result_ptr = NULL;
                            graphics_app_result_ptr = NULL;
                            m_bScience_gfx_running = false;
                        } else {
                            // A GFX App is running: hide moving BOINC logo
                            //
                            SetError(FALSE, SCRAPPERR_BOINCSCREENSAVERLOADING);
                            last_change_time = dtime();
                            m_bScience_gfx_running = true;
                            // Make a local copy of current result, since original pointer 
                            // may have been freed by the time we perform later tests
                            previous_result = *graphics_app_result_ptr;
                            previous_result_ptr = &previous_result;
                            if (previous_result_ptr) {
                                BOINCTRACE(_T("CScreensaver::DataManagementProc - launching %s\n"), 
                                    previous_result.graphics_exec_path
                                );
                            }
                        }
                    }
                } else {
                    if (!m_bDefault_gfx_running) {
                        // We can't run a science graphics app, so run the default graphics if available
                        SetError(TRUE, m_hrError); 
                        if (m_bDefault_ss_exists) {
                            switch_to_default_gfx = true;
                        }
                    }

                }   // End if no science graphics available
            }      // End if no current science graphics app is running

#endif      // ! SIMULATE_NO_GRAPHICS

            if (switch_to_default_gfx) {
                switch_to_default_gfx = false;
                if (!m_bDefault_gfx_running) {
                    retval = launch_default_screensaver(default_ss_dir_path, m_hGraphicsApplication);
                    if (retval) {
                        m_hGraphicsApplication = 0;
                        previous_result_ptr = NULL;
                        graphics_app_result_ptr = NULL;
                        m_bDefault_gfx_running = false;
                        SetError(TRUE, SCRAPPERR_CANTLAUNCHDEFAULTGFXAPP);
                        // No GFX App is running: show BOINC logo
                    } else {
                        m_bDefault_gfx_running = true;
                        default_saver_start_time_in_science_phase = dtime();
                        SetError(FALSE, SCRAPPERR_BOINCSCREENSAVERLOADING);
                        // Default GFX App is running: hide moving BOINC logo
                    }
                }
            }
        }   // End if ((ss_phase == SCIENCE_SS_PHASE) && !switch_to_default_gfx)
        
        
        
        // Is the graphics app still running?
        if (m_hGraphicsApplication) {
            if (HasProcessExited(m_hGraphicsApplication, exit_status)) {
                // Something has happened to the previously selected screensaver
                //   application. Start a different one.
                BOINCTRACE(_T("CScreensaver::DataManagementProc - Graphics application isn't running, start a new one.\n"));
                if (m_bDefault_gfx_running) {
                    // If we were able to connect to core client
                    // but gfx app can't, don't use it. 
                    //
                    BOINCTRACE(_T("CScreensaver::DataManagementProc - Default graphics application exited with code %d.\n"), exit_status);
                    if (!killing_default_gfx) {     // If this is an unexpected exit
                        if (exit_status == DEFAULT_GFX_CANT_CONNECT) {
                            SetError(TRUE, SCRAPPERR_DEFAULTGFXAPPCANTCONNECT);
                            // No GFX App is running: show moving BOINC logo
                        } else {
                            SetError(TRUE, SCRAPPERR_DEFAULTGFXAPPCRASHED);
                            // No GFX App is running: show moving BOINC logo
                        }
                        m_bDefault_ss_exists = false;
                        ss_phase = SCIENCE_SS_PHASE;
                    }
                    killing_default_gfx = false;
                }
                SetError(TRUE, SCRAPPERR_BOINCNOGRAPHICSAPPSEXECUTING);
                // No GFX App is running: show moving BOINC logo
                m_hGraphicsApplication = 0;
                graphics_app_result_ptr = NULL;
                m_bDefault_gfx_running = false;
                m_bScience_gfx_running = false;
#ifdef __APPLE__
                launchedGfxApp("", 0, -1);
#endif
                continue;
            }
        }
    }   // end while(true)
}


#ifdef _WIN32
BOOL CScreensaver::HasProcessExited(HANDLE pid_handle, int &exitCode) {
    unsigned long status = 1;
    if (GetExitCodeProcess(pid_handle, &status)) {
        if (status == STILL_ACTIVE) {
            exitCode = 0;
            return false;
        }
    }
    exitCode = (int)status;
    return true;
}
#else
bool CScreensaver::HasProcessExited(pid_t pid, int &exitCode) {
    int status;
    pid_t p;
    
    if (gIsCatalina) {
        // Only the process which launched an app can use waitpid() to test 
        // whether that app is still running. If we sent an RPC to the client 
        // asking the client to launch a graphics app via switcher, we must 
        // send another RPC to the client to call waitpid() for that app.
        //
        if (pid_from_shmem) {
            //fprintf(stderr, "screensaver HasProcessExited got pid_from_shmem = %d\n", *pid_from_shmem);
            if (*pid_from_shmem != 0) return false;
        }
        return true;
    }
    
    p = waitpid(pid, &status, WNOHANG);
    exitCode = WEXITSTATUS(status);
    if (p == pid) return true;     // process has exited
    if (p == -1) return true;      // PID doesn't exist
    exitCode = 0;
    return false;
}
#endif


void CScreensaver::GetDefaultDisplayPeriods(struct ss_periods &periods) {
    char*           default_data_dir_path = NULL;
    char            buf[1024];
    FILE*           f;
    MIOFILE         mf;

    periods.GFXDefaultPeriod = GFX_DEFAULT_PERIOD;
    periods.GFXSciencePeriod = GFX_SCIENCE_PERIOD;
    periods.GFXChangePeriod = GFX_CHANGE_PERIOD;
    periods.Show_default_ss_first = false;

#ifdef __APPLE__
    default_data_dir_path = "/Library/Application Support/BOINC Data";
#else
    default_data_dir_path = (char*)m_strBOINCDataDirectory.c_str();
#endif

    strlcpy(buf, default_data_dir_path, sizeof(buf));
    strlcat(buf, PATH_SEPARATOR, sizeof(buf));
    strlcat(buf, THE_SS_CONFIG_FILE, sizeof(buf));

    f = boinc_fopen(buf, "r");
    if (!f) return;
    
    mf.init_file(f);
    XML_PARSER xp(&mf);

    while (!xp.get_tag()) {
        if (xp.parse_bool("default_ss_first", periods.Show_default_ss_first)) continue;
        if (xp.parse_double("default_gfx_duration", periods.GFXDefaultPeriod)) continue;
        if (xp.parse_double("science_gfx_duration", periods.GFXSciencePeriod)) continue;
        if (xp.parse_double("science_gfx_change_interval", periods.GFXChangePeriod)) continue;
    }
    fclose(f);
    
    BOINCTRACE(
        _T("CScreensaver::GetDefaultDisplayPeriods: m_bShow_default_ss_first=%d, m_fGFXDefaultPeriod=%f, m_fGFXSciencePeriod=%f, m_fGFXChangePeriod=%f\n"),
        (int)periods.Show_default_ss_first,
        periods.GFXDefaultPeriod,
        periods.GFXSciencePeriod,
        periods.GFXChangePeriod
    );
}
