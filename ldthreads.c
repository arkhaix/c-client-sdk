#include <stdlib.h>
#include <stdio.h>
#ifndef _WINDOWS
#include <unistd.h>
#else
#endif
#include <math.h>

#include "ldapi.h"
#include "ldinternal.h"

/*
 * all the code that runs in the background here.
 * plus the server event parser and streaming update handler.
 */

THREAD_RETURN
LDi_bgeventsender(void *const v)
{
    LDClient *const client = v; bool finalflush = false;

    while (true) {
        LDi_wrlock(&client->clientLock);

        const LDStatus status = client->status;

        if (status == LDStatusFailed || finalflush) {
            LDi_log(LD_LOG_TRACE, "killing thread LDi_bgeventsender");
            client->threads--;
            if (!client->threads) { LDi_condsignal(&client->initCond); }
            LDi_wrunlock(&client->clientLock);
            return THREAD_RETURN_DEFAULT;
        }

        int ms = client->shared->sharedConfig->eventsFlushIntervalMillis;
        LDi_wrunlock(&client->clientLock);

        if (status != LDStatusShuttingdown) {
            LDi_log(LD_LOG_TRACE, "bg sender sleeping");
            LDi_mtxenter(&client->condMtx);
            LDi_condwait(&client->eventCond, &client->condMtx, ms);
            LDi_mtxleave(&client->condMtx);
        }
        LDi_log(LD_LOG_TRACE, "bgsender running");

        LDi_rdlock(&client->clientLock);
        if (client->status == LDStatusShuttingdown) {
            finalflush = true;
        }

        if (client->offline) {
            LDi_rdunlock(&client->clientLock);
            continue;
        }
        LDi_rdunlock(&client->clientLock);

        char *const eventdata = LDi_geteventdata(client);
        if (!eventdata) { continue; }

        bool sendfailed = false;
        while (true) {
            int response = 0;

            LDi_sendevents(client, eventdata, &response);

            if (response == 401 || response == 403) {
                LDi_wrlock(&client->clientLock);
                LDi_updatestatus(client, LDStatusFailed);
                LDi_wrunlock(&client->clientLock);

                LDi_log(LD_LOG_ERROR, "mobile key not authorized, event sending failed");

                sendfailed = true; break;
            } else if (response == -1) {
                if (sendfailed) {
                    break;
                } else {
                    sendfailed = true;
                }
            } else {
                sendfailed = false; break;
            }

            LDi_millisleep(1000);
        }

        if (sendfailed) {
            LDi_log(LD_LOG_WARNING, "sending events failed deleting event batch");
        }

        free(eventdata);
    }
}

/*
 * this thread always runs, even when using streaming, but then it just sleeps
 */
THREAD_RETURN
LDi_bgfeaturepoller(void *const v)
{
    LDClient *const client = v;

    while (true) {
        LDi_wrlock(&client->clientLock);

        if (client->status == LDStatusFailed || client->status == LDStatusShuttingdown) {
            LDi_log(LD_LOG_TRACE, "killing thread LDi_bgfeaturepoller");
            client->threads--;
            if (!client->threads) { LDi_condsignal(&client->initCond); }
            LDi_wrunlock(&client->clientLock);
            return THREAD_RETURN_DEFAULT;
        }

        bool skippolling = client->offline;
        int ms = client->shared->sharedConfig->pollingIntervalMillis;
        if (client->background) {
            ms = client->shared->sharedConfig->backgroundPollingIntervalMillis;
            skippolling = skippolling || client->shared->sharedConfig->disableBackgroundUpdating;
        } else {
            skippolling = skippolling || client->shared->sharedConfig->streaming;
        }

        /* this triggers the first time the thread runs, so we don't have to wait */
        if (!skippolling && client->status == LDStatusInitializing) { ms = 0; }
        LDi_wrunlock(&client->clientLock);

        if (ms > 0) {
            LDi_mtxenter(&client->condMtx);
            LDi_condwait(&client->pollCond, &client->condMtx, ms);
            LDi_mtxleave(&client->condMtx);
        }
        if (skippolling) { continue; }

        LDi_rdlock(&client->clientLock);
        if (client->status == LDStatusFailed || client->status == LDStatusShuttingdown) {
            LDi_rdunlock(&client->clientLock);
            continue;
        }
        LDi_rdunlock(&client->clientLock);

        int response = 0;
        char *const data = LDi_fetchfeaturemap(client, &response);

        if (response == 401 || response == 403) {
            LDi_wrlock(&client->clientLock);
            LDi_updatestatus(client, LDStatusFailed);
            LDi_wrunlock(&client->clientLock);

            LDi_log(LD_LOG_ERROR, "mobile key not authorized, polling failed");
        }
        if (!data) { continue; }
        if (LDi_clientsetflags(client, true, data, 1)) {
            LDi_savehash(client);
        }
        free(data);
    }
}

