/*
 * This file is part of the TREZOR project.
 *
 * Copyright (C) 2014 Pavol Rusnak <stick@satoshilabs.com>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

/* === Includes ============================================================ */

#include <string.h>
#include <stdint.h>

#include <libopencm3/stm32/flash.h>

#include <bip39.h>
#include <aes.h>
#include <pbkdf2.h>
#include <keepkey_board.h>
#include <pbkdf2.h>
#include <keepkey_flash.h>
#include <interface.h>
#include <memory.h>
#include <rng.h>

#include "util.h"
#include "storage.h"
#include "passphrase_sm.h"
#include "fsm.h"
#include <stddef.h>
#include <keepkey_usart.h>

/* === Private Variables =================================================== */

static bool   sessionRootNodeCached;
static HDNode sessionRootNode;

static bool sessionPinCached;
static char sessionPin[17];

static bool sessionPassphraseCached;
static char sessionPassphrase[51];
static Allocation storage_location = FLASH_INVALID;
static uint32_t pfa_index = 0;

/* Shadow memory for configuration data in storage partition */
_Static_assert(sizeof(ConfigFlash) <= FLASH_STORAGE_LEN, "ConfigFlash struct is too large for storage partition");
static ConfigFlash shadow_config;

/* === Private Functions =================================================== */

void dump_stor_offset_pkhoo(void) /*pkhoo */
{
    dbg_print("Storage has_node Offset =  0x%x : 0x%x\n\r", 
            offsetof(ConfigFlash, storage.has_node), offsetof(ConfigFlash_v1, storage.has_node));
    dbg_print("Storage has_passphrase_protection Offset =  0x%x : 0x%x\n\r", 
            offsetof(ConfigFlash, storage.has_passphrase_protection), offsetof(ConfigFlash_v1, storage.has_passphrase_protection));
    dbg_print("Storage passphrase_protection Offset =  0x%x : 0x%x\n\r", 
            offsetof(ConfigFlash, storage.passphrase_protection), offsetof(ConfigFlash_v1, storage.passphrase_protection));
    dbg_print("Storage has_pin_failed_attempts offset =  0x%x : 0x%x\n\r", 
            offsetof(ConfigFlash, storage.has_pin_failed_attempts), offsetof(ConfigFlash_v1, storage.has_pin_failed_attempts));
    dbg_print("Storage pin_failed_attempts Offset =  0x%x : 0x%x\n\r", 
            offsetof(ConfigFlash, storage.pin_failed_attempts), offsetof(ConfigFlash_v1, storage.pin_failed_attempts));
    dbg_print("Storage has_pin Offset =  0x%x : 0x%x\n\r", 
            offsetof(ConfigFlash, storage.has_pin), offsetof(ConfigFlash_v1, storage.has_pin));
    dbg_print("Storage pin Offset =  0x%x : 0x%x\n\r", 
            offsetof(ConfigFlash, storage.pin), offsetof(ConfigFlash_v1, storage.pin));
    dbg_print("Storage Cache Offset =  0x%x : 0x%x\n\r", 
            offsetof(ConfigFlash, cache), offsetof(ConfigFlash_v1, cache));
}

/*
 * upd_storage_v1tov2() - Update version 1 storage to version 2 format
 *
 * INPUT
 *      none
 * OUTPUT
 *      none
 *
 */
static void upd_storage_v1tov2(void)
{
    ConfigFlash_v1 *stor_config_v1 = (ConfigFlash_v1 *)flash_write_helper(storage_location);

    dump_stor_offset_pkhoo();
    dbg_print("******************************************************\n\r");
    dbg_print("Update Storage table V1 to V2 \n\r");
    dbg_print("******************************************************\n\r");


    /*** update "has_node" to  "has_pin_failed_attempts" ***/
    memcpy(&shadow_config.storage.has_node, &stor_config_v1->storage.has_node, 
        offsetof(ConfigFlash, storage.has_pin_failed_attempts) - offsetof(ConfigFlash, storage.has_node) + 1);


    /*** update "pin_failed_attempts" ***/
    if(stor_config_v1->storage.pin_failed_attempts <= 0xFF)
    {
        shadow_config.storage.pin_failed_attempts[0] =  (uint8_t)(stor_config_v1->storage.pin_failed_attempts);
    }
    else
    {
        /* saturage pin_failed_attempts to 0xFF;  */  
        shadow_config.storage.pin_failed_attempts[0] =  0xFF;
    }

    /*** update "PIN" ***/
    shadow_config.storage.has_pin =  (uint8_t)(stor_config_v1->storage.has_pin);
    memcpy(&shadow_config.storage.pin, stor_config_v1->storage.pin, sizeof(shadow_config.storage.pin));

    /*** update "language" ***/
    shadow_config.storage.has_language =  (uint8_t)(stor_config_v1->storage.has_language);
    memcpy(&shadow_config.storage.language, stor_config_v1->storage.language, sizeof(shadow_config.storage.language));

    /*** update "label" ***/ 
    shadow_config.storage.has_label =  (uint8_t)(stor_config_v1->storage.has_label);
    memcpy(&shadow_config.storage.label, stor_config_v1->storage.label, sizeof(shadow_config.storage.label));

    /*** update "imported" ***/ 
    shadow_config.storage.has_imported =  (uint8_t)(stor_config_v1->storage.has_imported);
    memcpy(&shadow_config.storage.imported, &stor_config_v1->storage.imported, sizeof(shadow_config.storage.imported));

    /*** update "cache" ***/
    memcpy(&shadow_config.cache, &stor_config_v1->cache, sizeof(shadow_config.cache));
}

