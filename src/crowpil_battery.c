/*
 * crowpil_battery.c — CrowPi-L battery as a Linux power_supply
 *
 * Based on fake_battery by Rob Hoelz (GPL v2).
 * Replaces the misc-device write path with direct I2C polling.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/i2c-smbus.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

/* ---- CrowPi-L I2C constants ---- */
//#define CROWPI_I2C_BUS      1
#define CROWPI_I2C_BUS      8
#define CROWPI_BAT_ADDR     0x03
#define CROWPI_REG          0x35   /* block read 6 bytes */

#define POLL_INTERVAL       (2 * HZ)
//#define POLL_INTERVAL       (10 * HZ)
//#define POLL_INTERVAL       (30 * HZ)
#define ENERGY_FULL_UWH     18500000   /* 18.5 Wh in µWh */

/* ---- battery state ---- */
static struct {
    int status;
    int capacity_level;
    int capacity;
    int voltage_mv;
} bat_state = {
    .status         = POWER_SUPPLY_STATUS_DISCHARGING,
    .capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL,
    .capacity       = 100,
};

static int ac_online = 1;

/* ---- persistent I2C client ---- */
static struct i2c_client *bat_client;

/* ---- I2C board info (file scope) ---- */
static struct i2c_board_info bat_info = {
    I2C_BOARD_INFO("crowpi-battery", CROWPI_BAT_ADDR)
};

/* ---- power_supply pointers ---- */
static struct power_supply *psy_bat;
static struct power_supply *psy_ac;

/* ---- delayed work (declared BEFORE the function) ---- */
static void crowpi_poll(struct work_struct *work);
static DECLARE_DELAYED_WORK(poll_dwork, crowpi_poll);

/* ---- voltage-to-percentage via linear interpolation ---- */
static const struct {
    int mv;
    int pct;
} bat_curve[] = {
    { 7200, 100 },
    { 7032,  86 },
    { 6852,  71 },
    { 6684,  57 },
    { 6516,  43 },
    { 6348,  29 },
    { 6168,  14 },
    { 6024,   2 },
};

static int voltage_to_percent(int voltage_mv)
{
    int i;

    if (voltage_mv >= bat_curve[0].mv)
        return 100;

    for (i = 0; i < ARRAY_SIZE(bat_curve) - 1; i++) {
        if (voltage_mv >= bat_curve[i + 1].mv) {
            int hi_mv  = bat_curve[i].mv;
            int hi_pct = bat_curve[i].pct;
            int lo_mv  = bat_curve[i + 1].mv;
            int lo_pct = bat_curve[i + 1].pct;
            return lo_pct + (voltage_mv - lo_mv) * (hi_pct - lo_pct)
                           / (hi_mv - lo_mv);
        }
    }
    return 0;
}

/* ---- I2C read helper ---- */
static int crowpi_i2c_read(int *cap, int *charging, int *voltage_mv)
{
    u8 buf[6];
    int ret;

    if (!bat_client)
        return -ENODEV;

    ret = i2c_smbus_read_i2c_block_data(bat_client, CROWPI_REG, 6, buf);
    if (ret != 6)
        return -EREMOTEIO;

    if (buf[0] != 0xAA || buf[5] != 0x55)
        return -EBADMSG;

    *charging = (buf[1] != 0) ? 1 : 0;

    {
        u16 raw = (buf[2] << 8) | buf[3];
        *voltage_mv = (int)((raw * 3000 / 1024) * 260 / 100);
    }

    *cap = voltage_to_percent(*voltage_mv);
    return 0;
}

/* ---- periodic poll work ---- */
static void crowpi_poll(struct work_struct *work)
{
    int cap = 0, charging = 0, vmv = 0;

    if (crowpi_i2c_read(&cap, &charging, &vmv) == 0) {
        bat_state.capacity = cap;
        ac_online          = charging;
        bat_state.voltage_mv = vmv;

        if (charging)
            bat_state.status = (cap >= 100)
                ? POWER_SUPPLY_STATUS_FULL
                : POWER_SUPPLY_STATUS_CHARGING;
        else
            bat_state.status = POWER_SUPPLY_STATUS_DISCHARGING;

        if (cap >= 98)
            bat_state.capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
        else if (cap >= 70)
            bat_state.capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_HIGH;
        else if (cap >= 30)
            bat_state.capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
        else if (cap >= 5)
            bat_state.capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
        else
            bat_state.capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;

        power_supply_changed(psy_bat);
        power_supply_changed(psy_ac);
    }

    queue_delayed_work(system_wq, &poll_dwork, POLL_INTERVAL);
}