/* exposed for testing */
void
LDi_onstreameventput(LDClient *const client, const char *const data)
{
    if (LDi_clientsetflags(client, true, data, 1)) {
        LDi_rdlock(&client->shared->sharedUserLock);
        LDi_savedata("features", client->shared->sharedUser->key, data);
        LDi_rdunlock(&client->shared->sharedUserLock);
    }
}

static void
applypatch(LDClient *const client, cJSON *const payload, const bool isdelete)
{
    LDNode *patch = NULL;
    if (cJSON_IsObject(payload)) {
        patch = LDi_jsontohash(payload, 2);
    }
    cJSON_Delete(payload);

    LDi_wrlock(&client->clientLock);
    LDNode *hash = client->allFlags;
    LDNode *node, *tmp;
    HASH_ITER(hh, patch, node, tmp) {
        LDNode *res = NULL;
        HASH_FIND_STR(hash, node->key, res);
        if (res && res->version > node->version) {
            /* stale patch, skip */
            continue;
        }
        if (res) {
            HASH_DEL(hash, res);
            LDi_freenode(res);
        }
        if (!isdelete) {
            HASH_DEL(patch, node);
            HASH_ADD_KEYPTR(hh, hash, node->key, strlen(node->key), node);
        }
        for (struct listener *list = client->listeners; list; list = list->next) {
            if (strcmp(list->key, node->key) == 0) {
                LDi_wrunlock(&client->clientLock);
                list->fn(node->key, isdelete ? 1 : 0);
                LDi_wrlock(&client->clientLock);
            }
        }
    }

    client->allFlags = hash;
    LDi_wrunlock(&client->clientLock);

    LDi_freehash(patch);
}

void
LDi_onstreameventpatch(LDClient *const client, const char *const data)
{
    cJSON *const payload = cJSON_Parse(data);

    if (!payload) {
        LDi_log(LD_LOG_ERROR, "parsing patch failed");
        return;
    }

    applypatch(client, payload, false);
    LDi_savehash(client);
}

void
LDi_onstreameventdelete(LDClient *const client, const char *const data)
{
    cJSON *const payload = cJSON_Parse(data);

    if (!payload) {
        LDi_log(LD_LOG_ERROR, "parsing delete patch failed");
        return;
    }

    applypatch(client, payload, 1);
    LDi_savehash(client);
}

static void
onstreameventping(LDClient *const client)
{
    LDi_rdlock(&client->clientLock);

    if (client->status == LDStatusFailed || client->status == LDStatusShuttingdown) {
        LDi_rdunlock(&client->clientLock);
        return;
    }

    LDi_rdunlock(&client->clientLock);

    int response = 0;
    char *const data = LDi_fetchfeaturemap(client, &response);

    if (response == 401 || response == 403) {
        LDi_wrlock(&client->clientLock);
        LDi_updatestatus(client, LDStatusFailed);
        LDi_wrunlock(&client->clientLock);
    }
    if (!data) { return; }
    if (LDi_clientsetflags(client, true, data, 1)) {
        LDi_rdlock(&client->shared->sharedUserLock);
        LDi_savedata("features", client->shared->sharedUser->key, data);
        LDi_rdunlock(&client->shared->sharedUserLock);
    }
    free(data);
}

void
LDi_startstopstreaming(LDClient *const client, bool stopstreaming)
{
    client->shouldstopstreaming = stopstreaming;
    LDi_condsignal(&client->pollCond);
    LDi_condsignal(&client->streamCond);
}

static void
LDi_updatehandle(LDClient *const client, const int handle)
{
    LDi_wrlock(&client->clientLock);
    client->streamhandle = handle;
    LDi_wrunlock(&client->clientLock);
}

void
LDi_reinitializeconnection(LDClient *const client)
{
    if (client->streamhandle) {
        LDi_cancelread(client->streamhandle);
        client->streamhandle = 0;
    }
    LDi_condsignal(&client->pollCond);
    LDi_condsignal(&client->streamCond);
}

/*
 * as far as event stream parsers go, this is pretty basic.
 * : -> comment gets eaten
 * event:type -> type is remembered for the next data lines
 * data:line -> line is processed according to last seen event type
 */