/*
 * storage_from_flash() - Copy configuration from storage partition in flash memory to shadow memory in RAM
 *
 * INPUT
 *     - stor_config: storage config
 * OUTPUT
 *     true/false status
 *
 */
static bool storage_from_flash(ConfigFlash *stor_config)
{
    /* load config values from active config node */
    switch(stor_config->storage.version)
    {
        case 1:
        {
            /* update storage V1 to V2 format */
            upd_storage_v1tov2();
            break;
        }
        case STORAGE_VERSION:
        {
            memcpy(&shadow_config, stor_config, sizeof(shadow_config));
            update_pfa_stat();
            break;
        }
        default:
        {
            return false;
        }
    }

    shadow_config.storage.version = STORAGE_VERSION;
    return true;
}

/*
 * wear_leveling_shift() - Shifts sector for config storage
 *
 * INPUT
 *     none
 * OUTPUT
 *     none
 *
 */
static void wear_leveling_shift(void)
{
    switch(storage_location)
    {
        case FLASH_STORAGE1:
        {
            storage_location = FLASH_STORAGE2;
            break;
        }

        case FLASH_STORAGE2:
        {
            storage_location = FLASH_STORAGE3;
            break;
        }

        /* wraps around */
        case FLASH_STORAGE3:
        {
            storage_location = FLASH_STORAGE1;
            break;
        }

        default:
        {
            storage_location = STORAGE_SECT_DEFAULT;
            break;
        }
    }
}

/*
 * storage_set_root_node_cache() - Set root node in storage cache
 *
 * INPUT
 *     - node: hd node to cache
 * OUTPUT
 *     none
 *
 */
static void storage_set_root_node_cache(HDNode *node)
{
    if(!(shadow_config.storage.has_passphrase_protection &&
            shadow_config.storage.passphrase_protection && strlen(sessionPassphrase)))
    {
        memset(&shadow_config.cache.root_node_cache, 0,
               sizeof(((ConfigFlash *)NULL)->cache.root_node_cache));
        memcpy(&shadow_config.cache.root_node_cache, node,
               sizeof(((ConfigFlash *)NULL)->cache.root_node_cache));
        shadow_config.cache.root_node_cache_status = CACHE_EXISTS;
        storage_commit();
    }
}

/*
 * storage_get_root_node_cache() - Gets root node cache from storage and returns true if found
 *
 * INPUT
 *     - node: hd node to be filled with found cache
 * OUTPUT
 *     true/false
 *
 */
static bool storage_get_root_node_cache(HDNode *node)
{
    if(!(shadow_config.storage.has_passphrase_protection &&
            shadow_config.storage.passphrase_protection && strlen(sessionPassphrase)) &&
            shadow_config.cache.root_node_cache_status == CACHE_EXISTS)
    {
        memcpy(node, &shadow_config.cache.root_node_cache,
               sizeof(((ConfigFlash *)NULL)->cache.root_node_cache));
        return true;
    }

    return false;
}

/* === Functions =========================================================== */

/*
 * storage_init() - Validate storage content and copy data to shadow memory
 *
 * INPUT
 *     none
 * OUTPUT
 *     none
 */
