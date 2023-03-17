/*
 * Copyright (c) 2018 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "syscfg/syscfg.h"
#define MESH_LOG_MODULE BLE_MESH_SETTINGS_LOG

#if (MYNEWT_VAL(BLE_MESH_SETTINGS) && MYNEWT_VAL(BLE_MESH))

#include "mesh/mesh.h"
#include "mesh/glue.h"
#include "net.h"
#include "crypto.h"
#include "transport.h"
#include "access.h"
#include "foundation.h"
#include "proxy.h"
#include "settings.h"
#include "nodes.h"
#include "wm_ble_mesh_store.h"

/**store with binary format*/
#define BASE64_ENCODE_VALUE 0

/* Tracking of what storage changes are pending for App and Net Keys. We
 * track this in a separate array here instead of within the respective
 * bt_mesh_app_key and bt_mesh_subnet structs themselves, since once a key
 * gets deleted its struct becomes invalid and may be reused for other keys.
 */
static struct key_update {
    u16_t key_idx: 12,   /* AppKey or NetKey Index */
          valid: 1,      /* 1 if this entry is valid, 0 if not */
          app_key: 1,    /* 1 if this is an AppKey, 0 if a NetKey */
          clear: 1;      /* 1 if key needs clearing, 0 if storing */
} key_updates[CONFIG_BT_MESH_APP_KEY_COUNT + CONFIG_BT_MESH_SUBNET_COUNT];

static struct k_delayed_work pending_store;

/* Mesh network storage information */
struct net_val {
    u16_t primary_addr;
    u8_t  dev_key[16];
} __packed;

/* Sequence number storage */
struct seq_val {
    u8_t val[3];
} __packed;

/* Heartbeat Publication storage */
struct hb_pub_val {
    u16_t dst;
    u8_t  period;
    u8_t  ttl;
    u16_t feat;
    u16_t net_idx: 12,
          indefinite: 1;
};

/* Miscelaneous configuration server model states */
struct cfg_val {
    u8_t net_transmit;
    u8_t relay;
    u8_t relay_retransmit;
    u8_t beacon;
    u8_t gatt_proxy;
    u8_t frnd;
    u8_t default_ttl;
};

/* IV Index & IV Update storage */
struct iv_val {
    u32_t iv_index;
    u8_t  iv_update: 1,
          iv_duration: 7;
} __packed;

/* Replay Protection List storage */
struct rpl_val {
    u32_t seq: 24,
          old_iv: 1;
};

/* NetKey storage information */
struct net_key_val {
    u8_t kr_flag: 1,
         kr_phase: 7;
    u8_t val[2][16];
} __packed;

/* AppKey storage information */
struct app_key_val {
    u16_t net_idx;
    bool  updated;
    u8_t  val[2][16];
} __packed;

struct mod_pub_val {
    u16_t addr;
    u16_t key;
    u8_t  ttl;
    u8_t  retransmit;
    u8_t  period;
    u8_t  period_div: 4,
          cred: 1;
};

/* Virtual Address information */
struct va_val {
    u16_t ref;
    u16_t addr;
    u8_t uuid[16];
} __packed;

/* Node storage information */
struct node_val {
    u16_t net_idx;
    u8_t  dev_key[16];
    u8_t  num_elem;
} __packed;

struct node_update {
    u16_t addr;
    bool clear;
};

#if MYNEWT_VAL(BLE_MESH_PROVISIONER)
static struct node_update node_updates[CONFIG_BT_MESH_NODE_COUNT];
#else
static struct node_update node_updates[0];
#endif

/* We need this so we don't overwrite app-hardcoded values in case FCB
 * contains a history of changes but then has a NULL at the end.
 */
static struct {
    bool valid;
    struct cfg_val cfg;
} stored_cfg;

#if BASE64_ENCODE_VALUE
static int net_set(int argc, char **argv, char *val)
{
    struct net_val net;
    int len, err;
    BT_DBG("val %s", val ? val : "(null)");

    if(!val) {
        bt_mesh_comp_unprovision();
        memset(bt_mesh.dev_key, 0, sizeof(bt_mesh.dev_key));
        return 0;
    }

    len = sizeof(net);
    err = settings_bytes_from_str(val, &net, &len);

    if(err) {
        BT_ERR("Failed to decode value %s (err %d)", val, err);
        return err;
    }

    if(len != sizeof(net)) {
        BT_ERR("Unexpected value length (%d != %zu)", len, sizeof(net));
        return -EINVAL;
    }

    memcpy(bt_mesh.dev_key, net.dev_key, sizeof(bt_mesh.dev_key));
    bt_mesh_comp_provision(net.primary_addr);
    BT_DBG("Provisioned with primary address 0x%04x", net.primary_addr);
    BT_DBG("Recovered DevKey %s", bt_hex(bt_mesh.dev_key, 16));
    return 0;
}

static int iv_set(int argc, char **argv, char *val)
{
    struct iv_val iv;
    int len, err;
    BT_DBG("val %s", val ? val : "(null)");

    if(!val) {
        bt_mesh.iv_index = 0U;
        atomic_clear_bit(bt_mesh.flags, BT_MESH_IVU_IN_PROGRESS);
        return 0;
    }

    len = sizeof(iv);
    err = settings_bytes_from_str(val, &iv, &len);

    if(err) {
        BT_ERR("Failed to decode value %s (err %d)", val, err);
        return err;
    }

    if(len != sizeof(iv)) {
        BT_ERR("Unexpected value length (%d != %zu)", len, sizeof(iv));
        return -EINVAL;
    }

    bt_mesh.iv_index = iv.iv_index;
    atomic_set_bit_to(bt_mesh.flags, BT_MESH_IVU_IN_PROGRESS, iv.iv_update);
    bt_mesh.ivu_duration = iv.iv_duration;
    BT_DBG("IV Index 0x%04x (IV Update Flag %u) duration %u hours",
           (unsigned) iv.iv_index, iv.iv_update, iv.iv_duration);
    return 0;
}

static int seq_set(int argc, char **argv, char *val)
{
    struct seq_val seq;
    int len, err;
    BT_DBG("val %s", val ? val : "(null)");

    if(!val) {
        bt_mesh.seq = 0;
        return 0;
    }

    len = sizeof(seq);
    err = settings_bytes_from_str(val, &seq, &len);

    if(err) {
        BT_ERR("Failed to decode value %s (err %d)", val, err);
        return err;
    }

    if(len != sizeof(seq)) {
        BT_ERR("Unexpected value length (%d != %zu)", len, sizeof(seq));
        return -EINVAL;
    }

    bt_mesh.seq = ((u32_t)seq.val[0] | ((u32_t)seq.val[1] << 8) |
                   ((u32_t)seq.val[2] << 16));

    if(CONFIG_BT_MESH_SEQ_STORE_RATE > 0) {
        /* Make sure we have a large enough sequence number. We
         * subtract 1 so that the first transmission causes a write
         * to the settings storage.
         */
        bt_mesh.seq += (CONFIG_BT_MESH_SEQ_STORE_RATE -
                        (bt_mesh.seq % CONFIG_BT_MESH_SEQ_STORE_RATE));
        bt_mesh.seq--;
    }

    BT_DBG("Sequence Number 0x%06x", bt_mesh.seq);
    return 0;
}

static struct bt_mesh_rpl *rpl_find(u16_t src)
{
    int i;

    for(i = 0; i < ARRAY_SIZE(bt_mesh.rpl); i++) {
        if(bt_mesh.rpl[i].src == src) {
            return &bt_mesh.rpl[i];
        }
    }

    return NULL;
}

static struct bt_mesh_rpl *rpl_alloc(u16_t src)
{
    int i;

    for(i = 0; i < ARRAY_SIZE(bt_mesh.rpl); i++) {
        if(!bt_mesh.rpl[i].src) {
            bt_mesh.rpl[i].src = src;
            return &bt_mesh.rpl[i];
        }
    }

    return NULL;
}

static int rpl_set(int argc, char **argv, char *val)
{
    struct bt_mesh_rpl *entry;
    struct rpl_val rpl;
    int len, err;
    u16_t src;

    if(argc < 1) {
        BT_ERR("Invalid argc (%d)", argc);
        return -ENOENT;
    }

    BT_DBG("argv[0] %s val %s", argv[0], val ? val : "(null)");
    src = strtol(argv[0], NULL, 16);
    entry = rpl_find(src);

    if(!val) {
        if(entry) {
            memset(entry, 0, sizeof(*entry));
        } else {
            BT_WARN("Unable to find RPL entry for 0x%04x", src);
        }

        return 0;
    }

    if(!entry) {
        entry = rpl_alloc(src);

        if(!entry) {
            BT_ERR("Unable to allocate RPL entry for 0x%04x", src);
            return -ENOMEM;
        }
    }

    len = sizeof(rpl);
    err = settings_bytes_from_str(val, &rpl, &len);

    if(err) {
        BT_ERR("Failed to decode value %s (err %d)", val, err);
        return err;
    }

    if(len != sizeof(rpl)) {
        BT_ERR("Unexpected value length (%d != %zu)", len, sizeof(rpl));
        return -EINVAL;
    }

    entry->seq = rpl.seq;
    entry->old_iv = rpl.old_iv;
    BT_DBG("RPL entry for 0x%04x: Seq 0x%06x old_iv %u", entry->src,
           (unsigned) entry->seq, entry->old_iv);
    return 0;
}

static int net_key_set(int argc, char **argv, char *val)
{
    struct bt_mesh_subnet *sub;
    struct net_key_val key;
    int len, i, err;
    u16_t net_idx;
    BT_DBG("argv[0] %s val %s", argv[0], val ? val : "(null)");
    net_idx = strtol(argv[0], NULL, 16);
    sub = bt_mesh_subnet_get(net_idx);

    if(!val) {
        if(!sub) {
            BT_ERR("No subnet with NetKeyIndex 0x%03x", net_idx);
            return -ENOENT;
        }

        BT_DBG("Deleting NetKeyIndex 0x%03x", net_idx);
        bt_mesh_subnet_del(sub, false);
        return 0;
    }

    len = sizeof(key);
    err = settings_bytes_from_str(val, &key, &len);

    if(err) {
        BT_ERR("Failed to decode value %s (err %d)", val, err);
        return err;
    }

    if(len != sizeof(key)) {
        BT_ERR("Unexpected value length (%d != %zu)", len, sizeof(key));
        return -EINVAL;
    }

    if(sub) {
        BT_DBG("Updating existing NetKeyIndex 0x%03x", net_idx);
        sub->kr_flag = key.kr_flag;
        sub->kr_phase = key.kr_phase;
        memcpy(sub->keys[0].net, &key.val[0], 16);
        memcpy(sub->keys[1].net, &key.val[1], 16);
        return 0;
    }

    for(i = 0; i < ARRAY_SIZE(bt_mesh.sub); i++) {
        if(bt_mesh.sub[i].net_idx == BT_MESH_KEY_UNUSED) {
            sub = &bt_mesh.sub[i];
            break;
        }
    }

    if(!sub) {
        BT_ERR("No space to allocate a new subnet");
        return -ENOMEM;
    }

    sub->net_idx = net_idx;
    sub->kr_flag = key.kr_flag;
    sub->kr_phase = key.kr_phase;
    memcpy(sub->keys[0].net, &key.val[0], 16);
    memcpy(sub->keys[1].net, &key.val[1], 16);
    BT_DBG("NetKeyIndex 0x%03x recovered from storage", net_idx);
    return 0;
}

