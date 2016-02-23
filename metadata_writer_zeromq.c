/* Copyright (c) 2015, Celerway, Kristian Evensen <kristrev@celerway.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <zmq.h>
#include JSON_LOC
#include <getopt.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <getopt.h>

#include "lib/minmea.h"
#include "metadata_exporter.h"
#include "metadata_writer_zeromq.h"
#include "system_helpers.h"
#include "metadata_utils.h"
#include "metadata_exporter_log.h"

static json_object *md_zeromq_create_json_string(json_object *obj,
        const char *key, const char *value)
{
    struct json_object *obj_add = json_object_new_string(value);

    if (!obj_add)
        return NULL;
    
    json_object_object_add(obj, key, obj_add);
    return obj;
}

static json_object *md_zeromq_create_json_int(json_object *obj, const char *key,
        int value)
{
    struct json_object *obj_add = json_object_new_int(value);

    if (!obj_add)
        return NULL;
    
    json_object_object_add(obj, key, obj_add);
    return obj;
}

static json_object *md_zeromq_create_json_int64(json_object *obj,
        const char *key, int64_t value)
{
    struct json_object *obj_add = json_object_new_int64(value);

    if (!obj_add)
        return NULL;
    
    json_object_object_add(obj, key, obj_add);
    return obj;
}

static json_object* md_zeromq_create_json_gps(struct md_writer_zeromq *mwz,
                                              struct md_gps_event *mge)
{
    struct json_object *obj = NULL, *obj_add = NULL;

    if (!(obj = json_object_new_object()))
        return NULL;

    if (!(obj_add = json_object_new_int(mge->sequence))) {
        json_object_put(obj);
        return NULL;
    }
    json_object_object_add(obj, "seq", obj_add);

    if (!(obj_add = json_object_new_int64(mge->tstamp_tv.tv_sec))) {
        json_object_put(obj);
        return NULL;
    }
    json_object_object_add(obj, "tstamp", obj_add);

    if (!(obj_add = json_object_new_double(mge->latitude))) {
        json_object_put(obj);
        return NULL;
    }
    json_object_object_add(obj, "lat", obj_add);

    if (!(obj_add = json_object_new_double(mge->longitude))) {
        json_object_put(obj);
        return NULL;
    }
    json_object_object_add(obj, "lng", obj_add);

    if (mge->speed) {
        obj_add = json_object_new_double(mge->speed);

        if (obj_add == NULL) {
            json_object_put(obj);
            return NULL;
        }
        json_object_object_add(obj, "speed", obj_add);
    }

    if (mge->altitude) {
        obj_add = json_object_new_double(mge->altitude);

        if (obj_add == NULL) {
            json_object_put(obj);
            return NULL;
        }
        json_object_object_add(obj, "altitude", obj_add);
    }

    if (mge->satellites_tracked) {
        obj_add = json_object_new_int(mge->satellites_tracked);

        if (obj_add == NULL) {
            json_object_put(obj);
            return NULL;
        }
        json_object_object_add(obj, "num_sat", obj_add);
    }

    if (mge->nmea_raw) {
        if (!(obj_add = json_object_new_string(mge->nmea_raw))) {
            json_object_put(obj);
            return NULL;
        }
        json_object_object_add(obj, "nmea_raw", obj_add);
    }
    
    return obj;
}

static void md_zeromq_handle_gps(struct md_writer_zeromq *mwz,
                                 struct md_gps_event *mge)
{
    char topic[8192];
    struct json_object *gps_obj = md_zeromq_create_json_gps(mwz, mge);
    int retval;

    if (gps_obj == NULL) {
        META_PRINT(mwz->parent->logfile, "Failed to create GPS ZMQ JSON\n");
        return;
    }

    retval = snprintf(topic, sizeof(topic), "MONROE.META.DEVICE.GPS %s", json_object_to_json_string_ext(gps_obj, JSON_C_TO_STRING_PLAIN));

    if (retval < sizeof(topic)) {
        zmq_send(mwz->zmq_publisher, topic, strlen(topic), 0);
    }
    json_object_put(gps_obj);
}

static void md_zeromq_handle_munin(struct md_writer_zeromq *mwz,
                                   struct md_munin_event *mge)
{
    char topic[8192];
    int retval;

    json_object_object_foreach(mge->json_blob, key, val) {
        retval = snprintf(topic, sizeof(topic), "MONROE.META.NODE.SENSOR.%s %s", key, json_object_to_json_string_ext(val, JSON_C_TO_STRING_PLAIN));
        if (retval < sizeof(topic)) {
            zmq_send(mwz->zmq_publisher, topic, strlen(topic), 0);
        }
    }
}

static json_object* md_zeromq_create_json_modem_default(struct md_writer_zeromq *mwz,
                                                        struct md_conn_event *mce)
{
    struct json_object *obj = NULL, *obj_add = NULL;

    if (!(obj = json_object_new_object()))
        return NULL;

    if (!(obj_add = json_object_new_int(mce->sequence))) {
        json_object_put(obj);
        return NULL;
    }
    json_object_object_add(obj, "seq", obj_add);

    if (!(obj_add = json_object_new_int64(mce->tstamp))) {
        json_object_put(obj);
        return NULL;
    }
    json_object_object_add(obj, "tstamp", obj_add);

    if (!(obj_add = json_object_new_string(mce->interface_id))) {
        json_object_put(obj);
        return NULL;
    }
    json_object_object_add(obj, "interface_id", obj_add);

    if (!(obj_add = json_object_new_string(mce->interface_name))) {
        json_object_put(obj);
        return NULL;
    }
    json_object_object_add(obj, "interface_name", obj_add);

    if (!(obj_add = json_object_new_int(mce->network_provider))) {
        json_object_put(obj);
        return NULL;
    }
    json_object_object_add(obj, "operator", obj_add);

    return obj;
}

static void md_zeromq_handle_conn(struct md_writer_zeromq *mwz,
                                   struct md_conn_event *mce)
{
    struct json_object *json_obj, *obj_add;
    char event_str_cpy[EVENT_STR_LEN];
    size_t event_str_len;
    uint8_t mode;
    char topic[8192];
    int retval;

    if ((mce->event_param != CONN_EVENT_MODE_CHANGE &&
         mce->event_param != CONN_EVENT_META_UPDATE) ||
         mce->interface_type != INTERFACE_MODEM)
        return;

    if (mce->event_param == CONN_EVENT_MODE_CHANGE) {
        mode = mce->event_value;
    } else {
        event_str_len = strlen(mce->event_value_str);

        if (event_str_len >= EVENT_STR_LEN) {
            META_PRINT(mwz->parent->logfile, "Event string too long\n");
            return;
        }

        memcpy(event_str_cpy, mce->event_value_str, event_str_len);
        event_str_cpy[event_str_len] = '\0';
        mode = metadata_utils_get_csv_pos(event_str_cpy, 2);
    }
    
    json_obj = md_zeromq_create_json_modem_default(mwz, mce);

    if (!(obj_add = json_object_new_int(mode))) {
        json_object_put(json_obj);
        return;
    }
    json_object_object_add(json_obj, "mode", obj_add);

    if (mce->event_param == CONN_EVENT_META_UPDATE &&
        mce->signal_strength != -127) {
        obj_add = json_object_new_int(mce->signal_strength);

        if (!obj_add) {
            json_object_put(json_obj);
            return;
        } else {
            json_object_object_add(json_obj, "signal_strength", obj_add);
        }
    }

    if (mce->event_param == CONN_EVENT_META_UPDATE)
        retval = snprintf(topic, sizeof(topic), "MONROE.META.DEVICE.MODEM.%s.UPDATE %s",
                mce->interface_name,
                json_object_to_json_string_ext(json_obj, JSON_C_TO_STRING_PLAIN));
    else
        retval = snprintf(topic, sizeof(topic), "MONROE.META.DEVICE.MODEM.%s.MODE_CHANGE %s",
                mce->interface_name,
                json_object_to_json_string_ext(json_obj, JSON_C_TO_STRING_PLAIN));

    if (retval < sizeof(topic)) {
        zmq_send(mwz->zmq_publisher, topic, strlen(topic), 0);
    }
    json_object_put(json_obj);
}

static json_object *md_zeromq_create_iface_json(struct md_iface_event *mie)
{
    struct json_object *obj = NULL;

    if (!(obj = json_object_new_object()))
        return NULL;

    if (!md_zeromq_create_json_int(obj, "seq", mie->sequence) ||
        !md_zeromq_create_json_int64(obj, "tstamp", mie->tstamp) ||
        !md_zeromq_create_json_string(obj, "iccid", mie->iccid) ||
        !md_zeromq_create_json_string(obj, "imsi", mie->imsi) ||
        !md_zeromq_create_json_string(obj, "imei", mie->imei)) {
        json_object_put(obj);
        return NULL;
    }

    if (mie->isp_name && !md_zeromq_create_json_string(obj, "isp_name",
                mie->isp_name)) {
        json_object_put(obj);
        return NULL;
    }

    if (mie->ip_addr && !md_zeromq_create_json_string(obj, "ip_addr",
                mie->ip_addr)) {
        json_object_put(obj);
        return NULL;
    }

    if (mie->internal_ip_addr && !md_zeromq_create_json_string(obj,
                "internal_ip_addr", mie->internal_ip_addr)) {
        json_object_put(obj);
        return NULL;
    }

    if (mie->imsi_mccmnc &&
            !md_zeromq_create_json_int64(obj, "imsi_mccmnc", mie->imsi_mccmnc)) {
        json_object_put(obj);
        return NULL;
    }

    if (mie->nw_mccmnc &&
            !md_zeromq_create_json_int64(obj, "nw_mccmnc", mie->nw_mccmnc)) {
        json_object_put(obj);
        return NULL;
    }

    if ((mie->cid > -1 && mie->lac > -1) &&
            (!md_zeromq_create_json_int(obj, "lac", mie->lac) ||
             !md_zeromq_create_json_int(obj, "cid", mie->cid))) {
        json_object_put(obj);
        return NULL;
    }

    if (mie->rscp != META_IFACE_INVALID &&
            !md_zeromq_create_json_int(obj, "rscp", mie->rscp)) {
        json_object_put(obj);
        return NULL;
    }

    if (mie->lte_rsrp != META_IFACE_INVALID &&
            !md_zeromq_create_json_int(obj, "lte_rsrp", mie->rscp)) {
        json_object_put(obj);
        return NULL;
    }

    if (mie->lte_freq &&
            !md_zeromq_create_json_int(obj, "lte_freq", mie->lte_freq)) {
        json_object_put(obj);
        return NULL;
    }

    if (mie->rssi != META_IFACE_INVALID &&
            !md_zeromq_create_json_int(obj, "rssi", mie->rssi)) {
        json_object_put(obj);
        return NULL;
    }

    if (mie->ecio != META_IFACE_INVALID &&
            !md_zeromq_create_json_int(obj, "ecio", mie->ecio)) {
        json_object_put(obj);
        return NULL;
    }

    if (mie->lte_rssi != META_IFACE_INVALID &&
            !md_zeromq_create_json_int(obj, "lte_rssi", mie->lte_rssi)) {
        json_object_put(obj);
        return NULL;
    }

    if (mie->lte_rsrq != META_IFACE_INVALID &&
            !md_zeromq_create_json_int(obj, "lte_rsrq", mie->lte_rsrq)) {
        json_object_put(obj);
        return NULL;
    }

    if (mie->device_mode && !md_zeromq_create_json_int(obj, "device_mode",
                mie->device_mode)) {
        json_object_put(obj);
        return NULL;
    }

    if (mie->device_submode && !md_zeromq_create_json_int(obj, "device_submode",
                mie->device_submode)) {
        json_object_put(obj);
        return NULL;
    }

    if (mie->lte_band && !md_zeromq_create_json_int(obj, "lte_band",
                mie->lte_band)) {
        json_object_put(obj);
        return NULL;
    }

    if (mie->device_state && !md_zeromq_create_json_int(obj, "device_state",
                mie->device_state)) {
        json_object_put(obj);
        return NULL;
    }

    if (mie->lte_pci != 0xFFFF &&
            !md_zeromq_create_json_int(obj, "lte_pci", mie->lte_pci)) {
        json_object_put(obj);
        return NULL;
    }

    if (mie->enodeb_id >= 0 &&
            !md_zeromq_create_json_int(obj, "enodeb_id", mie->enodeb_id)) {
        json_object_put(obj);
        return NULL;
    }

    return obj;
}

static void md_zeromq_handle_iface(struct md_writer_zeromq *mwz,
                                   struct md_iface_event *mie)
{
    struct json_object *json_obj =  md_zeromq_create_iface_json(mie);
    char topic[8192] = {0};
    int retval = 0;

    if (json_obj == NULL)
        return;

    //Switch on topic
    switch (mie->event_param) {
    case IFACE_EVENT_REGISTER:
        retval = snprintf(topic, sizeof(topic), "MONROE.META.DEVICE.MODEM.REGISTER %s",
                json_object_to_json_string_ext(json_obj, JSON_C_TO_STRING_PLAIN));
        break;
    case IFACE_EVENT_CONNECT:
        retval = snprintf(topic, sizeof(topic), "MONROE.META.DEVICE.MODEM.CONNECT %s",
                json_object_to_json_string_ext(json_obj, JSON_C_TO_STRING_PLAIN));
        break;
    case IFACE_EVENT_MODE_CHANGE:
        retval = snprintf(topic, sizeof(topic), "MONROE.META.DEVICE.MODEM.MODE %s",
                json_object_to_json_string_ext(json_obj, JSON_C_TO_STRING_PLAIN));
        break;
    case IFACE_EVENT_SIGNAL_CHANGE:
        retval = snprintf(topic, sizeof(topic), "MONROE.META.DEVICE.MODEM.SIGNAL %s",
                json_object_to_json_string_ext(json_obj, JSON_C_TO_STRING_PLAIN));
        break;
    case IFACE_EVENT_LTE_BAND_CHANGE:
        retval = snprintf(topic, sizeof(topic), "MONROE.META.DEVICE.MODEM.LTE_BAND %s",
                json_object_to_json_string_ext(json_obj, JSON_C_TO_STRING_PLAIN));
        break;
    case IFACE_EVENT_UPDATE:
        retval = snprintf(topic, sizeof(topic), "MONROE.META.DEVICE.MODEM.UPDATE %s",
                json_object_to_json_string_ext(json_obj, JSON_C_TO_STRING_PLAIN));
        break;
    default:
        json_object_put(json_obj);
        return;
    }

    if (retval >= sizeof(topic)) {
        json_object_put(json_obj);
        return;
    }

    zmq_send(mwz->zmq_publisher, topic, strlen(topic), 0);
    json_object_put(json_obj);
}

static void md_zeromq_handle(struct md_writer *writer, struct md_event *event)
{
    struct md_writer_zeromq *mwz = (struct md_writer_zeromq*) writer;

    switch (event->md_type) {
    case META_TYPE_POS:
        md_zeromq_handle_gps(mwz, (struct md_gps_event*) event);
        break;
    case META_TYPE_CONNECTION:
        md_zeromq_handle_conn(mwz, (struct md_conn_event*) event);
        break;
    case META_TYPE_MUNIN:
        md_zeromq_handle_munin(mwz, (struct md_munin_event*) event);
        break;
    case META_TYPE_INTERFACE:
        md_zeromq_handle_iface(mwz, (struct md_iface_event*) event);
        break;
    default:
        break;
    }
}

static uint8_t md_zeromq_config(struct md_writer_zeromq *mwz,
                                const char *address,
                                uint16_t port)
{
    //INET6_ADDRSTRLEN is 46 (max length of ipv6 + trailing 0), 5 is port, 6 is
    //protocol (we right now only support TCP)
    char zmq_addr[INET6_ADDRSTRLEN + 5 + 6];
    int32_t retval;

    snprintf(zmq_addr, sizeof(zmq_addr), "tcp://%s:%d", address, port);

    if ((mwz->zmq_context = zmq_ctx_new()) == NULL)
        return RETVAL_FAILURE;

    if ((mwz->zmq_publisher = zmq_socket(mwz->zmq_context, ZMQ_PUB)) == NULL)
        return RETVAL_FAILURE;

    if ((retval = zmq_bind(mwz->zmq_publisher, zmq_addr)) != 0) {
        META_PRINT(mwz->parent->logfile, "zmq_bind failed (%d): %s\n", errno,
                zmq_strerror(errno));
        return RETVAL_FAILURE;
    }

    META_PRINT(mwz->parent->logfile, "ZeroMQ init done\n");

    return RETVAL_SUCCESS;
}

static int32_t md_zeromq_init(void *ptr, int argc, char *argv[])
{
    struct md_writer_zeromq *mwz = ptr;
    const char *address = NULL;
    uint16_t port = 0;
    int c, option_index = 0;

    static struct option zmq_options[] = {
        {"zmq_address",         required_argument,  0,  0},
        {"zmq_port",            required_argument,  0,  0},
        {0,                                     0,  0,  0}};

    while (1) {
        //No permuting of array here as well
        c = getopt_long_only(argc, argv, "--", zmq_options, &option_index);


        if (c == -1)
            break;
        else if (c)
            continue;
        
        if (!strcmp(zmq_options[option_index].name, "zmq_address"))
            address = optarg;
        else if (!strcmp(zmq_options[option_index].name, "zmq_port"))
            port = (uint16_t) atoi(optarg);
    }

    if (address == NULL || port == 0) {
        META_PRINT(mwz->parent->logfile, "Missing required ZeroMQ argument\n");
        return RETVAL_FAILURE;
    }

    if (system_helpers_check_address(address)) {
        META_PRINT(mwz->parent->logfile, "Error in ZeroMQ address\n");
        return RETVAL_FAILURE;
    }

    return md_zeromq_config(mwz, address, port);
}

static void md_zeromq_usage()
{
    fprintf(stderr, "ZeroMQ writer:\n");
    fprintf(stderr, "--zmq_address: address used by publisher (r)\n");
    fprintf(stderr, "--zmq_port: port used by publisher (r)\n");
}

void md_zeromq_setup(struct md_exporter *mde, struct md_writer_zeromq* mwz) {
    mwz->parent = mde;
    mwz->usage = md_zeromq_usage;
    mwz->init = md_zeromq_init;
    mwz->handle = md_zeromq_handle;
}