void storage_init(void)
{
    ConfigFlash *stor_config;

    /* Find storage sector with valid data and set storage_location variable */
    if(find_active_storage(&storage_location))
    {
        stor_config = (ConfigFlash *)flash_write_helper(storage_location);
    }
    else
    {
        /* Set to storage sector1 as default if no sector has been initialized */
        storage_location = STORAGE_SECT_DEFAULT;
        stor_config = (ConfigFlash *)flash_write_helper(storage_location);
    }
    /* Reset shadow configuration in RAM */
    storage_reset();

    /* Verify storage partition is initialized */
    if(memcmp((void *)stor_config->meta.magic , STORAGE_MAGIC_STR,
              STORAGE_MAGIC_LEN) == 0)
    {
        /* Clear out stor_config before finding end config node */
        memcpy(shadow_config.meta.uuid, (void *)&stor_config->meta.uuid,
               sizeof(shadow_config.meta.uuid));
        data2hex(shadow_config.meta.uuid, sizeof(shadow_config.meta.uuid),
                 shadow_config.meta.uuid_str);

        if(stor_config->storage.version)
        {
            if(stor_config->storage.version <= STORAGE_VERSION)
            {
                storage_from_flash(stor_config);
            }
        }

        /* New app with storage version changed!  update the storage space */
        if(stor_config->storage.version != STORAGE_VERSION)
        {
            storage_commit();
        }
    }
    else
    {
        /* Keep storage area cleared */
        storage_reset_uuid();
        storage_commit();
    }
    
    dbg_print("sizeof pin_failed_attempts = %d\n\r", PFA_BFR_SIZE);
    dbg_print("pfa ptr Start = 0x%x, pfaEnd = 0%x\n\r", 
            &stor_config->storage.pin_failed_attempts, &stor_config->storage.has_pin);
    dbg_print("storVer = 0x%x, F/W storVer = 0x%x\n\r", stor_config->storage.version, STORAGE_VERSION);
    dbg_print("pfa[] = 0x%x\n\r", shadow_config.storage.pin_failed_attempts[pfa_index]);
}

/*
 * storage_reset_uuid() - Reset configuration uuid in RAM with random numbers
 *
 * INPUT
 *     none
 * OUTPUT
 *     none
 *
 */
void storage_reset_uuid(void)
{
    // set random uuid
    random_buffer(shadow_config.meta.uuid, sizeof(shadow_config.meta.uuid));
    data2hex(shadow_config.meta.uuid, sizeof(shadow_config.meta.uuid),
             shadow_config.meta.uuid_str);
}

/*
 * storage_reset() - Clear configuration in RAM
 *
 * INPUT
 *     none
 * OUTPUT
 *     none
 */
void storage_reset(void)
{
    // reset storage
    memset(&shadow_config.storage, 0, sizeof(shadow_config.storage));

    dbg_print("pkhoo(%s) : pfa[%d] = 0x%x\n\r", __FUNCTION__, pfa_index, shadow_config.storage.pin_failed_attempts[pfa_index]);
    
    /* set pin_failed_attempts buffer with 0xFF for future update */
    memset(shadow_config.storage.pin_failed_attempts, 0xFF, 
                sizeof(shadow_config.storage.pin_failed_attempts));


    // reset cache
    memset(&shadow_config.cache, 0, sizeof(shadow_config.cache));

    shadow_config.storage.version = STORAGE_VERSION;
    session_clear(true); // clear PIN as well
}

/*
 * session_clear() - Reset session states
 *
 * INPUT
 *     - clear_pin: whether to clear pin or not
 * OUTPUT
 *     none
 */
void session_clear(bool clear_pin)
{
    sessionRootNodeCached = false;
    memset(&sessionRootNode, 0, sizeof(sessionRootNode));
    sessionPassphraseCached = false;
    memset(&sessionPassphrase, 0, sizeof(sessionPassphrase));

    if(clear_pin)
    {
        sessionPinCached = false;
    }
}

/*
 * storage_pfa_write() - write to pin_failed_attempts buffer
 *
 * INPUT
 *      none
 * OUTPUT
 *      none
 */
void storage_pfa_write(uint32_t index, uint8_t pfa)
{
    uint32_t offset;
    ConfigFlash *stor_addr;

    /* check end of buffer reached */
    if(index < PFA_BFR_SIZE - 1)
    {
        /* translate to physical address */
        stor_addr = (ConfigFlash *)flash_write_helper(storage_location);

        /* get offset of pin_failed_attempt[index] pointer in the structure */
        offset = offsetof(ConfigFlash, storage.pin_failed_attempts) + index;

        dbg_print("pkhoo(%s): stloc=%d (0x%x)\n\r",  __FUNCTION__, storage_location,
            &stor_addr->storage.pin_failed_attempts[index]);

        dbg_print("pkhoo(%s): offset = 0x%x, pfa = %d\n\r", __FUNCTION__, offset, pfa);

        flash_unlock();
        flash_write(storage_location, offset, 1, &pfa);
        flash_lock();
    }
    else
    {
        dbg_print("pkhoo(%s) ->storage_commit (pfa wrap - pfaindex = 0x%x)\n\r",  __FUNCTION__, index);
        storage_commit();
    }

}