static int app_key_set(int argc, char **argv, char *val)
{
    struct bt_mesh_app_key *app;
    struct app_key_val key;
    u16_t app_idx;
    int len, err;
    BT_DBG("argv[0] %s val %s", argv[0], val ? val : "(null)");
    app_idx = strtol(argv[0], NULL, 16);

    if(!val) {
        BT_DBG("Deleting AppKeyIndex 0x%03x", app_idx);
        app = bt_mesh_app_key_find(app_idx);

        if(app) {
            bt_mesh_app_key_del(app, false);
        }

        return 0;
    }

    len = sizeof(key);
    err = settings_bytes_from_str(val, &key, &len);

    if(err) {
        BT_ERR("Failed to decode value %s (err %d)", val, err);
        return err;
    }

    if(len != sizeof(key)) {
        BT_ERR("Unexpected value length (%d != %zu)", len, sizeof(key));
        return -EINVAL;
    }

    app = bt_mesh_app_key_find(app_idx);

    if(!app) {
        app = bt_mesh_app_key_alloc(app_idx);
    }

    if(!app) {
        BT_ERR("No space for a new app key");
        return -ENOMEM;
    }

    app->net_idx = key.net_idx;
    app->app_idx = app_idx;
    app->updated = key.updated;
    memcpy(app->keys[0].val, key.val[0], 16);
    memcpy(app->keys[1].val, key.val[1], 16);
    bt_mesh_app_id(app->keys[0].val, &app->keys[0].id);
    bt_mesh_app_id(app->keys[1].val, &app->keys[1].id);
    BT_DBG("AppKeyIndex 0x%03x recovered from storage", app_idx);
    return 0;
}

static int hb_pub_set(int argc, char **argv, char *val)
{
    struct bt_mesh_hb_pub *pub = bt_mesh_hb_pub_get();
    struct hb_pub_val hb_val;
    int len, err;
    BT_DBG("val %s", val ? val : "(null)");

    if(!pub) {
        return -ENOENT;
    }

    if(!val) {
        pub->dst = BT_MESH_ADDR_UNASSIGNED;
        pub->count = 0;
        pub->ttl = 0;
        pub->period = 0;
        pub->feat = 0;
        BT_DBG("Cleared heartbeat publication");
        return 0;
    }

    len = sizeof(hb_val);
    err = settings_bytes_from_str(val, &hb_val, &len);

    if(err) {
        BT_ERR("Failed to decode value %s (err %d)", val, err);
        return err;
    }

    if(len != sizeof(hb_val)) {
        BT_ERR("Unexpected value length (%d != %zu)", len,
               sizeof(hb_val));
        return -EINVAL;
    }

    pub->dst = hb_val.dst;
    pub->period = hb_val.period;
    pub->ttl = hb_val.ttl;
    pub->feat = hb_val.feat;
    pub->net_idx = hb_val.net_idx;

    if(hb_val.indefinite) {
        pub->count = 0xffff;
    } else {
        pub->count = 0;
    }

    BT_DBG("Restored heartbeat publication");
    return 0;
}

static int cfg_set(int argc, char **argv, char *val)
{
    struct bt_mesh_cfg_srv *cfg = bt_mesh_cfg_get();
    int len, err;
    BT_DBG("val %s", val ? val : "(null)");

    if(!cfg) {
        return -ENOENT;
    }

    if(!val) {
        stored_cfg.valid = false;
        BT_DBG("Cleared configuration state");
        return 0;
    }

    len = sizeof(stored_cfg.cfg);
    err = settings_bytes_from_str(val, &stored_cfg.cfg, &len);

    if(err) {
        BT_ERR("Failed to decode value %s (err %d)", val, err);
        return err;
    }

    if(len != sizeof(stored_cfg.cfg)) {
        BT_ERR("Unexpected value length (%d != %zu)", len,
               sizeof(stored_cfg.cfg));
        return -EINVAL;
    }

    stored_cfg.valid = true;
    BT_DBG("Restored configuration state");
    return 0;
}

static int mod_set_bind(struct bt_mesh_model *mod, char *val)
{
    int len, err, i;

    /* Start with empty array regardless of cleared or set value */
    for(i = 0; i < ARRAY_SIZE(mod->keys); i++) {
        mod->keys[i] = BT_MESH_KEY_UNUSED;
    }

    if(!val) {
        BT_DBG("Cleared bindings for model");
        return 0;
    }

    len = sizeof(mod->keys);
    err = settings_bytes_from_str(val, mod->keys, &len);

    if(err) {
        BT_ERR("Failed to decode value %s (err %d)", val, err);
        return -EINVAL;
    }

    BT_DBG("Decoded %u bound keys for model", len / sizeof(mod->keys[0]));
    return 0;
}

static int mod_set_sub(struct bt_mesh_model *mod, char *val)
{
    int len, err;
    /* Start with empty array regardless of cleared or set value */
    memset(mod->groups, 0, sizeof(mod->groups));

    if(!val) {
        BT_DBG("Cleared subscriptions for model");
        return 0;
    }

    len = sizeof(mod->groups);
    err = settings_bytes_from_str(val, mod->groups, &len);

    if(err) {
        BT_ERR("Failed to decode value %s (err %d)", val, err);
        return -EINVAL;
    }

    BT_DBG("Decoded %u subscribed group addresses for model",
           len / sizeof(mod->groups[0]));
    return 0;
}

static int mod_set_pub(struct bt_mesh_model *mod, char *val)
{
    struct mod_pub_val pub;
    int len, err;

    if(!mod->pub) {
        BT_WARN("Model has no publication context!");
        return -EINVAL;
    }

    if(!val) {
        mod->pub->addr = BT_MESH_ADDR_UNASSIGNED;
        mod->pub->key = 0;
        mod->pub->cred = 0;
        mod->pub->ttl = 0;
        mod->pub->period = 0;
        mod->pub->retransmit = 0;
        mod->pub->count = 0;
        BT_DBG("Cleared publication for model");
        return 0;
    }

    len = sizeof(pub);
    err = settings_bytes_from_str(val, &pub, &len);

    if(err) {
        BT_ERR("Failed to decode value %s (err %d)", val, err);
        return -EINVAL;
    }

    if(len != sizeof(pub)) {
        BT_ERR("Invalid length for model publication");
        return -EINVAL;
    }

    mod->pub->addr = pub.addr;
    mod->pub->key = pub.key;
    mod->pub->cred = pub.cred;
    mod->pub->ttl = pub.ttl;
    mod->pub->period = pub.period;
    mod->pub->retransmit = pub.retransmit;
    mod->pub->count = 0;
    BT_DBG("Restored model publication, dst 0x%04x app_idx 0x%03x",
           pub.addr, pub.key);
    return 0;
}

static int mod_set(bool vnd, int argc, char **argv, char *val)
{
    struct bt_mesh_model *mod;
    u8_t elem_idx, mod_idx;
    u16_t mod_key;

    if(argc < 2) {
        BT_ERR("Too small argc (%d)", argc);
        return -ENOENT;
    }

    mod_key = strtol(argv[0], NULL, 16);
    elem_idx = mod_key >> 8;
    mod_idx = mod_key;
    BT_DBG("Decoded mod_key 0x%04x as elem_idx %u mod_idx %u",
           mod_key, elem_idx, mod_idx);
    mod = bt_mesh_model_get(vnd, elem_idx, mod_idx);

    if(!mod) {
        BT_ERR("Failed to get model for elem_idx %u mod_idx %u",
               elem_idx, mod_idx);
        return -ENOENT;
    }

    if(!strcmp(argv[1], "bind")) {
        return mod_set_bind(mod, val);
    }

    if(!strcmp(argv[1], "sub")) {
        return mod_set_sub(mod, val);
    }

    if(!strcmp(argv[1], "pub")) {
        return mod_set_pub(mod, val);
    }

    if(!strcmp(argv[1], "data")) {
        mod->flags |= BT_MESH_MOD_DATA_PRESENT;

        if(mod->cb && mod->cb->settings_set) {
            return mod->cb->settings_set(mod, val);
        }
    }

    BT_WARN("Unknown module key %s", argv[1]);
    return -ENOENT;
}

static int sig_mod_set(int argc, char **argv, char *val)
{
    return mod_set(false, argc, argv, val);
}

static int vnd_mod_set(int argc, char **argv, char *val)
{
    return mod_set(true, argc, argv, val);
}

#if CONFIG_BT_MESH_LABEL_COUNT > 0
static int va_set(int argc, char **argv, char *val)
{
    struct va_val va;
    struct label *lab;
    u16_t index;
    int len, err;

    if(argc < 1) {
        BT_ERR("Insufficient number of arguments");
        return -ENOENT;
    }

    index = strtol(argv[0], NULL, 16);

    if(val == NULL) {
        BT_WARN("Mesh Virtual Address length = 0");
        return 0;
    }

    err = settings_bytes_from_str(val, &va, &len);

    if(err) {
        BT_ERR("Failed to decode value %s (err %d)", val, err);
        return -EINVAL;
    }

    if(len != sizeof(struct va_val)) {
        BT_ERR("Invalid length for virtual address");
        return -EINVAL;
    }

    if(va.ref == 0) {
        BT_WARN("Ignore Mesh Virtual Address ref = 0");
        return 0;
    }

    lab = get_label(index);

    if(lab == NULL) {
        BT_WARN("Out of labels buffers");
        return -ENOBUFS;
    }

    memcpy(lab->uuid, va.uuid, 16);
    lab->addr = va.addr;
    lab->ref = va.ref;
    BT_DBG("Restored Virtual Address, addr 0x%04x ref 0x%04x",
           lab->addr, lab->ref);
    return 0;
}
#endif

#if MYNEWT_VAL(BLE_MESH_PROVISIONER)
static int node_set(int argc, char **argv, char *str)
{
    struct bt_mesh_node *node;
    struct node_val val;
    u16_t addr;
    int len, err;

    if(argc < 1) {
        BT_ERR("Insufficient number of arguments");
        return -ENOENT;
    }

    addr = strtol(argv[0], NULL, 16);

    if(str == NULL) {
        BT_DBG("val (null)");
        BT_DBG("Deleting node 0x%04x", addr);
        node = bt_mesh_node_find(addr);

        if(node) {
            bt_mesh_node_del(node, false);
        }

        return 0;
    }

    err = settings_bytes_from_str(str, &val, &len);

    if(err) {
        BT_ERR("Failed to decode value %s (err %d)", val, err);
        return -EINVAL;
    }

    if(len != sizeof(struct node_val)) {
        BT_ERR("Invalid length for node_val");
        return -EINVAL;
    }

    node = bt_mesh_node_find(addr);

    if(!node) {
        node = bt_mesh_node_alloc(addr, val.num_elem, val.net_idx);
    }

    if(!node) {
        BT_ERR("No space for a new node");
        return -ENOMEM;
    }

    memcpy(node->dev_key, &val.dev_key, 16);
    BT_DBG("Node 0x%04x recovered from storage", addr);
    return 0;
}
#endif