/* ---- get_property callbacks ---- */
static int crowpil_battery_get_property(struct power_supply *psy,
                                   enum power_supply_property psp,
                                   union power_supply_propval *val)
{
    switch (psp) {
    case POWER_SUPPLY_PROP_STATUS:
        val->intval = bat_state.status;
        break;
    case POWER_SUPPLY_PROP_CAPACITY:
        val->intval = bat_state.capacity;
        break;
    case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
        val->intval = bat_state.capacity_level;
        break;
    case POWER_SUPPLY_PROP_PRESENT:
        val->intval = 1;
        break;
    case POWER_SUPPLY_PROP_TECHNOLOGY:
        val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
        break;
    case POWER_SUPPLY_PROP_MODEL_NAME:
        val->strval = "CrowPi-L Battery";
        break;
    case POWER_SUPPLY_PROP_MANUFACTURER:
        val->strval = "CrowPi";
        break;
    case POWER_SUPPLY_PROP_VOLTAGE_NOW:
        val->intval = bat_state.voltage_mv * 1000;  /* mV → µV */
        break;
    case POWER_SUPPLY_PROP_ENERGY_FULL:
    case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
        val->intval = ENERGY_FULL_UWH;
        break;
//    case POWER_SUPPLY_PROP_ENERGY_NOW:
//        val->intval = ENERGY_FULL_UWH * bat_state.capacity / 100;
//        break;
    case POWER_SUPPLY_PROP_ENERGY_NOW:
        val->intval = ENERGY_FULL_UWH * (bat_state.voltage_mv - 6024) / (7200 - 6024);
        break;   
    case POWER_SUPPLY_PROP_TEMP:
        return -ENODATA;   /* no temperature sensor */
    case POWER_SUPPLY_PROP_POWER_NOW:
        return -ENODATA;   /* no current sensor */
    default:
        return -EINVAL;
    }
    return 0;
}

static int crowpi_ac_get_property(struct power_supply *psy,
                                  enum power_supply_property psp,
                                  union power_supply_propval *val)
{
    if (psp == POWER_SUPPLY_PROP_ONLINE)
        val->intval = ac_online;
    else
        return -EINVAL;
    return 0;
}

/* ---- property lists ---- */
static const enum power_supply_property bat_props[] = {
    POWER_SUPPLY_PROP_STATUS,
    POWER_SUPPLY_PROP_CAPACITY,
    POWER_SUPPLY_PROP_CAPACITY_LEVEL,
    POWER_SUPPLY_PROP_PRESENT,
    POWER_SUPPLY_PROP_TECHNOLOGY,
    POWER_SUPPLY_PROP_MODEL_NAME,
    POWER_SUPPLY_PROP_MANUFACTURER,
    POWER_SUPPLY_PROP_VOLTAGE_NOW,
    POWER_SUPPLY_PROP_ENERGY_FULL,
    POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN,
    POWER_SUPPLY_PROP_ENERGY_NOW,
    POWER_SUPPLY_PROP_TEMP,
    POWER_SUPPLY_PROP_POWER_NOW,
};

static const enum power_supply_property ac_props[] = {
    POWER_SUPPLY_PROP_ONLINE,
};

/* ---- supply descriptors (file scope) ---- */
static struct power_supply_desc bat_desc = {
    .name           = "BAT0",
    .type           = POWER_SUPPLY_TYPE_BATTERY,
    .properties     = bat_props,
    .num_properties = ARRAY_SIZE(bat_props),
    .get_property   = crowpil_battery_get_property,
};

static struct power_supply_desc ac_desc = {
    .name           = "AC0",
    .type           = POWER_SUPPLY_TYPE_MAINS,
    .properties     = ac_props,
    .num_properties = ARRAY_SIZE(ac_props),
    .get_property   = crowpi_ac_get_property,
};

/* ---- supplied_to linkage ---- */
static char *ac_supplies[] = { "BAT0" };

static struct power_supply_config bat_cfg = { };
static struct power_supply_config ac_cfg = {
    .supplied_to     = ac_supplies,
    .num_supplicants = ARRAY_SIZE(ac_supplies),
};

/* ---- module init ---- */
static int __init crowpil_battery_init(void)
{
    struct i2c_adapter *adap;
    int ret;

    adap = i2c_get_adapter(CROWPI_I2C_BUS);
    if (!adap) {
        pr_err("crowpil_battery: I2C bus %d not found\n", CROWPI_I2C_BUS);
        return -ENODEV;
    }

    bat_client = i2c_new_client_device(adap, &bat_info);
    i2c_put_adapter(adap);

    if (IS_ERR(bat_client)) {
        ret = PTR_ERR(bat_client);
        pr_err("crowpil_battery: failed to create I2C client (err %d)\n", ret);
        return ret;
    }

    psy_bat = power_supply_register(&bat_client->dev, &bat_desc, &bat_cfg);
    if (IS_ERR(psy_bat)) {
        ret = PTR_ERR(psy_bat);
        pr_err("crowpil_battery: failed to register BAT0 (err %d)\n", ret);
        goto err_i2c;
    }

    psy_ac = power_supply_register(&bat_client->dev, &ac_desc, &ac_cfg);
    if (IS_ERR(psy_ac)) {
        ret = PTR_ERR(psy_ac);
        pr_err("crowpil_battery: failed to register AC0 (err %d)\n", ret);
        power_supply_unregister(psy_bat);
        goto err_i2c;
    }

    queue_delayed_work(system_wq, &poll_dwork, HZ);

    pr_info("crowpil_battery: loaded\n");
    return 0;

err_i2c:
    i2c_unregister_device(bat_client);
    bat_client = NULL;
    return ret;
}

/* ---- module exit ---- */
static void __exit crowpil_battery_exit(void)
{
    cancel_delayed_work_sync(&poll_dwork);
    power_supply_unregister(psy_ac);
    power_supply_unregister(psy_bat);
    i2c_unregister_device(bat_client);
    pr_info("crowpil_battery: unloaded\n");
}

module_init(crowpil_battery_init);
module_exit(crowpil_battery_exit);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("CrowPi");
MODULE_DESCRIPTION("CrowPi-L battery power_supply via I2C");