/*
 * storage_commit() - Write content of configuration in shadow memory to
 * storage partion in flash
 *
 * INPUT
 *     none
 * OUTPUT
 *     none
 */
void storage_commit(void)
{
    uint32_t shadow_ram_crc32, shadow_flash_crc32, retries;

    memcpy((void *)&shadow_config, STORAGE_MAGIC_STR, STORAGE_MAGIC_LEN);

    dbg_print("%s :  %d\n\r", __FUNCTION__, pfa_index);
    re_index_pfa(&pfa_index);
    
    for(retries = 0; retries < STORAGE_RETRIES; retries++)
    {
        /* Capture CRC for verification at restore */
        shadow_ram_crc32 = calc_crc32((uint32_t *)&shadow_config,
                                      sizeof(shadow_config) / sizeof(uint32_t));

        if(shadow_ram_crc32 == 0)
        {
            continue; /* Retry */
        }

        /* Make sure flash is in good state before proceeding */
        if(!flash_chk_status())
        {
            flash_clear_status_flags();
            continue; /* Retry */
        }

        /* Make sure storage sector is valid before proceeding */
        if(storage_location < FLASH_STORAGE1 && storage_location > FLASH_STORAGE3)
        {
            /* Let it exhaust the retries and error out */
            continue;
        }

        flash_unlock();
        flash_erase_word(storage_location);
        wear_leveling_shift();


        flash_erase_word(storage_location);

        /* Load storage data first before loading storage magic  */
        if(flash_write_word(storage_location, STORAGE_MAGIC_LEN,
                            sizeof(shadow_config) - STORAGE_MAGIC_LEN,
                            (uint8_t *)&shadow_config + STORAGE_MAGIC_LEN))
        {
            if(!flash_write_word(storage_location, 0, STORAGE_MAGIC_LEN,
                                 (uint8_t *)&shadow_config))
            {
                continue; /* Retry */
            }
        }
        else
        {
            continue; /* Retry */
        }

        /* Flash write completed successfully.  Verify CRC */
        shadow_flash_crc32 = calc_crc32((uint32_t *)flash_write_helper(
                                            storage_location),
                                        sizeof(shadow_config) / sizeof(uint32_t));

        if(shadow_flash_crc32 == shadow_ram_crc32)
        {
            /* Commit successful, break to exit */
            break;
        }
        else
        {
            continue; /* Retry */
        }
    }

    flash_lock();

    if(retries >= STORAGE_RETRIES)
    {
        layout_warning_static("Error Detected.  Reboot Device!");
        system_halt();
    }
}

/*
 * storage_load_device() - Load configuration data from usb message to shadow memory
 *
 * INPUT
 *     - msg: load device message
 * OUTPUT
 *     none
 */
void storage_load_device(LoadDevice *msg)
{
    storage_reset();

    shadow_config.storage.has_imported = true;
    shadow_config.storage.imported = true;

    if(msg->has_pin > 0)
    {
        storage_set_pin(msg->pin);
    }

    if(msg->has_passphrase_protection)
    {
        shadow_config.storage.has_passphrase_protection = true;
        shadow_config.storage.passphrase_protection = msg->passphrase_protection;
    }
    else
    {
        shadow_config.storage.has_passphrase_protection = false;
    }

    if(msg->has_node)
    {
        shadow_config.storage.has_node = true;
        shadow_config.storage.has_mnemonic = false;
        memcpy(&shadow_config.storage.node, &(msg->node), sizeof(HDNodeType));
        sessionRootNodeCached = false;
        memset(&sessionRootNode, 0, sizeof(sessionRootNode));
    }
    else if(msg->has_mnemonic)
    {
        shadow_config.storage.has_mnemonic = true;
        shadow_config.storage.has_node = false;
        strlcpy(shadow_config.storage.mnemonic, msg->mnemonic,
                sizeof(shadow_config.storage.mnemonic));
        sessionRootNodeCached = false;
        memset(&sessionRootNode, 0, sizeof(sessionRootNode));
    }

    if(msg->has_language)
    {
        storage_set_language(msg->language);
    }

    if(msg->has_label)
    {
        storage_set_label(msg->label);
    }
}

/*
 * storage_set_label() - Set device label
 *
 * INPUT
 *     - label: label to set
 * OUTPUT
 *     none
 */