static int
streamcallback(LDClient *const client, const char *line)
{
    LDi_wrlock(&client->clientLock);

    if (client->shouldstopstreaming) {
        free(client->databuffer); client->databuffer = NULL; client->eventname[0] = 0;
        LDi_wrunlock(&client->clientLock);
        return 1;
    }

    if (!line) {
        //should not happen from the networking side but is not fatal
        LDi_log(LD_LOG_ERROR, "streamcallback got NULL line");
    } else if (line[0] == ':') {
        //skip comment
    } else if (line[0] == 0) {
        if (client->eventname[0] == 0) {
            LDi_log(LD_LOG_WARNING, "streamcallback got dispatch but type was never set");
        } else if (strcmp(client->eventname, "ping") == 0) {
            LDi_wrunlock(&client->clientLock);
            onstreameventping(client);
            LDi_wrlock(&client->clientLock);
        } else if (client->databuffer == NULL) {
            LDi_log(LD_LOG_WARNING, "streamcallback got dispatch but data was never set");
        } else if (strcmp(client->eventname, "put") == 0) {
            LDi_wrunlock(&client->clientLock);
            LDi_onstreameventput(client, client->databuffer);
            LDi_wrlock(&client->clientLock);
        } else if (strcmp(client->eventname, "patch") == 0) {
            LDi_wrunlock(&client->clientLock);
            LDi_onstreameventpatch(client, client->databuffer);
            LDi_wrlock(&client->clientLock);
        } else if (strcmp(client->eventname, "delete") == 0) {
            LDi_wrunlock(&client->clientLock);
            LDi_onstreameventdelete(client, client->databuffer);
            LDi_wrlock(&client->clientLock);
        } else {
            LDi_log(LD_LOG_WARNING, "streamcallback unknown event name: %s", client->eventname);
        }

        free(client->databuffer); client->databuffer = NULL; client->eventname[0] = 0;
    } else if (strncmp(line, "data:", 5) == 0) {
        line += 5; line += line[0] == ' ';

        const bool nempty = client->databuffer != NULL;

        const size_t linesize = strlen(line);

        size_t currentsize = 0;
        if (nempty) { currentsize = strlen(client->databuffer); }

        client->databuffer = realloc(client->databuffer, linesize + currentsize + nempty + 1);

        if (nempty) { client->databuffer[currentsize] = '\n'; }

        memcpy(client->databuffer + currentsize + nempty, line, linesize);

        client->databuffer[currentsize + nempty + linesize] = 0;
    } else if (strncmp(line, "event:", 6) == 0) {
        line += 6; line += line[0] == ' ';

        if (snprintf(client->eventname, sizeof(client->eventname), "%s", line) < 0) {
            LDi_wrunlock(&client->clientLock);
            LDi_log(LD_LOG_CRITICAL, "snprintf failed in streamcallback type processing");
            return 1;
        }
    }

    LDi_wrunlock(&client->clientLock);

    return 0;
}

THREAD_RETURN
LDi_bgfeaturestreamer(void *const v)
{
    LDClient *const client = v;

    int retries = 0;
    while (true) {
        LDi_wrlock(&client->clientLock);

        if (client->status == LDStatusFailed || client->status == LDStatusShuttingdown) {
            LDi_log(LD_LOG_TRACE, "killing thread LDi_bgfeaturestreamer");
            client->threads--;
            if (!client->threads) { LDi_condsignal(&client->initCond); }
            LDi_wrunlock(&client->clientLock);
            return THREAD_RETURN_DEFAULT;
        }

        if (!client->shared->sharedConfig->streaming || client->offline || client->background) {
            LDi_wrunlock(&client->clientLock);
            int ms = 30000;
            LDi_mtxenter(&client->condMtx);
            LDi_condwait(&client->streamCond, &client->condMtx, ms);
            LDi_mtxleave(&client->condMtx);
            continue;
        }

        LDi_wrunlock(&client->clientLock);

        int response;
        /* this won't return until it disconnects */
        LDi_readstream(client,  &response, streamcallback, LDi_updatehandle);

        if (response == 401 || response == 403) {
            LDi_wrlock(&client->clientLock);
            LDi_updatestatus(client, LDStatusFailed);
            LDi_wrunlock(&client->clientLock);

            LDi_log(LD_LOG_ERROR, "mobile key not authorized, streaming failed");
            continue;
        } else if (response == -1) {
            LDi_rdlock(&client->clientLock);
            /* check if the stream was intentionally closed */
            if (client->streamhandle) {
                retries++;
            } else {
                retries = 0;
            }
            LDi_rdunlock(&client->clientLock);
        }

        if (retries) {
            unsigned int rng = 0;
            if (!LDi_random(&rng)) {
                LDi_log(LD_LOG_CRITICAL, "rng failed in bgeventsender");
            }

            int backoff = 1000 * pow(2, retries - 2);
            backoff += rng % backoff;
            if (backoff > 3600 * 1000) {
                backoff = 3600 * 1000;
                retries--; /* avoid excessive incrementing */
            }

            LDi_millisleep(backoff);
        }
    }
}