const struct mesh_setting {
    const char *name;
    int (*func)(int argc, char **argv, char *val);
} settings[] = {
    { "Net", net_set },
    { "IV", iv_set },
    { "Seq", seq_set },
    { "RPL", rpl_set },
    { "NetKey", net_key_set },
    { "AppKey", app_key_set },
    { "HBPub", hb_pub_set },
    { "Cfg", cfg_set },
    { "s", sig_mod_set },
    { "v", vnd_mod_set },
#if CONFIG_BT_MESH_LABEL_COUNT > 0
    { "Va", va_set },
#endif
#if MYNEWT_VAL(BLE_MESH_PROVISIONER)
    { "Node", node_set },
#endif
};

static int mesh_set(int argc, char **argv, char *val)
{
    int i;

    if(argc < 1) {
        BT_ERR("Insufficient number of arguments");
        return -EINVAL;
    }

    BT_DBG("argv[0] %s val %s", argv[0], val ? val : "(null)");

    for(i = 0; i < ARRAY_SIZE(settings); i++) {
        if(!strcmp(settings[i].name, argv[0])) {
            argc--;
            argv++;
            return settings[i].func(argc, argv, val);
        }
    }

    BT_WARN("No matching handler for key %s", argv[0]);
    return -ENOENT;
}


#else

static int role_set_bin(const char *key, uint8_t *value, int value_len)
{
    uint8_t role = value[0];
    BT_DBG("key:%s, val %s", key, bt_hex(value, value_len));

    if(role == 1) {
        atomic_set_bit(bt_mesh.flags, BT_MESH_NODE);
    } else if(role == 2) {
        atomic_set_bit(bt_mesh.flags, BT_MESH_PROVISIONER);
    }

    return 0;
}

static int net_set_bin(const char *key, uint8_t *value, int value_len)
{
    struct net_val net;
    BT_DBG("key:%s, val %s", key, bt_hex(value, value_len));

    if(value_len == 0) {
        bt_mesh_comp_unprovision();
        memset(bt_mesh.dev_key, 0, sizeof(bt_mesh.dev_key));
        return 0;
    }

    if(value_len != sizeof(net)) {
        BT_ERR("Unexpected value length (%d != %zu)", value_len, sizeof(net));
        return -EINVAL;
    }

    memcpy(&net, value, value_len);
    memcpy(bt_mesh.dev_key, net.dev_key, sizeof(bt_mesh.dev_key));
    bt_mesh_comp_provision(net.primary_addr);
    BT_DBG("Provisioned with primary address 0x%04x", net.primary_addr);
    BT_DBG("Recovered DevKey %s", bt_hex(bt_mesh.dev_key, 16));
    return 0;
}

static int iv_set_bin(const char *key, uint8_t *value, int value_len)
{
    struct iv_val iv;
    BT_DBG("key:%s, val %s", key, bt_hex(value, value_len));

    if(value_len == 0) {
        bt_mesh.iv_index = 0U;
        atomic_clear_bit(bt_mesh.flags, BT_MESH_IVU_IN_PROGRESS);
        return 0;
    }

    if(value_len != sizeof(iv)) {
        BT_ERR("Unexpected value length (%d != %zu)", value_len, sizeof(iv));
        return -EINVAL;
    }

    memcpy(&iv, value, value_len);
    bt_mesh.iv_index = iv.iv_index;
    atomic_set_bit_to(bt_mesh.flags, BT_MESH_IVU_IN_PROGRESS, iv.iv_update);
    bt_mesh.ivu_duration = iv.iv_duration;
    BT_DBG("IV Index 0x%04x (IV Update Flag %u) duration %u hours",
           (unsigned) iv.iv_index, iv.iv_update, iv.iv_duration);
    return 0;
}
static int seq_set_bin(const char *key, uint8_t *value, int value_len)
{
    struct seq_val seq;
    BT_DBG("key:%s, val %s", key, bt_hex(value, value_len));

    if(value_len == 0) {
        bt_mesh.seq = 0;
        return 0;
    }

    if(value_len != sizeof(seq)) {
        BT_ERR("Unexpected value length (%d != %zu)", value_len, sizeof(seq));
        return -EINVAL;
    }

    memcpy(&seq, value, value_len);
    bt_mesh.seq = ((u32_t)seq.val[0] | ((u32_t)seq.val[1] << 8) |
                   ((u32_t)seq.val[2] << 16));

    if(CONFIG_BT_MESH_SEQ_STORE_RATE > 0) {
        /* Make sure we have a large enough sequence number. We
         * subtract 1 so that the first transmission causes a write
         * to the settings storage.
         */
        bt_mesh.seq += (CONFIG_BT_MESH_SEQ_STORE_RATE -
                        (bt_mesh.seq % CONFIG_BT_MESH_SEQ_STORE_RATE));
        bt_mesh.seq--;
    }

    BT_DBG("Sequence Number 0x%06x", bt_mesh.seq);
    return 0;
}

static struct bt_mesh_rpl *rpl_find(u16_t src)
{
    int i;

    for(i = 0; i < ARRAY_SIZE(bt_mesh.rpl); i++) {
        if(bt_mesh.rpl[i].src == src) {
            return &bt_mesh.rpl[i];
        }
    }

    return NULL;
}

static struct bt_mesh_rpl *rpl_alloc(u16_t src)
{
    int i;

    for(i = 0; i < ARRAY_SIZE(bt_mesh.rpl); i++) {
        if(!bt_mesh.rpl[i].src) {
            bt_mesh.rpl[i].src = src;
            return &bt_mesh.rpl[i];
        }
    }

    return NULL;
}

static int rpl_set_bin(const char *key, uint8_t *value, int value_len)
{
    struct bt_mesh_rpl *entry;
    struct rpl_val rpl;
    u16_t src;
    BT_DBG("key %s val %s", key, bt_hex(value, value_len));
    /*skip "RPL/", 4 BYTES*/
    src = strtol(key + 4, NULL, 16);
    entry = rpl_find(src);

    if(value_len == 0) {
        if(entry) {
            memset(entry, 0, sizeof(*entry));
        } else {
            BT_WARN("Unable to find RPL entry for 0x%04x", src);
        }

        return 0;
    }

    if(!entry) {
        entry = rpl_alloc(src);

        if(!entry) {
            BT_ERR("Unable to allocate RPL entry for 0x%04x", src);
            return -ENOMEM;
        }
    }

    if(value_len != sizeof(rpl)) {
        BT_ERR("Unexpected value length (%d != %zu)", value_len, sizeof(rpl));
        return -EINVAL;
    }

    memcpy(&rpl, value, value_len);
    entry->seq = rpl.seq;
    entry->old_iv = rpl.old_iv;
    BT_DBG("RPL entry for 0x%04x: Seq 0x%06x old_iv %u", entry->src,
           (unsigned) entry->seq, entry->old_iv);
    return 0;
}

static int net_key_set_bin(const char *key_value, uint8_t *value, int value_len)
{
    struct bt_mesh_subnet *sub;
    struct net_key_val key;
    int i;
    u16_t net_idx;
    BT_DBG("key %s val %s", key_value, bt_hex(value, value_len));
    /*skip  Netkey/ : 7 bytes*/
    net_idx = strtol(key_value + 7, NULL, 16);
    sub = bt_mesh_subnet_get(net_idx);

    if(value_len == 0) {
        if(!sub) {
            BT_ERR("No subnet with NetKeyIndex 0x%03x", net_idx);
            return -ENOENT;
        }

        BT_DBG("Deleting NetKeyIndex 0x%03x", net_idx);
        bt_mesh_subnet_del(sub, false);
        return 0;
    }

    if(value_len != sizeof(key)) {
        BT_ERR("Unexpected value length (%d != %zu)", value_len, sizeof(key));
        return -EINVAL;
    }

    memcpy(&key, value, value_len);

    if(sub) {
        BT_DBG("Updating existing NetKeyIndex 0x%03x", net_idx);
        sub->kr_flag = key.kr_flag;
        sub->kr_phase = key.kr_phase;
        memcpy(sub->keys[0].net, &key.val[0], 16);
        memcpy(sub->keys[1].net, &key.val[1], 16);
        return 0;
    }

    for(i = 0; i < ARRAY_SIZE(bt_mesh.sub); i++) {
        if(bt_mesh.sub[i].net_idx == BT_MESH_KEY_UNUSED) {
            sub = &bt_mesh.sub[i];
            break;
        }
    }

    if(!sub) {
        BT_ERR("No space to allocate a new subnet");
        return -ENOMEM;
    }

    sub->net_idx = net_idx;
    sub->kr_flag = key.kr_flag;
    sub->kr_phase = key.kr_phase;
    memcpy(sub->keys[0].net, &key.val[0], 16);
    memcpy(sub->keys[1].net, &key.val[1], 16);
    BT_DBG("NetKeyIndex 0x%03x recovered from storage", net_idx);
    return 0;
}

static int app_key_set_bin(const char *key_value, uint8_t *value, int value_len)
{
    struct bt_mesh_app_key *app;
    struct app_key_val key;
    u16_t app_idx;
    BT_DBG("key %s val %s", key_value, bt_hex(value, value_len));
    /*skip  Appkey/ : 7 bytes*/
    app_idx = strtol(key_value + 7, NULL, 16);

    if(value_len == 0) {
        BT_DBG("Deleting AppKeyIndex 0x%03x", app_idx);
        app = bt_mesh_app_key_find(app_idx);

        if(app) {
            bt_mesh_app_key_del(app, false);
        }

        return 0;
    }

    if(value_len != sizeof(key)) {
        BT_ERR("Unexpected value length (%d != %zu)", value_len, sizeof(key));
        return -EINVAL;
    }

    memcpy(&key, value, value_len);
    app = bt_mesh_app_key_find(app_idx);

    if(!app) {
        app = bt_mesh_app_key_alloc(app_idx);
    }

    if(!app) {
        BT_ERR("No space for a new app key");
        return -ENOMEM;
    }

    app->net_idx = key.net_idx;
    app->app_idx = app_idx;
    app->updated = key.updated;
    memcpy(app->keys[0].val, key.val[0], 16);
    memcpy(app->keys[1].val, key.val[1], 16);
    bt_mesh_app_id(app->keys[0].val, &app->keys[0].id);
    bt_mesh_app_id(app->keys[1].val, &app->keys[1].id);
    BT_DBG("AppKeyIndex 0x%03x recovered from storage", app_idx);
    return 0;
}

static int hb_pub_set_bin(const char *key_value, uint8_t *value, int value_len)
{
    struct bt_mesh_hb_pub *pub = bt_mesh_hb_pub_get();
    struct hb_pub_val hb_val;
    BT_DBG("key %s val %s", key_value, bt_hex(value, value_len));

    if(!pub) {
        return -ENOENT;
    }

    if(value_len == 0) {
        pub->dst = BT_MESH_ADDR_UNASSIGNED;
        pub->count = 0;
        pub->ttl = 0;
        pub->period = 0;
        pub->feat = 0;
        BT_DBG("Cleared heartbeat publication");
        return 0;
    }

    if(value_len != sizeof(hb_val)) {
        BT_ERR("Unexpected value length (%d != %zu)", value_len,
               sizeof(hb_val));
        return -EINVAL;
    }

    memcpy(&hb_val, value, value_len);
    pub->dst = hb_val.dst;
    pub->period = hb_val.period;
    pub->ttl = hb_val.ttl;
    pub->feat = hb_val.feat;
    pub->net_idx = hb_val.net_idx;

    if(hb_val.indefinite) {
        pub->count = 0xffff;
    } else {
        pub->count = 0;
    }

    BT_DBG("Restored heartbeat publication");
    return 0;
}