void storage_set_label(const char *label)
{
    if(!label) { return; }

    shadow_config.storage.has_label = true;
    memset(shadow_config.storage.label, 0, sizeof(shadow_config.storage.label));
    strlcpy(shadow_config.storage.label, label,
            sizeof(shadow_config.storage.label));
}

/*
 * storage_get_label() - Get device's label
 *
 * INPUT
 *     none
 * OUTPUT
 *     device's label
 *
 */
const char *storage_get_label(void)
{
    if(shadow_config.storage.has_label)
    {
        return shadow_config.storage.label;
    }
    else
    {
        return NULL;
    }
}

/*
 * storage_set_language() - Set device language
 *
 * INPUT
 *     - lang: language to apply
 * OUTPUT
 *     none
 */
void storage_set_language(const char *lang)
{
    if(!lang) { return; }

    // sanity check
    if(strcmp(lang, "english") == 0)
    {
        shadow_config.storage.has_language = true;
        memset(shadow_config.storage.language, 0,
               sizeof(shadow_config.storage.language));
        strlcpy(shadow_config.storage.language, lang,
                sizeof(shadow_config.storage.language));
    }
}

/*
 * storage_get_language() - Get device's language
 *
 * INPUT
 *     none
 * OUTPUT
 *     device's language
 */
const char *storage_get_language(void)
{
    if(shadow_config.storage.has_language)
    {
        return shadow_config.storage.language;
    }
    else
    {
        return NULL;
    }
}

/*
 * storage_is_pin_correct() - Validates PIN
 *
 * INPUT
 *     - pin: PIN to validate
 * OUTPUT
 *     true/false whether PIN is correct
 */
bool storage_is_pin_correct(const char *pin)
{
    return strcmp(shadow_config.storage.pin, pin) == 0;
}

/*
 * storage_has_pin() - Determines whther device has PIN
 *
 * INPUT
 *     none
 * OUTPUT
 *     true/false whether device has a PIN
 */
bool storage_has_pin(void)
{
    return shadow_config.storage.has_pin && strlen(shadow_config.storage.pin) > 0;
}

/*
 * storage_set_pin() - Save PIN
 *
 * INPUT
 *     - pin: PIN to save
 * OUTPUT
 *     none
 */
void storage_set_pin(const char *pin)
{
    if(pin && strlen(pin) > 0)
    {
        shadow_config.storage.has_pin = true;
        strlcpy(shadow_config.storage.pin, pin, sizeof(shadow_config.storage.pin));
        session_cache_pin(pin);
    }
    else
    {
        shadow_config.storage.has_pin = false;
        memset(shadow_config.storage.pin, 0, sizeof(shadow_config.storage.pin));
        sessionPinCached = false;
    }
}

/*
 * storage_get_pin() - Returns PIN
 *
 * INPUT
 *     none
 * OUTPUT
 *     device's PIN
 */
const char *storage_get_pin(void)
{
    return (shadow_config.storage.has_pin) ? shadow_config.storage.pin : NULL;
}

/*
 * session_cache_pin() - Save pin in session cache
 *
 * INPUT
 *     - pin: PIN to save to session cache
 * OUTPUT
 *     none
 */
void session_cache_pin(const char *pin)
{
    strlcpy(sessionPin, pin, sizeof(sessionPin));
    sessionPinCached = true;
}

/*
 * session_is_pin_cached() - Is PIN cached in session
 *
 * INPUT
 *     none
 * OUTPUT
 *     true/false whether PIN is cached in session
 *
 */
bool session_is_pin_cached(void)
{
    return sessionPinCached && strcmp(sessionPin, shadow_config.storage.pin) == 0;
}

/*
 * storage_reset_pin_fails() - Reset PIN failures
 *
 * INPUT
 *     none
 * OUTPUT
 *     none
 */
void storage_reset_pin_fails(void)
{
    /* Only write to flash if there's a change in status */
    if(shadow_config.storage.has_pin_failed_attempts == true)
    {
        shadow_config.storage.has_pin_failed_attempts = false;

        /* don't advance pfa_index for 1st entry */
        if(pfa_index || shadow_config.storage.pin_failed_attempts[pfa_index] != 0xFF)
        {
            pfa_index++;
        }
        shadow_config.storage.pin_failed_attempts[pfa_index] = 0;

        dbg_print("pkhoo(%s): [%d] = %d\n\r", __FUNCTION__, pfa_index,
                shadow_config.storage.pin_failed_attempts[pfa_index]);

        storage_pfa_write(pfa_index, shadow_config.storage.pin_failed_attempts[pfa_index]);
    }
}

/*
 * storage_increase_pin_fails() - Increment PIN failed attempts
 *
 * INPUT
 *     none
 * OUTPUT
 *     none
 */
void storage_increase_pin_fails(void)
{
    char tvar;


    if(!shadow_config.storage.has_pin_failed_attempts)
    {

        /* advance index for initialized buffer */
        if(pfa_index || shadow_config.storage.pin_failed_attempts[pfa_index] != 0xFF)
        {
            pfa_index++;
        }
        shadow_config.storage.has_pin_failed_attempts = true;
        shadow_config.storage.pin_failed_attempts[pfa_index] = 1;
    }
    else
    {
        tvar = shadow_config.storage.pin_failed_attempts[pfa_index] + 1;
        shadow_config.storage.pin_failed_attempts[++pfa_index] = tvar;

    }

    storage_pfa_write(pfa_index, shadow_config.storage.pin_failed_attempts[pfa_index]);

    dbg_print("pkhoo(%s): [%d]=%d\n\r", __FUNCTION__,
                pfa_index,
                shadow_config.storage.pin_failed_attempts[pfa_index]);
}

/*
 * storage_get_pin_fails() - Get number PIN failures
 *
 * INPUT
 *     none
 * OUTPOUT
 *     number of PIN failures
 */
uint32_t storage_get_pin_fails(void)
{
#if 1 //pkhoo
    dbg_print("pkhoo(%s): [%d]=%d\n\r", __FUNCTION__, pfa_index, 
            shadow_config.storage.has_pin_failed_attempts ?  
            shadow_config.storage.pin_failed_attempts[pfa_index] : 0);

    return shadow_config.storage.has_pin_failed_attempts ?  shadow_config.storage.pin_failed_attempts[pfa_index] : 0;
#else
    return shadow_config.storage.has_pin_failed_attempts ?  shadow_config.storage.pin_failed_attempts : 0;
#endif
}

/*
 * get_root_node_callback() - Calls animation callback
 *
 * INPUT
 *     - iter: current iteration
 *     - total: total iterations
 * OUTPUT
 *     none
 */
void get_root_node_callback(uint32_t iter, uint32_t total)
{
    (void)iter;
    (void)total;
    animating_progress_handler();
}

/*
 * storage_get_root_node() - Returns root node of device
 *
 * INPUT
 *     - node: where to put the node that is found
 * OUTPUT
 *     true/false whether root node was found
 */
bool storage_get_root_node(HDNode *node)
{
    // root node is properly cached
    if(sessionRootNodeCached)
    {
        memcpy(node, &sessionRootNode, sizeof(HDNode));
        return true;
    }

    // if storage has node, decrypt and use it
    if(shadow_config.storage.has_node)
    {
        if(!passphrase_protect())
        {
            return false;
        }

        layout_loading();

        if(hdnode_from_xprv(shadow_config.storage.node.depth,
                            shadow_config.storage.node.fingerprint,
                            shadow_config.storage.node.child_num,
                            shadow_config.storage.node.chain_code.bytes,
                            shadow_config.storage.node.private_key.bytes,
                            &sessionRootNode) == 0)
        {
            return false;
        }

        if(shadow_config.storage.has_passphrase_protection &&
                shadow_config.storage.passphrase_protection && strlen(sessionPassphrase))
        {
            // decrypt hd node
            uint8_t secret[64];

            /* Length of salt + 4 bytes are needed as workspace by pbkdf2_hmac_sha512 */
            uint8_t salt[strlen(PBKDF2_HMAC_SHA512_SALT) + 4];
            memcpy((char *)salt, PBKDF2_HMAC_SHA512_SALT, strlen(PBKDF2_HMAC_SHA512_SALT));

            animating_progress_handler();

            pbkdf2_hmac_sha512((const uint8_t *)sessionPassphrase,
                               strlen(sessionPassphrase),
                               salt, strlen(PBKDF2_HMAC_SHA512_SALT), BIP39_PBKDF2_ROUNDS, secret, 64,
                               get_root_node_callback);

            aes_decrypt_ctx ctx;
            aes_decrypt_key256(secret, &ctx);
            aes_cbc_decrypt(sessionRootNode.chain_code, sessionRootNode.chain_code, 32,
                            secret + 32,
                            &ctx);
            aes_cbc_decrypt(sessionRootNode.private_key, sessionRootNode.private_key, 32,
                            secret + 32,
                            &ctx);
        }

        memcpy(node, &sessionRootNode, sizeof(HDNode));
        sessionRootNodeCached = true;
        return true;
    }

    // if storage has mnemonic, convert it to node and use it
    if(shadow_config.storage.has_mnemonic)
    {
        if(!passphrase_protect())
        {
            return false;
        }

        if(storage_get_root_node_cache(node))
        {
            return true;
        }

        layout_loading();

        uint8_t seed[64];

        animating_progress_handler();

        mnemonic_to_seed(shadow_config.storage.mnemonic, sessionPassphrase, seed,
                         get_root_node_callback); // BIP-0039

        if(hdnode_from_seed(seed, sizeof(seed), &sessionRootNode) == 0)
        {
            return false;
        }

        storage_set_root_node_cache(&sessionRootNode);

        memcpy(node, &sessionRootNode, sizeof(HDNode));
        sessionRootNodeCached = true;
        return true;
    }

    return false;
}