static int cfg_set_bin(const char *key_value, uint8_t *value, int value_len)
{
    struct bt_mesh_cfg_srv *cfg = bt_mesh_cfg_get();
    BT_DBG("key %s val %s", key_value, bt_hex(value, value_len));

    if(!cfg) {
        return -ENOENT;
    }

    if(value_len == 0) {
        stored_cfg.valid = false;
        BT_DBG("Cleared configuration state");
        return 0;
    }

    if(value_len != sizeof(stored_cfg.cfg)) {
        BT_ERR("Unexpected value length (%d != %zu)", value_len,
               sizeof(stored_cfg.cfg));
        return -EINVAL;
    }

    memcpy(&stored_cfg.cfg, value, value_len);
    stored_cfg.valid = true;
    BT_DBG("Restored configuration state");
    return 0;
}


static int mod_set_bind_bin(struct bt_mesh_model *mod, uint8_t *val, int val_len)
{
    int i;

    /* Start with empty array regardless of cleared or set value */
    for(i = 0; i < ARRAY_SIZE(mod->keys); i++) {
        mod->keys[i] = BT_MESH_KEY_UNUSED;
    }

    if(val_len == 0) {
        BT_DBG("Cleared bindings for model");
        return 0;
    }

    memcpy(mod->keys, val, val_len);
    BT_DBG("Decoded %u bound keys for model", val_len / sizeof(mod->keys[0]));
    return 0;
}

static int mod_set_sub_bin(struct bt_mesh_model *mod, uint8_t *val, int val_len)
{
    /* Start with empty array regardless of cleared or set value */
    memset(mod->groups, 0, sizeof(mod->groups));

    if(val_len == 0) {
        BT_DBG("Cleared subscriptions for model");
        return 0;
    }

    memcpy(mod->groups, val, val_len);
    BT_DBG("Decoded %u subscribed group addresses for model",
           val_len / sizeof(mod->groups[0]));
    return 0;
}

static int mod_set_pub_bin(struct bt_mesh_model *mod, uint8_t *val, int val_len)
{
    struct mod_pub_val pub;

    if(!mod->pub) {
        BT_WARN("Model has no publication context!");
        return -EINVAL;
    }

    if(val_len == 0) {
        mod->pub->addr = BT_MESH_ADDR_UNASSIGNED;
        mod->pub->key = 0;
        mod->pub->cred = 0;
        mod->pub->ttl = 0;
        mod->pub->period = 0;
        mod->pub->retransmit = 0;
        mod->pub->count = 0;
        BT_DBG("Cleared publication for model");
        return 0;
    }

    if(val_len != sizeof(pub)) {
        BT_ERR("Invalid length for model publication");
        return -EINVAL;
    }

    memcpy(&pub, val, val_len);
    mod->pub->addr = pub.addr;
    mod->pub->key = pub.key;
    mod->pub->cred = pub.cred;
    mod->pub->ttl = pub.ttl;
    mod->pub->period = pub.period;
    mod->pub->retransmit = pub.retransmit;
    mod->pub->count = 0;
    BT_DBG("Restored model publication, dst 0x%04x app_idx 0x%03x",
           pub.addr, pub.key);
    return 0;
}

static int mod_set_bin(bool vnd, const char *key_value, uint8_t *value, int value_len)
{
    struct bt_mesh_model *mod;
    u8_t elem_idx, mod_idx;
    u16_t mod_key;
    char mod_data[128];
    /*skip s/ or v/ : 2 bytes*/
    mod_key = strtol(key_value + 2, NULL, 16);
    elem_idx = mod_key >> 8;
    mod_idx = mod_key;
    BT_DBG("Decoded mod_key 0x%04x as elem_idx %u mod_idx %u",
           mod_key, elem_idx, mod_idx);
    mod = bt_mesh_model_get(vnd, elem_idx, mod_idx);

    if(!mod) {
        BT_ERR("Failed to get model for elem_idx %u mod_idx %u",
               elem_idx, mod_idx);
        return -ENOENT;
    }

    if(strstr(key_value, "bind")) {
        return mod_set_bind_bin(mod, value, value_len);
    }

    if(strstr(key_value, "sub")) {
        return mod_set_sub_bin(mod, value, value_len);
    }

    if(strstr(key_value, "pub")) {
        return mod_set_pub_bin(mod, value, value_len);
    }

    if(strstr(key_value, "data")) {
        mod->flags |= BT_MESH_MOD_DATA_PRESENT;

        if(mod->cb && mod->cb->settings_set) {
            assert(value_len <= sizeof(mod_data));
            memcpy(mod_data, value, value_len);
            mod_data[value_len] = 0;
            return mod->cb->settings_set(mod, mod_data);
        }
    }

    BT_WARN("Unknown module key %s", key_value);
    return -ENOENT;
}

static int sig_mod_set_bin(const char *key_value, uint8_t *value, int value_len)
{
    return mod_set_bin(false, key_value, value, value_len);
}

static int vnd_mod_set_bin(const char *key_value, uint8_t *value, int value_len)
{
    return mod_set_bin(true, key_value, value, value_len);
}

#if CONFIG_BT_MESH_LABEL_COUNT > 0
static int va_set_bin(const char *key_value, uint8_t *value, int value_len)
{
    struct va_val va;
    struct label *lab;
    u16_t index;
    //Va/%x
    //skip Va/
    index = strtol(key_value + 3, NULL, 16);

    if(value_len == 0) {
        BT_WARN("Mesh Virtual Address length = 0");
        return 0;
    }

    if(value_len != sizeof(struct va_val)) {
        BT_ERR("Invalid length for virtual address");
        return -EINVAL;
    }

    memcpy(&va, value, value_len);

    if(va.ref == 0) {
        BT_WARN("Ignore Mesh Virtual Address ref = 0");
        return 0;
    }

    lab = get_label(index);

    if(lab == NULL) {
        BT_WARN("Out of labels buffers");
        return -ENOBUFS;
    }

    memcpy(lab->uuid, va.uuid, 16);
    lab->addr = va.addr;
    lab->ref = va.ref;
    BT_DBG("Restored Virtual Address, addr 0x%04x ref 0x%04x",
           lab->addr, lab->ref);
    return 0;
}
#endif

#if MYNEWT_VAL(BLE_MESH_PROVISIONER)
static int node_set_bin(const char *key_value, uint8_t *value, int value_len)
{
    struct bt_mesh_node *node;
    struct node_val val;
    u16_t addr;
    //Node/%x
    //skip Node/: 5 bytes
    addr = strtol(key_value + 5, NULL, 16);

    if(value_len == 0) {
        BT_DBG("val (null)");
        BT_DBG("Deleting node 0x%04x", addr);
        node = bt_mesh_node_find(addr);

        if(node) {
            bt_mesh_node_del(node, false);
        }

        return 0;
    }

    if(value_len != sizeof(struct node_val)) {
        BT_ERR("Invalid length for node_val");
        return -EINVAL;
    }

    memcpy(&val, value, value_len);
    node = bt_mesh_node_find(addr);

    if(!node) {
        node = bt_mesh_node_alloc(addr, val.num_elem, val.net_idx);
    }

    if(!node) {
        BT_ERR("No space for a new node");
        return -ENOMEM;
    }

    memcpy(node->dev_key, &val.dev_key, 16);
    BT_DBG("Node 0x%04x recovered from storage", addr);
    return 0;
}
#endif

const struct mesh_setting {
    const char *name;
    int (*func)(const char *key, uint8_t *value, int value_len);
} settings[] = {
    { "Net", net_set_bin },
    { "IV", iv_set_bin },
    { "Seq", seq_set_bin },
    { "RPL", rpl_set_bin },
    { "NetKey", net_key_set_bin },
    { "AppKey", app_key_set_bin },
    { "HBPub", hb_pub_set_bin },
    { "Cfg", cfg_set_bin },
    { "s", sig_mod_set_bin },
    { "v", vnd_mod_set_bin },
    { "Role", role_set_bin },
#if CONFIG_BT_MESH_LABEL_COUNT > 0
    { "Va", va_set_bin },
#endif
#if MYNEWT_VAL(BLE_MESH_PROVISIONER)
    { "Node", node_set_bin },
#endif
};

static int mesh_set_bin(const char *key, uint8_t *value, int value_len)
{
    int i;
    char tmp_key[64];
    char *ptr_s = NULL;
    BT_DBG("<<<Load: %s val: %s", key, bt_hex(value, value_len));
    sprintf(tmp_key, "%s", key);
    //chop '/'
    ptr_s = strstr(tmp_key, "/");

    if(ptr_s) { ptr_s[0] = '\0'; }

    for(i = 0; i < ARRAY_SIZE(settings); i++) {
        if(!strcmp(settings[i].name, tmp_key)) {
            return settings[i].func(key, value, value_len);
        }
    }

    BT_WARN("No matching handler for key %s", key);
    return -ENOENT;
}


#endif




static int subnet_init(struct bt_mesh_subnet *sub)
{
    int err;
    err = bt_mesh_net_keys_create(&sub->keys[0], sub->keys[0].net);

    if(err) {
        BT_ERR("Unable to generate keys for subnet");
        return -EIO;
    }

    if(sub->kr_phase != BT_MESH_KR_NORMAL) {
        err = bt_mesh_net_keys_create(&sub->keys[1], sub->keys[1].net);

        if(err) {
            BT_ERR("Unable to generate keys for subnet");
            memset(&sub->keys[0], 0, sizeof(sub->keys[0]));
            return -EIO;
        }
    }

    if(IS_ENABLED(CONFIG_BT_MESH_GATT_PROXY)) {
        sub->node_id = BT_MESH_NODE_IDENTITY_STOPPED;
    } else {
        sub->node_id = BT_MESH_NODE_IDENTITY_NOT_SUPPORTED;
    }

    /* Make sure we have valid beacon data to be sent */
    bt_mesh_net_beacon_update(sub);
    return 0;
}

static void commit_mod(struct bt_mesh_model *mod, struct bt_mesh_elem *elem,
                       bool vnd, bool primary, void *user_data)
{
    if(mod->pub && mod->pub->update &&
            mod->pub->addr != BT_MESH_ADDR_UNASSIGNED) {
        s32_t ms = bt_mesh_model_pub_period_get(mod);

        if(ms) {
            BT_DBG("Starting publish timer (period %u ms)",
                   (unsigned) ms);
            k_delayed_work_submit(&mod->pub->timer, ms);
        }
    }

    if(mod->cb && mod->cb->settings_commit)  {
        mod->cb->settings_commit(mod);
    }
}