/*
 * storage_isInitialized() - Is device initialized?
 *
 * INPUT
 *     none
 * OUTPUT
 *     true/false wether device is initialized
 *
 *
 */
bool storage_is_initialized(void)
{
    return shadow_config.storage.has_node || shadow_config.storage.has_mnemonic;
}

/*
 * storage_get_uuid_str() - Get device's UUID
 *
 * INPUT
 *     none
 * OUTPUT
 *     device's UUID
 */
const char *storage_get_uuid_str(void)
{
    return shadow_config.meta.uuid_str;
}

/*
 * storage_get_passphrase_protected() - Get passphrase protection status
 *
 * INPUT
 *     none
 * OUTPUT
 *     true/false whether device is passphrase protected
 *
 */
bool storage_get_passphrase_protected(void)
{
    if(shadow_config.storage.has_passphrase_protection)
    {
        return shadow_config.storage.passphrase_protection;
    }
    else
    {
        return false;
    }
}

/*
 * storage_set_passphrase_protected() - Set passphrase protection
 *
 * INPUT
 *     - p: state of passphrase protection to set
 * OUTPUT
 *     none
 *
 */
void storage_set_passphrase_protected(bool passphrase)
{
    shadow_config.storage.has_passphrase_protection = true;
    shadow_config.storage.passphrase_protection = passphrase;
}

/*
 * session_cachePassphrase() - Set session passphrase
 *
 * INPUT
 *     - passphrase: passphrase to set for session
 * OUTPUT
 *     none
 */
void session_cache_passphrase(const char *passphrase)
{
    strlcpy(sessionPassphrase, passphrase, sizeof(sessionPassphrase));
    sessionPassphraseCached = true;
}

/*
 * session_isPassphraseCached() - Returns whether there is a cached passphrase
 *
 * INPUT
 *     none
 * OUTPUT
 *     true/false session passphrase cache status
 */
bool session_is_passphrase_cached(void)
{
    return sessionPassphraseCached;
}

/*
 * storage_set_mnemonic_from_words() - Set config mnemonic in shadow memory from words
 *
 * INPUT
 *     - words: mnemonic
 *     - word_count: how words in mnemonic
 * OUTPUT
 *     none
 */
void storage_set_mnemonic_from_words(const char (*words)[12],
                                     unsigned int word_count)
{
    strlcpy(shadow_config.storage.mnemonic, words[0],
            sizeof(shadow_config.storage.mnemonic));

    for(uint32_t i = 1; i < word_count; i++)
    {
        strlcat(shadow_config.storage.mnemonic, " ",
                sizeof(shadow_config.storage.mnemonic));
        strlcat(shadow_config.storage.mnemonic, words[i],
                sizeof(shadow_config.storage.mnemonic));
    }

    shadow_config.storage.has_mnemonic = true;
}

/*
 * storage_set_mnemonic() - Set config mnemonic in shadow memory
 *
 * INPUT
 *     - m: mnemonic to set in shadow memory
 * OUTPUT
 *     none
 *
 */
void storage_set_mnemonic(const char *m)
{
    memset(shadow_config.storage.mnemonic, 0,
           sizeof(shadow_config.storage.mnemonic));
    strlcpy(shadow_config.storage.mnemonic, m,
            sizeof(shadow_config.storage.mnemonic));
    shadow_config.storage.has_mnemonic = true;
}

/*
 * storage_has_mnemonic() - Does device have mnemonic?
 *
 * INPUT
 *     none
 * OUTPUT
 *     true/false whether device has mnemonic
 *
 */
bool storage_has_mnemonic(void)
{
    return shadow_config.storage.has_mnemonic;
}

/*
 * storage_get_mnemonic() - Get mnemonic from flash
 *
 * INPUT
 *     none
 * OUTPUT
 *     mnemonic from storage
 *
 */
const char *storage_get_mnemonic(void)
{
    return shadow_config.storage.mnemonic;
}

/*
 * storage_get_shadow_mnemonic() - Get mnemonic from shadow memory
 *
 * INPUT
 *     none
 * OUTPUT
 *     mnemonic from shadow memory
 */
const char *storage_get_shadow_mnemonic(void)
{
    return shadow_config.storage.mnemonic;
}

/*
 * storage_get_imported() - Whether private key stored on device was imported
 *
 * INPUT
 *     none
 * OUTPUT
 *     true/false whether private key was imported
 */
bool storage_get_imported(void)
{
    return shadow_config.storage.has_imported && shadow_config.storage.imported;
}

/*
 * storage_has_node() - Does device have an HDNode
 *
 * INPUT
 *     none
 * OUTPUT
 *     true/false whether device has HDNode
 */
bool storage_has_node(void)
{
    return shadow_config.storage.has_node;
}

/*
 * storage_get_node() - Get HDNode
 *
 * INPUT
 *     none
 * OUTPUT
 *     HDNode from storage
 */
HDNodeType *storage_get_node(void)
{
    return &shadow_config.storage.node;
}

/*
 * get_storage_location() - Get storage data start address
 *
 * INPUT -
 *      none
 * OUTPUT -
 *      none
 *
 */
Allocation get_storage_location(void)
{
    return(storage_location);
}


/*
 * re_index_pfa() - Reset index for pin_failed_attempts[] for wearlevel shift
 *
 * INPUT
 *      porinter to pfa currect index
 * OUTPUT
 *      none
 */

void re_index_pfa(uint32_t *index)
{
    char tvar;

    dbg_print("pkhoo(%s): [%d] = 0x%x\n\r", __FUNCTION__, *index, shadow_config.storage.pin_failed_attempts[*index]);
    tvar = shadow_config.storage.pin_failed_attempts[*index];
    if(tvar > 0 && tvar < 0xFF)
    {
        shadow_config.storage.has_pin_failed_attempts = true;
    }
    /* set pin_failed_attempts bits to 1's for future update */
    memset(shadow_config.storage.pin_failed_attempts, 0xFF, 
                sizeof(shadow_config.storage.pin_failed_attempts));

    /*reset the index for next sector */
    *index = 0;
    shadow_config.storage.pin_failed_attempts[*index] = tvar;

}

/*
 * find_pfa_end - Find last "pin_failed_attempts" in the link list buffer 
 *
 * INPUT -
 *      none
 * OUTPUT -
 *      none
 *
 */
uint32_t find_pfa_end(void)
{
    uint32_t loc_index, ret_index;
    uint8_t *start_addr = (uint8_t *)shadow_config.storage.pin_failed_attempts;

    /*find last entry in the list */
    for(ret_index = loc_index = 0; loc_index < sizeof(shadow_config.storage.pin_failed_attempts); loc_index++)
    {
        dbg_print("pkhoo(%s): 0x%x (%d : %d) = 0%x\n\r", __FUNCTION__, 
                start_addr, loc_index, ret_index, *(uint8_t *)(start_addr + loc_index));

        /* verify the loc_index is beyond assigned length */
        if(*(uint8_t *)(start_addr + loc_index) == 0xFF)
        {
            /* error: found end of pin buffer*/
            break;
        }
        ret_index = loc_index;
    }
    if(loc_index >= sizeof(shadow_config.storage.pin_failed_attempts))
    {
        ret_index = 0xFF;
    }
    dbg_print("pkhoo(%s): pfa_index = %d\n\r", __FUNCTION__, ret_index);
    return(ret_index);

}

/*
 * update_pfa_stat() - update PIN failed attempt status
 *
 * INPUT -
 *      none
 * OUTPUT -
 *      update pfa index and pin failed attempts status in shadow_config memory
 *
 */
void update_pfa_stat(void)
{
    /* update pfa_index */
    pfa_index  = find_pfa_end();

    dbg_print("pkhoo(%s): pfa_index = %d\n\r", __FUNCTION__, pfa_index);

    /* update pfa status */
    if(pfa_index == 0)
    {
        if(shadow_config.storage.pin_failed_attempts[pfa_index] == 0 || 
            shadow_config.storage.pin_failed_attempts[pfa_index] == 0xFF)
        {
            shadow_config.storage.has_pin_failed_attempts = false; 
        }
    }
    else
    {
        shadow_config.storage.has_pin_failed_attempts = true;
    }

}