static int mesh_commit(void)
{
    struct bt_mesh_hb_pub *hb_pub;
    struct bt_mesh_cfg_srv *cfg;
    int i;
    BT_DBG("sub[0].net_idx 0x%03x", bt_mesh.sub[0].net_idx);

    if(bt_mesh.sub[0].net_idx == BT_MESH_KEY_UNUSED) {
        /* Nothing to do since we're not yet provisioned */
        return 0;
    }

    if(IS_ENABLED(CONFIG_BT_MESH_PB_GATT)) {
        bt_mesh_proxy_prov_disable(true);
    }

    for(i = 0; i < ARRAY_SIZE(bt_mesh.sub); i++) {
        struct bt_mesh_subnet *sub = &bt_mesh.sub[i];
        int err;

        if(sub->net_idx == BT_MESH_KEY_UNUSED) {
            continue;
        }

        err = subnet_init(sub);

        if(err) {
            BT_ERR("Failed to init subnet 0x%03x", sub->net_idx);
        }
    }

    if(bt_mesh.ivu_duration < BT_MESH_IVU_MIN_HOURS) {
        k_delayed_work_submit(&bt_mesh.ivu_timer, BT_MESH_IVU_TIMEOUT);
    }

    bt_mesh_model_foreach(commit_mod, NULL);
    hb_pub = bt_mesh_hb_pub_get();

    if(hb_pub && hb_pub->dst != BT_MESH_ADDR_UNASSIGNED &&
            hb_pub->count && hb_pub->period) {
        BT_DBG("Starting heartbeat publication");
        k_work_submit(&hb_pub->timer.work);
    }

    cfg = bt_mesh_cfg_get();

    if(cfg && stored_cfg.valid) {
        cfg->net_transmit = stored_cfg.cfg.net_transmit;
        cfg->relay = stored_cfg.cfg.relay;
        cfg->relay_retransmit = stored_cfg.cfg.relay_retransmit;
        cfg->beacon = stored_cfg.cfg.beacon;
        cfg->gatt_proxy = stored_cfg.cfg.gatt_proxy;
        cfg->frnd = stored_cfg.cfg.frnd;
        cfg->default_ttl = stored_cfg.cfg.default_ttl;
    }

    atomic_set_bit(bt_mesh.flags, BT_MESH_VALID);

    if(!atomic_test_bit(bt_mesh.flags, BT_MESH_PROVISIONER)) {
        bt_mesh_net_start();
    }

    return 0;
}

/* Pending flags that use K_NO_WAIT as the storage timeout */
#define NO_WAIT_PENDING_BITS (BIT(BT_MESH_NET_PENDING) |           \
                  BIT(BT_MESH_IV_PENDING) |            \
                  BIT(BT_MESH_ROLE_PENDING))

/* Pending flags that use CONFIG_BT_MESH_STORE_TIMEOUT */
#define GENERIC_PENDING_BITS (BIT(BT_MESH_KEYS_PENDING) |          \
                  BIT(BT_MESH_HB_PUB_PENDING) |        \
                  BIT(BT_MESH_CFG_PENDING) |           \
                  BIT(BT_MESH_MOD_PENDING) |           \
                  BIT(BT_MESH_SEQ_PENDING)|          \
                  BIT(BT_MESH_NODES_PENDING))

#define CONFIG_MESH_STORE_WORKAROUND_FLASH_FLUSH_TIMEOUT  500
/**after provision, the mesh param is saved immediatelly, we give an 500ms delay to avoid blocking disconnection progress*/
static void schedule_store(int flag)
{
    s32_t timeout, remaining;
    atomic_set_bit(bt_mesh.flags, flag);

    if(atomic_get(bt_mesh.flags) & NO_WAIT_PENDING_BITS) {
        timeout = CONFIG_MESH_STORE_WORKAROUND_FLASH_FLUSH_TIMEOUT;//K_NO_WAIT;
    } else if(atomic_test_bit(bt_mesh.flags, BT_MESH_RPL_PENDING) &&
              (!(atomic_get(bt_mesh.flags) & GENERIC_PENDING_BITS) ||
               (CONFIG_BT_MESH_RPL_STORE_TIMEOUT <
                CONFIG_BT_MESH_STORE_TIMEOUT))) {
        timeout = K_SECONDS(CONFIG_BT_MESH_RPL_STORE_TIMEOUT);
    } else {
        timeout = K_SECONDS(CONFIG_BT_MESH_STORE_TIMEOUT);
    }

    remaining = k_delayed_work_remaining_get(&pending_store);

    if(remaining && remaining < timeout) {
        BT_DBG("Not rescheduling due to existing earlier deadline[remaining=%d, timeout=%d]", remaining,
               timeout);
        return;
    }

    BT_DBG("Waiting %d seconds", (int)(timeout / MSEC_PER_SEC));
    k_delayed_work_submit(&pending_store, timeout);
}

static void clear_iv(void)
{
    int err;
#if BASE64_ENCODE_VALUE
    err = settings_save_one("bt_mesh/IV", 0, NULL);
#else
    err = settings_save_one("IV", 0, NULL);
#endif

    if(err) {
        BT_ERR("Failed to clear IV");
    } else {
        BT_DBG("Cleared IV");
    }
}

static void clear_net(void)
{
    int err;
#if BASE64_ENCODE_VALUE
    err = settings_save_one("bt_mesh/Net", 0, NULL);
#else
    err = settings_save_one("Net", 0, NULL);
#endif

    if(err) {
        BT_ERR("Failed to clear Network");
    } else {
        BT_DBG("Cleared Network");
    }
}

static void store_pending_net(void)
{
#if BASE64_ENCODE_VALUE
    char buf[BT_SETTINGS_SIZE(sizeof(struct net_val))];
    char *str;
#endif    
    struct net_val net;
    int err;
    BT_DBG("addr 0x%04x DevKey %s", bt_mesh_primary_addr(),
           bt_hex(bt_mesh.dev_key, 16));
    net.primary_addr = bt_mesh_primary_addr();
    memcpy(net.dev_key, bt_mesh.dev_key, 16);
#if BASE64_ENCODE_VALUE
    str = settings_str_from_bytes(&net, sizeof(net), buf, sizeof(buf));

    if(!str) {
        BT_ERR("Unable to encode Network as value");
        return;
    }

    BT_DBG("Saving Network as value %s", str);
    err = settings_save_one("bt_mesh/Net", sizeof(net), str);
#else
    BT_DBG("Saving Network as value:[%s-->%s]", "bt_mesh/Net", bt_hex(&net, sizeof(net)));
    err = settings_save_one("Net", sizeof(net), (const char*)&net);
#endif

    if(err) {
        BT_ERR("Failed to store Network");
    } else {
        BT_DBG("Stored Network");
    }
}

void bt_mesh_store_net(void)
{
    schedule_store(BT_MESH_NET_PENDING);
}

static void store_pending_iv(void)
{
#if BASE64_ENCODE_VALUE
    char buf[BT_SETTINGS_SIZE(sizeof(struct iv_val))];
    char *str;
#endif    
    
    struct iv_val iv;
    int err;
    iv.iv_index = bt_mesh.iv_index;
    iv.iv_update = atomic_test_bit(bt_mesh.flags, BT_MESH_IVU_IN_PROGRESS);
    iv.iv_duration = bt_mesh.ivu_duration;
#if BASE64_ENCODE_VALUE
    str = settings_str_from_bytes(&iv, sizeof(iv), buf, sizeof(buf));

    if(!str) {
        BT_ERR("Unable to encode IV as value");
        return;
    }

    BT_DBG("Saving IV as value %s", str);
    err = settings_save_one("bt_mesh/IV", sizeof(iv), str);
#else
    BT_DBG("Saving IV as value [%s-->%s]", "bt_mesh/IV", bt_hex(&iv, sizeof(iv)));
    err = settings_save_one("IV", sizeof(iv), (const char*)&iv);
#endif

    if(err) {
        BT_ERR("Failed to store IV");
    } else {
        BT_DBG("Stored IV");
    }
}

void bt_mesh_store_iv(bool only_duration)
{
    schedule_store(BT_MESH_IV_PENDING);

    if(!only_duration) {
        /* Always update Seq whenever IV changes */
        schedule_store(BT_MESH_SEQ_PENDING);
    }
}

void bt_mesh_store_role(void)
{
    schedule_store(BT_MESH_ROLE_PENDING);
}

static void store_pending_seq(void)
{
#if BASE64_ENCODE_VALUE
    char buf[BT_SETTINGS_SIZE(sizeof(struct seq_val))];
    char *str;
#endif    
    struct seq_val seq;
    int err;
    seq.val[0] = bt_mesh.seq;
    seq.val[1] = bt_mesh.seq >> 8;
    seq.val[2] = bt_mesh.seq >> 16;
#if BASE64_ENCODE_VALUE
    str = settings_str_from_bytes(&seq, sizeof(seq), buf, sizeof(buf));

    if(!str) {
        BT_ERR("Unable to encode Seq as value");
        return;
    }

    BT_DBG("Saving Seq as value %s", str);
    err = settings_save_one("bt_mesh/Seq", sizeof(seq), str);
#else
    BT_DBG("Saving Seq as value [%s-->%s]", "bt_mesh/Seq", bt_hex(&seq, sizeof(seq)));
    err = settings_save_one("Seq", sizeof(seq), (const char*)&seq);
#endif

    if(err) {
        BT_ERR("Failed to store Seq");
    } else {
        BT_DBG("Stored Seq");
    }
}

void bt_mesh_store_seq(void)
{
    if(CONFIG_BT_MESH_SEQ_STORE_RATE &&
            (bt_mesh.seq % CONFIG_BT_MESH_SEQ_STORE_RATE)) {
        return;
    }

    schedule_store(BT_MESH_SEQ_PENDING);
}

static void store_rpl(struct bt_mesh_rpl *entry)
{
#if BASE64_ENCODE_VALUE
    char buf[BT_SETTINGS_SIZE(sizeof(struct rpl_val))];
    char *str;
#endif    
    
    struct rpl_val rpl;
    char path[18];

    int err;
    BT_DBG("src 0x%04x seq 0x%06x old_iv %u", entry->src,
           (unsigned) entry->seq, entry->old_iv);
    rpl.seq = entry->seq;
    rpl.old_iv = entry->old_iv;
#if BASE64_ENCODE_VALUE
    str = settings_str_from_bytes(&rpl, sizeof(rpl), buf, sizeof(buf));

    if(!str) {
        BT_ERR("Unable to encode RPL as value");
        return;
    }

    snprintk(path, sizeof(path), "bt_mesh/RPL/%x", entry->src);
    BT_DBG("Saving RPL %s as value %s", path, str);
    err = settings_save_one(path, sizeof(rpl), str);
#else
    snprintk(path, sizeof(path), "RPL/%x", entry->src);
    BT_DBG("Saving RPL as value [%s-->%s]", path, bt_hex(&rpl, sizeof(rpl)));
    err = settings_save_one(path, sizeof(rpl), (const char*)&rpl);
#endif

    if(err) {
        BT_ERR("Failed to store RPL");
    } else {
        BT_DBG("Stored RPL");
    }
}

static void clear_rpl(void)
{
    int i, err;
    BT_DBG("");

    for(i = 0; i < ARRAY_SIZE(bt_mesh.rpl); i++) {
        struct bt_mesh_rpl *rpl = &bt_mesh.rpl[i];
        char path[18];

        if(!rpl->src) {
            continue;
        }

#if BASE64_ENCODE_VALUE
        snprintk(path, sizeof(path), "bt_mesh/RPL/%x", rpl->src);
#else
        snprintk(path, sizeof(path), "RPL/%x", rpl->src);
#endif
        err = settings_save_one(path, 0, NULL);

        if(err) {
            BT_ERR("Failed to clear RPL");
        } else {
            BT_DBG("Cleared RPL");
        }

        memset(rpl, 0, sizeof(*rpl));
    }
}

static void store_pending_rpl(void)
{
    int i;
    BT_DBG("");

    for(i = 0; i < ARRAY_SIZE(bt_mesh.rpl); i++) {
        struct bt_mesh_rpl *rpl = &bt_mesh.rpl[i];

        if(rpl->store) {
            rpl->store = false;
            store_rpl(rpl);
        }
    }
}

static void store_pending_role(void)
{
#if BASE64_ENCODE_VALUE
    char buf[12];
    char *str;
#endif    
    int err;
    uint8_t role = 0;


    if(atomic_test_bit(bt_mesh.flags, BT_MESH_PROVISIONER)) {
        role = 2;
    } else if(atomic_test_bit(bt_mesh.flags, BT_MESH_NODE)) {
        role = 1;
    } else {
        BT_ERR("Unknown mesh role");
        return ;
    }

#if BASE64_ENCODE_VALUE
    str = settings_str_from_bytes(&role, sizeof(role),
                                  buf, sizeof(buf));

    if(!str) {
        BT_ERR("Unable to encode mesh role as value");
        return;
    }

    BT_DBG("Saving Mesh role as value %s",
           str ? str : "(null)");
    err = settings_save_one("bt_mesh/Role", sizeof(role), str);
#else
    BT_DBG("Saving Mesh role [%s-->%s]", "bt_mesh/Role", bt_hex(&role, sizeof(role)));
    err = settings_save_one("Role", sizeof(role), (const char*)&role);
#endif

    if(err) {
        BT_ERR("Failed to store Mesh role");
    } else {
        BT_DBG("Stored Mesh role");
    }
}
static void store_pending_hb_pub(void)
{
#if BASE64_ENCODE_VALUE
    char buf[BT_SETTINGS_SIZE(sizeof(struct hb_pub_val))];
    char *str;
#endif    
    struct bt_mesh_hb_pub *pub = bt_mesh_hb_pub_get();
    struct hb_pub_val val;

    int err;

    if(!pub) {
        return;
    }

    if(pub->dst == BT_MESH_ADDR_UNASSIGNED) {
#if BASE64_ENCODE_VALUE		
        str = NULL;
#else
		memset(&val, 0, sizeof(val));
#endif		
    } else {
        val.indefinite = (pub->count == 0xffff);
        val.dst = pub->dst;
        val.period = pub->period;
        val.ttl = pub->ttl;
        val.feat = pub->feat;
        val.net_idx = pub->net_idx;
#if BASE64_ENCODE_VALUE
        str = settings_str_from_bytes(&val, sizeof(val),
                                      buf, sizeof(buf));

        if(!str) {
            BT_ERR("Unable to encode hb pub as value");
            return;
        }

#endif
    }

#if BASE64_ENCODE_VALUE
    BT_DBG("Saving Heartbeat Publication as value %s",
           str ? str : "(null)");
    err = settings_save_one("bt_mesh/HBPub", sizeof(val), str);
#else
    BT_DBG("Saving Heartbeat Publication [%s-->%s]", "bt_mesh/HBPub", bt_hex(&val, sizeof(val)));
    err = settings_save_one("HBPub", sizeof(val), (const char*)&val);
#endif

    if(err) {
        BT_ERR("Failed to store Heartbeat Publication");
    } else {
        BT_DBG("Stored Heartbeat Publication");
    }
}

static void store_pending_cfg(void)
{
#if BASE64_ENCODE_VALUE
    char buf[BT_SETTINGS_SIZE(sizeof(struct cfg_val))];
    char *str;
#endif    
    
    struct bt_mesh_cfg_srv *cfg = bt_mesh_cfg_get();
    struct cfg_val val;

    int err;

    if(!cfg) {
        return;
    }

    val.net_transmit = cfg->net_transmit;
    val.relay = cfg->relay;
    val.relay_retransmit = cfg->relay_retransmit;
    val.beacon = cfg->beacon;
    val.gatt_proxy = cfg->gatt_proxy;
    val.frnd = cfg->frnd;
    val.default_ttl = cfg->default_ttl;
#if BASE64_ENCODE_VALUE
    str = settings_str_from_bytes(&val, sizeof(val), buf, sizeof(buf));

    if(!str) {
        BT_ERR("Unable to encode configuration as value");
        return;
    }

    BT_DBG("Saving configuration as value %s", str);
    err = settings_save_one("bt_mesh/Cfg", sizeof(val), str);
#else
    BT_DBG("Saving configuration [%s-->%s]", "bt_mesh/Cfg", bt_hex(&val, sizeof(val)));
    err = settings_save_one("Cfg", sizeof(val), (const char*)&val);
#endif

    if(err) {
        BT_ERR("Failed to store configuration");
    } else {
        BT_DBG("Stored configuration");
    }
}

static void clear_cfg(void)
{
    int err;
#if BASE64_ENCODE_VALUE
    err = settings_save_one("bt_mesh/Cfg", 0, NULL);
#else
    err = settings_save_one("Cfg", 0, NULL);
#endif

    if(err) {
        BT_ERR("Failed to clear configuration");
    } else {
        BT_DBG("Cleared configuration");
    }
}

static void clear_app_key(u16_t app_idx)
{
    char path[20];
    int err;
    BT_DBG("AppKeyIndex 0x%03x", app_idx);
#if BASE64_ENCODE_VALUE
    snprintk(path, sizeof(path), "bt_mesh/AppKey/%x", app_idx);
#else
    snprintk(path, sizeof(path), "AppKey/%x", app_idx);
#endif
    err = settings_save_one(path, 0, NULL);

    if(err) {
        BT_ERR("Failed to clear AppKeyIndex 0x%03x", app_idx);
    } else {
        BT_DBG("Cleared AppKeyIndex 0x%03x", app_idx);
    }
}

static void clear_net_key(u16_t net_idx)
{
    char path[20];
    int err;
    BT_DBG("NetKeyIndex 0x%03x", net_idx);
#if BASE64_ENCODE_VALUE
    snprintk(path, sizeof(path), "bt_mesh/NetKey/%x", net_idx);
#else
    snprintk(path, sizeof(path), "NetKey/%x", net_idx);
#endif
    err = settings_save_one(path, 0, NULL);

    if(err) {
        BT_ERR("Failed to clear NetKeyIndex 0x%03x", net_idx);
    } else {
        BT_DBG("Cleared NetKeyIndex 0x%03x", net_idx);
    }
}

static void store_net_key(struct bt_mesh_subnet *sub)
{
#if BASE64_ENCODE_VALUE
    char buf[BT_SETTINGS_SIZE(sizeof(struct net_key_val))];
    char *str; 
#endif    
    struct net_key_val key;
    char path[20];

    int err;
    BT_DBG("NetKeyIndex 0x%03x NetKey %s", sub->net_idx,
           bt_hex(sub->keys[0].net, 16));
    memcpy(&key.val[0], sub->keys[0].net, 16);
    memcpy(&key.val[1], sub->keys[1].net, 16);
    key.kr_flag = sub->kr_flag;
    key.kr_phase = sub->kr_phase;
#if BASE64_ENCODE_VALUE
    str = settings_str_from_bytes(&key, sizeof(key), buf, sizeof(buf));

    if(!str) {
        BT_ERR("Unable to encode NetKey as value");
        return;
    }

    snprintk(path, sizeof(path), "bt_mesh/NetKey/%x", sub->net_idx);
    BT_DBG("Saving NetKey %s as value %s", path, str);
    err = settings_save_one(path, sizeof(key), str);
#else
    snprintk(path, sizeof(path), "NetKey/%x", sub->net_idx);
    BT_DBG("Saving NetKey [%s-->%s]", path, bt_hex(&key, sizeof(key)));
    err = settings_save_one(path, sizeof(key), (const char*)&key);
#endif

    if(err) {
        BT_ERR("Failed to store NetKey");
    } else {
        BT_DBG("Stored NetKey");
    }
}

static void store_app_key(struct bt_mesh_app_key *app)
{
#if BASE64_ENCODE_VALUE
    char buf[BT_SETTINGS_SIZE(sizeof(struct app_key_val))];
    char *str;
#endif    
    struct app_key_val key;
    char path[20];
    int err;
    key.net_idx = app->net_idx;
    key.updated = app->updated;
    memcpy(key.val[0], app->keys[0].val, 16);
    memcpy(key.val[1], app->keys[1].val, 16);
#if BASE64_ENCODE_VALUE
    str = settings_str_from_bytes(&key, sizeof(key), buf, sizeof(buf));

    if(!str) {
        BT_ERR("Unable to encode AppKey as value");
        return;
    }

    snprintk(path, sizeof(path), "bt_mesh/AppKey/%x", app->app_idx);
    BT_DBG("Saving AppKey %s as value %s", path, str);
    err = settings_save_one(path, sizeof(key), str);
#else
    snprintk(path, sizeof(path), "AppKey/%x", app->app_idx);
    BT_DBG("Saving AppKey [%s-->%s]", path, bt_hex(&key, sizeof(key)));
    err = settings_save_one(path, sizeof(key), (const char*)&key);
#endif

    if(err) {
        BT_ERR("Failed to store AppKey");
    } else {
        BT_DBG("Stored AppKey");
    }
}

static void store_pending_keys(void)
{
    int i;

    for(i = 0; i < ARRAY_SIZE(key_updates); i++) {
        struct key_update *update = &key_updates[i];

        if(!update->valid) {
            continue;
        }

        if(update->clear) {
            if(update->app_key) {
                clear_app_key(update->key_idx);
            } else {
                clear_net_key(update->key_idx);
            }
        } else {
            if(update->app_key) {
                struct bt_mesh_app_key *key;
                key = bt_mesh_app_key_find(update->key_idx);

                if(key) {
                    store_app_key(key);
                } else {
                    BT_WARN("AppKeyIndex 0x%03x not found",
                            update->key_idx);
                }
            } else {
                struct bt_mesh_subnet *sub;
                sub = bt_mesh_subnet_get(update->key_idx);

                if(sub) {
                    store_net_key(sub);
                } else {
                    BT_WARN("NetKeyIndex 0x%04x not found",
                            update->key_idx);
                }
            }
        }

        update->valid = 0;
    }
}

static void store_node(struct bt_mesh_node *node)
{
#if BASE64_ENCODE_VALUE
    char buf[BT_SETTINGS_SIZE(sizeof(struct node_val))];
    char *str;
#endif    
    struct node_val val;
    char path[20];

    int err;
    val.net_idx = node->net_idx;
    val.num_elem = node->num_elem;
    memcpy(val.dev_key, node->dev_key, 16);
#if BASE64_ENCODE_VALUE
    snprintk(path, sizeof(path), "bt_mesh/Node/%x", node->addr);
    str = settings_str_from_bytes(&val, sizeof(val), buf, sizeof(buf));

    if(!str) {
        BT_ERR("Unable to encode Node as value");
        return;
    }

    err = settings_save_one(path, sizeof(val), str);
#else
    snprintk(path, sizeof(path), "Node/%x", node->addr);
    BT_DBG("Saving Node [%s-->%s]", path, bt_hex(&val, sizeof(val)));
    err = settings_save_one(path, sizeof(val), (const char*)&val);
#endif

    if(err) {
        BT_ERR("Failed to store Node %s value", path);
    } else {
        BT_DBG("Stored Node %s value", path);
    }
}

static void clear_node(u16_t addr)
{
    char path[20];
    int err;
    BT_DBG("Node 0x%04x", addr);
#if BASE64_ENCODE_VALUE
    snprintk(path, sizeof(path), "bt_mesh/Node/%x", addr);
#else
    snprintk(path, sizeof(path), "Node/%x", addr);
#endif
    err = settings_save_one(path, 0, NULL);

    if(err) {
        BT_ERR("Failed to clear Node 0x%04x", addr);
    } else {
        BT_DBG("Cleared Node 0x%04x", addr);
    }
}

static void store_pending_nodes(void)
{
    int i;

    for(i = 0; i < ARRAY_SIZE(node_updates); ++i) {
        struct node_update *update = &node_updates[i];

        if(update->addr == BT_MESH_ADDR_UNASSIGNED) {
            continue;
        }

        if(update->clear) {
            clear_node(update->addr);
        } else {
            struct bt_mesh_node *node;
            node = bt_mesh_node_find(update->addr);

            if(node) {
                store_node(node);
            } else {
                BT_WARN("Node 0x%04x not found", update->addr);
            }
        }

        update->addr = BT_MESH_ADDR_UNASSIGNED;
    }
}

static struct node_update *node_update_find(u16_t addr,
        struct node_update **free_slot)
{
    struct node_update *match;
    int i;
    match = NULL;
    *free_slot = NULL;

    for(i = 0; i < ARRAY_SIZE(node_updates); i++) {
        struct node_update *update = &node_updates[i];

        if(update->addr == BT_MESH_ADDR_UNASSIGNED) {
            *free_slot = update;
            continue;
        }

        if(update->addr == addr) {
            match = update;
        }
    }

    return match;
}

static void encode_mod_path(struct bt_mesh_model *mod, bool vnd,
                            const char *key, char *path, size_t path_len)
{
    u16_t mod_key = (((u16_t)mod->elem_idx << 8) | mod->mod_idx);
#if BASE64_ENCODE_VALUE

    if(vnd) {
        snprintk(path, path_len, "bt_mesh/v/%x/%s", mod_key, key);
    } else {
        snprintk(path, path_len, "bt_mesh/s/%x/%s", mod_key, key);
    }

#else

    if(vnd) {
        snprintk(path, path_len, "v/%x/%s", mod_key, key);
    } else {
        snprintk(path, path_len, "s/%x/%s", mod_key, key);
    }

#endif
}

static void store_pending_mod_bind(struct bt_mesh_model *mod, bool vnd)
{
    u16_t keys[CONFIG_BT_MESH_MODEL_KEY_COUNT];
#if BASE64_ENCODE_VALUE    
    char buf[BT_SETTINGS_SIZE(sizeof(keys))];
    char *val;    
#endif    
    char path[20];
    int i, count, err;


    for(i = 0, count = 0; i < ARRAY_SIZE(mod->keys); i++) {
        if(mod->keys[i] != BT_MESH_KEY_UNUSED) {
            keys[count++] = mod->keys[i];
        }
    }

#if BASE64_ENCODE_VALUE

    if(count) {
        val = settings_str_from_bytes(keys, count * sizeof(keys[0]),
                                      buf, sizeof(buf));

        if(!val) {
            BT_ERR("Unable to encode model bindings as value");
            return;
        }
    } else {
        val = NULL;
    }

    encode_mod_path(mod, vnd, "bind", path, sizeof(path));
    BT_DBG("Saving %s as %s", path, val ? val : "(null)");
    err = settings_save_one(path, count * sizeof(keys[0]), val);
#else
    encode_mod_path(mod, vnd, "bind", path, sizeof(path));
    BT_DBG("Saving %s as %s", path, bt_hex(keys, count * sizeof(keys[0])));
    err = settings_save_one(path, count * sizeof(keys[0]), (const char *)&keys[0]);
#endif

    if(err) {
        BT_ERR("Failed to store bind");
    } else {
        BT_DBG("Stored bind");
    }
}

static void store_pending_mod_sub(struct bt_mesh_model *mod, bool vnd)
{
    u16_t groups[CONFIG_BT_MESH_MODEL_GROUP_COUNT] = {0};
#if BASE64_ENCODE_VALUE    
    char buf[BT_SETTINGS_SIZE(sizeof(groups))];
    char *val;
#endif    
    char path[20];
    int i, count, err;


    for(i = 0, count = 0; i < CONFIG_BT_MESH_MODEL_GROUP_COUNT; i++) {
        if(mod->groups[i] != BT_MESH_ADDR_UNASSIGNED) {
            groups[count++] = mod->groups[i];
        }
    }

#if BASE64_ENCODE_VALUE

    if(count) {
        val = settings_str_from_bytes(groups, count * sizeof(groups[0]),
                                      buf, sizeof(buf));

        if(!val) {
            BT_ERR("Unable to encode model subscription as value");
            return;
        }
    } else {
        val = NULL;
    }

    encode_mod_path(mod, vnd, "sub", path, sizeof(path));
    BT_DBG("Saving %s as %s", path, val ? val : "(null)");
    err = settings_save_one(path, count * sizeof(groups[0]), val);
#else
    encode_mod_path(mod, vnd, "sub", path, sizeof(path));
    BT_DBG("Saving %s as %s", path, bt_hex(groups, CONFIG_BT_MESH_MODEL_GROUP_COUNT * sizeof(groups[0])));
    err = settings_save_one(path, CONFIG_BT_MESH_MODEL_GROUP_COUNT * sizeof(groups[0]), (const char*)&groups[0]);
#endif

    if(err) {
        BT_ERR("Failed to store sub");
    } else {
        BT_DBG("Stored sub");
    }
}

static void store_pending_mod_pub(struct bt_mesh_model *mod, bool vnd)
{
#if BASE64_ENCODE_VALUE
    char buf[BT_SETTINGS_SIZE(sizeof(struct mod_pub_val))];
    char *val;
 #endif   
    struct mod_pub_val pub;
    char path[20];
    int err;

    if(!mod->pub || mod->pub->addr == BT_MESH_ADDR_UNASSIGNED) {
#if BASE64_ENCODE_VALUE		
        val = NULL;
#else
		memset(&pub, 0, sizeof(pub));
#endif		
    } else {
        pub.addr = mod->pub->addr;
        pub.key = mod->pub->key;
        pub.ttl = mod->pub->ttl;
        pub.retransmit = mod->pub->retransmit;
        pub.period = mod->pub->period;
        pub.period_div = mod->pub->period_div;
        pub.cred = mod->pub->cred;
#if BASE64_ENCODE_VALUE
        val = settings_str_from_bytes(&pub, sizeof(pub),
                                      buf, sizeof(buf));

        if(!val) {
            BT_ERR("Unable to encode model publication as value");
            return;
        }

#endif
    }

    encode_mod_path(mod, vnd, "pub", path, sizeof(path));
#if BASE64_ENCODE_VALUE
    BT_DBG("Saving %s as %s", path, val ? val : "(null)");
    err = settings_save_one(path, sizeof(pub), val);
#else
    BT_DBG("Saving %s as %s", path, bt_hex(&pub, sizeof(pub)));
    err = settings_save_one(path, sizeof(pub), (const char*)&pub);
#endif

    if(err) {
        BT_ERR("Failed to store pub");
    } else {
        BT_DBG("Stored pub");
    }
}

static void store_pending_mod(struct bt_mesh_model *mod,
                              struct bt_mesh_elem *elem, bool vnd,
                              bool primary, void *user_data)
{
    if(!mod->flags) {
        return;
    }

    if(mod->flags & BT_MESH_MOD_BIND_PENDING) {
        mod->flags &= ~BT_MESH_MOD_BIND_PENDING;
        store_pending_mod_bind(mod, vnd);
    }

    if(mod->flags & BT_MESH_MOD_SUB_PENDING) {
        mod->flags &= ~BT_MESH_MOD_SUB_PENDING;
        store_pending_mod_sub(mod, vnd);
    }

    if(mod->flags & BT_MESH_MOD_PUB_PENDING) {
        mod->flags &= ~BT_MESH_MOD_PUB_PENDING;
        store_pending_mod_pub(mod, vnd);
    }
}

#define IS_VA_DEL(_label)   ((_label)->ref == 0)
static void store_pending_va(void)
{
#if BASE64_ENCODE_VALUE
    char buf[BT_SETTINGS_SIZE(sizeof(struct va_val))];
    char *val;
#endif       
    struct label *lab;
    struct va_val va;
    char path[18];
    u16_t i;
    int err = 0;

    for(i = 0; (lab = get_label(i)) != NULL; i++) {
        if(!atomic_test_and_clear_bit(lab->flags,
                                      BT_MESH_VA_CHANGED)) {
            continue;
        }

#if BASE64_ENCODE_VALUE
        snprintk(path, sizeof(path), "bt_mesh/Va/%x", i);
#else
        snprintk(path, sizeof(path), "Va/%x", i);
#endif

        if(IS_VA_DEL(lab)) {
#if BASE64_ENCODE_VALUE			
            val = NULL;
#else
			memset(&va, 0, sizeof(va));
#endif			
        } else {
            va.ref = lab->ref;
            va.addr = lab->addr;
            memcpy(va.uuid, lab->uuid, 16);
#if BASE64_ENCODE_VALUE
            val = settings_str_from_bytes(&va, sizeof(va),
                                          buf, sizeof(buf));

            if(!val) {
                BT_ERR("Unable to encode model publication as value");
                return;
            }

            err = settings_save_one(path, sizeof(va), val);
#else
            err = settings_save_one(path, sizeof(va), (const char*)&va);
#endif
        }

        if(err) {
            BT_ERR("Failed to %s %s value (err %d)",
                   IS_VA_DEL(lab) ? "delete" : "store", path, err);
        } else {
            BT_DBG("%s %s value",
                   IS_VA_DEL(lab) ? "Deleted" : "Stored", path);
        }
    }
}

static void store_pending(struct ble_npl_event *work)
{
    BT_DBG("");

    if(atomic_test_and_clear_bit(bt_mesh.flags, BT_MESH_RPL_PENDING)) {
        if(atomic_test_bit(bt_mesh.flags, BT_MESH_VALID)) {
            store_pending_rpl();
        } else {
            clear_rpl();
        }
    }

    if(atomic_test_and_clear_bit(bt_mesh.flags, BT_MESH_KEYS_PENDING)) {
        store_pending_keys();
    }

    if(atomic_test_and_clear_bit(bt_mesh.flags, BT_MESH_NET_PENDING)) {
        if(atomic_test_bit(bt_mesh.flags, BT_MESH_VALID)) {
            store_pending_net();
        } else {
            clear_net();
        }
    }

    if(atomic_test_and_clear_bit(bt_mesh.flags, BT_MESH_IV_PENDING)) {
        if(atomic_test_bit(bt_mesh.flags, BT_MESH_VALID)) {
            store_pending_iv();
        } else {
            clear_iv();
        }
    }

    if(atomic_test_and_clear_bit(bt_mesh.flags, BT_MESH_SEQ_PENDING)) {
        store_pending_seq();
    }

    if(atomic_test_and_clear_bit(bt_mesh.flags, BT_MESH_HB_PUB_PENDING)) {
        store_pending_hb_pub();
    }

    if(atomic_test_and_clear_bit(bt_mesh.flags, BT_MESH_CFG_PENDING)) {
        if(atomic_test_bit(bt_mesh.flags, BT_MESH_VALID)) {
            store_pending_cfg();
        } else {
            clear_cfg();
        }
    }

    if(atomic_test_and_clear_bit(bt_mesh.flags, BT_MESH_MOD_PENDING)) {
        bt_mesh_model_foreach(store_pending_mod, NULL);
    }

    if(atomic_test_and_clear_bit(bt_mesh.flags, BT_MESH_VA_PENDING)) {
        store_pending_va();
    }

    if(atomic_test_and_clear_bit(bt_mesh.flags, BT_MESH_ROLE_PENDING)) {
        store_pending_role();
    }

    if(IS_ENABLED(CONFIG_BT_MESH_PROVISIONER) &&
            atomic_test_and_clear_bit(bt_mesh.flags, BT_MESH_NODES_PENDING)) {
        store_pending_nodes();
    }
}

void bt_mesh_store_rpl(struct bt_mesh_rpl *entry)
{
    entry->store = true;
    schedule_store(BT_MESH_RPL_PENDING);
}

static struct key_update *key_update_find(bool app_key, u16_t key_idx,
        struct key_update **free_slot)
{
    struct key_update *match;
    int i;
    match = NULL;
    *free_slot = NULL;

    for(i = 0; i < ARRAY_SIZE(key_updates); i++) {
        struct key_update *update = &key_updates[i];

        if(!update->valid) {
            *free_slot = update;
            continue;
        }

        if(update->app_key != app_key) {
            continue;
        }

        if(update->key_idx == key_idx) {
            match = update;
        }
    }

    return match;
}

void bt_mesh_store_subnet(struct bt_mesh_subnet *sub)
{
    struct key_update *update, *free_slot;
    BT_DBG("NetKeyIndex 0x%03x", sub->net_idx);
    update = key_update_find(false, sub->net_idx, &free_slot);

    if(update) {
        update->clear = 0;
        schedule_store(BT_MESH_KEYS_PENDING);
        return;
    }

    if(!free_slot) {
        store_net_key(sub);
        return;
    }

    free_slot->valid = 1;
    free_slot->key_idx = sub->net_idx;
    free_slot->app_key = 0;
    free_slot->clear = 0;
    schedule_store(BT_MESH_KEYS_PENDING);
}

void bt_mesh_store_app_key(struct bt_mesh_app_key *key)
{
    struct key_update *update, *free_slot;
    BT_DBG("AppKeyIndex 0x%03x", key->app_idx);
    update = key_update_find(true, key->app_idx, &free_slot);

    if(update) {
        update->clear = 0;
        schedule_store(BT_MESH_KEYS_PENDING);
        return;
    }

    if(!free_slot) {
        store_app_key(key);
        return;
    }

    free_slot->valid = 1;
    free_slot->key_idx = key->app_idx;
    free_slot->app_key = 1;
    free_slot->clear = 0;
    schedule_store(BT_MESH_KEYS_PENDING);
}

void bt_mesh_store_hb_pub(void)
{
    schedule_store(BT_MESH_HB_PUB_PENDING);
}

void bt_mesh_store_cfg(bool flush)
{
    if(flush)
    {
       store_pending_cfg(); 
    }else
    {
    schedule_store(BT_MESH_CFG_PENDING);
    }
}

void bt_mesh_clear_net(void)
{
    schedule_store(BT_MESH_NET_PENDING);
    schedule_store(BT_MESH_IV_PENDING);
    schedule_store(BT_MESH_CFG_PENDING);
}

void bt_mesh_clear_subnet(struct bt_mesh_subnet *sub)
{
    struct key_update *update, *free_slot;
    BT_DBG("NetKeyIndex 0x%03x", sub->net_idx);
    update = key_update_find(false, sub->net_idx, &free_slot);

    if(update) {
        update->clear = 1;
        schedule_store(BT_MESH_KEYS_PENDING);
        return;
    }

    if(!free_slot) {
        clear_net_key(sub->net_idx);
        return;
    }

    free_slot->valid = 1;
    free_slot->key_idx = sub->net_idx;
    free_slot->app_key = 0;
    free_slot->clear = 1;
    schedule_store(BT_MESH_KEYS_PENDING);
}

void bt_mesh_clear_app_key(struct bt_mesh_app_key *key)
{
    struct key_update *update, *free_slot;
    BT_DBG("AppKeyIndex 0x%03x", key->app_idx);
    update = key_update_find(true, key->app_idx, &free_slot);

    if(update) {
        update->clear = 1;
        schedule_store(BT_MESH_KEYS_PENDING);
        return;
    }

    if(!free_slot) {
        clear_app_key(key->app_idx);
        return;
    }

    free_slot->valid = 1;
    free_slot->key_idx = key->app_idx;
    free_slot->app_key = 1;
    free_slot->clear = 1;
    schedule_store(BT_MESH_KEYS_PENDING);
}

void bt_mesh_clear_rpl(void)
{
    //schedule_store(BT_MESH_RPL_PENDING);
    clear_rpl();
}

void bt_mesh_clear_seq(void)
{
    bt_mesh.seq = 0;
}
void bt_mesh_store_mod_bind(struct bt_mesh_model *mod)
{
    mod->flags |= BT_MESH_MOD_BIND_PENDING;
    schedule_store(BT_MESH_MOD_PENDING);
}

void bt_mesh_store_mod_sub(struct bt_mesh_model *mod)
{
    mod->flags |= BT_MESH_MOD_SUB_PENDING;
    schedule_store(BT_MESH_MOD_PENDING);
}

void bt_mesh_store_mod_pub(struct bt_mesh_model *mod)
{
    mod->flags |= BT_MESH_MOD_PUB_PENDING;
    schedule_store(BT_MESH_MOD_PENDING);
}


void bt_mesh_store_label(void)
{
    schedule_store(BT_MESH_VA_PENDING);
}

void bt_mesh_store_node(struct bt_mesh_node *node)
{
    struct node_update *update, *free_slot;
    BT_DBG("Node 0x%04x", node->addr);
    update = node_update_find(node->addr, &free_slot);

    if(update) {
        update->clear = false;
        schedule_store(BT_MESH_NODES_PENDING);
        return;
    }

    if(!free_slot) {
        store_node(node);
        return;
    }

    free_slot->addr = node->addr;
    schedule_store(BT_MESH_NODES_PENDING);
}

void bt_mesh_clear_node(struct bt_mesh_node *node)
{
    struct node_update *update, *free_slot;
    BT_DBG("Node 0x%04x", node->addr);
    update = node_update_find(node->addr, &free_slot);

    if(update) {
        update->clear = true;
        schedule_store(BT_MESH_NODES_PENDING);
        return;
    }

    if(!free_slot) {
        clear_node(node->addr);
        return;
    }

    free_slot->addr = node->addr;
    schedule_store(BT_MESH_NODES_PENDING);
}

int bt_mesh_model_data_store(struct bt_mesh_model *mod, bool vnd,
                             const void *data, size_t data_len)
{
    char path[20];
#if BASE64_ENCODE_VALUE    
    char buf[BT_SETTINGS_SIZE(sizeof(struct mod_pub_val))];
    char *val;
#endif    
    int err;
    encode_mod_path(mod, vnd, "data", path, sizeof(path));

    if(data_len) {
        mod->flags |= BT_MESH_MOD_DATA_PRESENT;
#if BASE64_ENCODE_VALUE
        val = settings_str_from_bytes(data, data_len,
                                      buf, sizeof(buf));

        if(!val) {
            BT_ERR("Unable to encode model publication as value");
            return -EINVAL;
        }

        err = settings_save_one(path, data_len, val);
#else
        err = settings_save_one(path, data_len, data);
#endif
    } else if(mod->flags & BT_MESH_MOD_DATA_PRESENT) {
        mod->flags &= ~BT_MESH_MOD_DATA_PRESENT;
        err = settings_save_one(path, 0, NULL);
    } else {
        /* Nothing to delete */
        err = 0;
    }

    if(err) {
        BT_ERR("Failed to store %s value", path);
    } else {
        BT_DBG("Stored %s value", path);
    }

    return err;
}

static bool conf_save_pending = false;
int conf_save_one(const char *path, int len, const char *value)
{
    BT_DBG(">>>Save: %s val: %s", path, bt_hex(value, len));
    tls_mesh_store_update(path, len > 0 ? 1 : 0, (const uint8_t *)value, len);
    //tls_mesh_store_flush();
    conf_save_pending = true;
    return 0;
}

static int conf_load(void)
{
    char key[64];
    uint8_t value[128];
    int value_len = 0, ret = 0;

    while(1) {
        memset(key, 0, sizeof(key));
        memset(value, 0, sizeof(value));
        value_len = 0;
        ret = tls_mesh_store_retrieve(key, value, &value_len);

        if(ret > 0) {
            mesh_set_bin(key, value, value_len);
        } else if(ret == 0) {
            BT_DBG("eof\r\n");
            break;
        } else {
            //eof
            BT_DBG("invalid param area\r\n");
            break;
        }
    }

    return ret;
}


void bt_mesh_settings_init(void)
{
    k_delayed_work_init(&pending_store, store_pending);
    tls_mesh_store_init();
}
int bt_mesh_settings_clear(void)
{
    return tls_mesh_store_erase();
}

/**
 * Load mesh parameters from nvram and check the prefered role matched or not.
 *
 * @param role_node             true: node ; false provisioner.
 *
 *
 * @return 0 on success.
 * @return -2 means the role mismatch.
 * @return -1 means parameter area unavailable
 */

int bt_mesh_settings_load(bool role_node)
{
    int err = 0;
    err = conf_load();

    if(!err) {
        if((atomic_test_bit(bt_mesh.flags, BT_MESH_NODE) && role_node) ||
                (atomic_test_bit(bt_mesh.flags, BT_MESH_PROVISIONER) && (!role_node))) {
            mesh_commit();
        } else {
            err = -2;
        }
    }

    return err;
}
void bt_mesh_settings_deinit(void)
{
    k_delayed_work_deinit(&pending_store);
    tls_mesh_store_deinit();
}

void bt_mesh_settings_flush(void)
{
    if(conf_save_pending)
    {
        conf_save_pending = false;
        tls_mesh_store_flush();
    }
}
#endif /* MYNEWT_VAL(BLE_MESH_SETTINGS) */
