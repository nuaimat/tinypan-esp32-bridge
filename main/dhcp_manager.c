/* DHCP reservation management with NVS persistence.
 *
 * Manages fixed IP address assignments for clients based on MAC address,
 * with optional device names for user-friendly identification.
 */

#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/ip4_addr.h"
#include "dhcpserver/dhcpserver.h"
#include "esp_wifi.h"
#include "router_config.h"
#include "dhcp_reservations.h"
#include "wifi_config.h"
#include "nvs_config.h"

#define DHCP_RES_SIZE (sizeof(struct dhcp_reservation_entry) * MAX_DHCP_RESERVATIONS)

static const char *TAG = "dhcp_mgr";

/* Serializes access to the shared dhcp_reservations[] table.
 * Writers run on the HTTP/console tasks; readers run on the lwIP tcpip
 * thread (DHCP server lookups). Created during get_dhcp_reservations(),
 * which main.c calls at boot before any reader can run. */
static SemaphoreHandle_t s_reservations_mutex = NULL;

static inline void reservations_lock(void)
{
    if (s_reservations_mutex != NULL) {
        xSemaphoreTake(s_reservations_mutex, portMAX_DELAY);
    }
}

static inline void reservations_unlock(void)
{
    if (s_reservations_mutex != NULL) {
        xSemaphoreGive(s_reservations_mutex);
    }
}

esp_err_t get_dhcp_reservations() {
    esp_err_t err;
    nvs_handle_t nvs;
    size_t len;

    if (s_reservations_mutex == NULL) {
        s_reservations_mutex = xSemaphoreCreateMutex();
        if (s_reservations_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_blob(nvs, "dhcp_res", NULL, &len);
    if (err == ESP_OK) {
        /* Boot-time-only path: the lock is only held for the NVS read itself. */
        reservations_lock();
        if (len != DHCP_RES_SIZE) {
            /* Stored blob has wrong size — reset to avoid stale data */
            memset(dhcp_reservations, 0, DHCP_RES_SIZE);
            err = ESP_ERR_NVS_INVALID_LENGTH;
        } else if ((err = nvs_get_blob(nvs, "dhcp_res", dhcp_reservations, &len)) != ESP_OK) {
            memset(dhcp_reservations, 0, DHCP_RES_SIZE);
        }
        reservations_unlock();
    }
    nvs_close(nvs);

    return err;
}

void print_dhcp_reservations() {
    reservations_lock();
    for (int i = 0; i < MAX_DHCP_RESERVATIONS; i++) {
        if (dhcp_reservations[i].valid) {
            printf("%02X:%02X:%02X:%02X:%02X:%02X -> ",
                dhcp_reservations[i].mac[0], dhcp_reservations[i].mac[1],
                dhcp_reservations[i].mac[2], dhcp_reservations[i].mac[3],
                dhcp_reservations[i].mac[4], dhcp_reservations[i].mac[5]);
            if (dhcp_reservations[i].ip == 0) {
                printf("BLOCKED");
            } else {
                ip4_addr_t addr;
                addr.addr = dhcp_reservations[i].ip;
                printf(IPSTR, IP2STR(&addr));
            }
            if (dhcp_reservations[i].name[0] != '\0') {
                printf(" (%s)", dhcp_reservations[i].name);
            }
            printf("\n");
        }
    }
    reservations_unlock();
}

void get_dhcp_pool_range(uint32_t server_ip, uint32_t *start_ip, uint32_t *end_ip) {
    // DHCP pool calculation logic (mirrors dhcps_poll_set in dhcpserver.c)
    // Default netmask is 255.255.255.0
    uint32_t netmask = 0xFFFFFF00; // 255.255.255.0 in host byte order
    uint32_t server_ip_host = ntohl(server_ip);
    uint32_t range_start_ip = server_ip_host & netmask;
    uint32_t range_end_ip = range_start_ip | ~netmask;

    // Determine which side of the server IP has more addresses
    if (server_ip_host - range_start_ip > range_end_ip - server_ip_host) {
        // More addresses before server IP
        range_start_ip = range_start_ip + 1;
        range_end_ip = server_ip_host - 1;
    } else {
        // More addresses after server IP
        range_start_ip = server_ip_host + 1;
        range_end_ip = range_end_ip - 1;
    }

    // Limit to DHCPS_MAX_LEASE (100 addresses) - already defined in dhcpserver.h
    if (range_end_ip - range_start_ip + 1 > DHCPS_MAX_LEASE) {
        range_end_ip = range_start_ip + DHCPS_MAX_LEASE - 1;
    }

    *start_ip = htonl(range_start_ip);
    *end_ip = htonl(range_end_ip);
}

void print_dhcp_pool() {
    uint32_t start_ip, end_ip;
    get_dhcp_pool_range(my_ap_ip, &start_ip, &end_ip);

    ip4_addr_t start_addr, end_addr;
    start_addr.addr = start_ip;
    end_addr.addr = end_ip;

    printf("DHCP Pool: " IPSTR " - " IPSTR " (%lu addresses)\n",
        IP2STR(&start_addr), IP2STR(&end_addr),
        (unsigned long)(ntohl(end_ip) - ntohl(start_ip) + 1));
}

esp_err_t add_dhcp_reservation(const uint8_t *mac, uint32_t ip, const char *name) {
    esp_err_t err;
    struct dhcp_reservation_entry snapshot[MAX_DHCP_RESERVATIONS];

    reservations_lock();
    // Check if MAC already exists and update it
    for (int i = 0; i < MAX_DHCP_RESERVATIONS; i++) {
        if (dhcp_reservations[i].valid &&
            memcmp(dhcp_reservations[i].mac, mac, 6) == 0) {
            // Update existing entry
            dhcp_reservations[i].ip = ip;
            if (name != NULL) {
                strncpy(dhcp_reservations[i].name, name, DHCP_RESERVATION_NAME_LEN - 1);
                dhcp_reservations[i].name[DHCP_RESERVATION_NAME_LEN - 1] = '\0';
            } else {
                dhcp_reservations[i].name[0] = '\0';
            }
            memcpy(snapshot, dhcp_reservations, sizeof(snapshot));
            reservations_unlock();
            goto save;
        }
    }

    // Find first empty slot
    for (int i = 0; i < MAX_DHCP_RESERVATIONS; i++) {
        if (!dhcp_reservations[i].valid) {
            memcpy(dhcp_reservations[i].mac, mac, 6);
            dhcp_reservations[i].ip = ip;
            if (name != NULL) {
                strncpy(dhcp_reservations[i].name, name, DHCP_RESERVATION_NAME_LEN - 1);
                dhcp_reservations[i].name[DHCP_RESERVATION_NAME_LEN - 1] = '\0';
            } else {
                dhcp_reservations[i].name[0] = '\0';
            }
            dhcp_reservations[i].valid = 1;
            memcpy(snapshot, dhcp_reservations, sizeof(snapshot));
            reservations_unlock();
            goto save;
        }
    }
    reservations_unlock();
    return ESP_ERR_NO_MEM;

save:
    err = set_config_param_blob("dhcp_res", snapshot, sizeof(snapshot));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "DHCP reservations stored.");
    }
    return err;
}

esp_err_t del_dhcp_reservation(const uint8_t *mac) {
    struct dhcp_reservation_entry snapshot[MAX_DHCP_RESERVATIONS];
    bool found = false;

    reservations_lock();
    for (int i = 0; i < MAX_DHCP_RESERVATIONS; i++) {
        if (dhcp_reservations[i].valid &&
            memcmp(dhcp_reservations[i].mac, mac, 6) == 0) {
            dhcp_reservations[i].valid = 0;
            found = true;
            break;
        }
    }
    memcpy(snapshot, dhcp_reservations, sizeof(snapshot));
    reservations_unlock();

    if (!found) {
        return ESP_OK;
    }

    esp_err_t err = set_config_param_blob("dhcp_res", snapshot, sizeof(snapshot));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "DHCP reservations stored.");
    }
    return err;
}

esp_err_t clear_all_dhcp_reservations() {
    struct dhcp_reservation_entry snapshot[MAX_DHCP_RESERVATIONS];

    reservations_lock();
    // Clear all reservation entries
    for (int i = 0; i < MAX_DHCP_RESERVATIONS; i++) {
        dhcp_reservations[i].valid = 0;
    }
    memcpy(snapshot, dhcp_reservations, sizeof(snapshot));
    reservations_unlock();

    // Save cleared table to NVS
    esp_err_t err = set_config_param_blob("dhcp_res", snapshot, sizeof(snapshot));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "All DHCP reservations cleared.");
    }
    return err;
}

uint32_t lookup_dhcp_reservation(const uint8_t *mac) {
    uint32_t result = 0;
    reservations_lock();
    for (int i = 0; i < MAX_DHCP_RESERVATIONS; i++) {
        if (dhcp_reservations[i].valid &&
            memcmp(dhcp_reservations[i].mac, mac, 6) == 0) {
            result = dhcp_reservations[i].ip;
            break;
        }
    }
    reservations_unlock();
    return result;
}

bool is_ip_reserved_for_other(uint32_t ip, const uint8_t *mac) {
    if (ip == 0) return false;
    bool result = false;
    reservations_lock();
    for (int i = 0; i < MAX_DHCP_RESERVATIONS; i++) {
        if (dhcp_reservations[i].valid &&
            dhcp_reservations[i].ip == ip &&
            memcmp(dhcp_reservations[i].mac, mac, 6) != 0) {
            result = true;
            break;
        }
    }
    reservations_unlock();
    return result;
}

bool is_mac_blocked(const uint8_t *mac) {
    bool result = false;
    reservations_lock();
    for (int i = 0; i < MAX_DHCP_RESERVATIONS; i++) {
        if (dhcp_reservations[i].valid &&
            memcmp(dhcp_reservations[i].mac, mac, 6) == 0) {
            result = (dhcp_reservations[i].ip == 0);
            break;
        }
    }
    reservations_unlock();
    return result;
}

const char* lookup_device_name_by_ip(uint32_t ip) {
    for (int i = 0; i < MAX_DHCP_RESERVATIONS; i++) {
        if (dhcp_reservations[i].valid &&
            dhcp_reservations[i].ip == ip &&
            dhcp_reservations[i].name[0] != '\0') {
            return dhcp_reservations[i].name;
        }
    }
    return NULL;
}

const char* lookup_device_name_by_mac(const uint8_t *mac) {
    for (int i = 0; i < MAX_DHCP_RESERVATIONS; i++) {
        if (dhcp_reservations[i].valid &&
            memcmp(dhcp_reservations[i].mac, mac, 6) == 0 &&
            dhcp_reservations[i].name[0] != '\0') {
            return dhcp_reservations[i].name;
        }
    }
    return NULL;
}

bool resolve_device_name_to_ip(const char *name, uint32_t *ip) {
    if (name == NULL || ip == NULL) {
        return false;
    }
    bool result = false;
    uint32_t found_ip = 0;
    reservations_lock();
    for (int i = 0; i < MAX_DHCP_RESERVATIONS; i++) {
        if (dhcp_reservations[i].valid &&
            dhcp_reservations[i].name[0] != '\0' &&
            strcasecmp(dhcp_reservations[i].name, name) == 0) {
            found_ip = dhcp_reservations[i].ip;
            result = true;
            break;
        }
    }
    reservations_unlock();
    *ip = found_ip;
    return result;
}

int get_connected_clients(connected_client_t *clients, int max_clients) {
    if (clients == NULL || max_clients <= 0) {
        return 0;
    }

    // Get list of connected WiFi stations
    wifi_sta_list_t sta_list;
    esp_err_t err = esp_wifi_ap_get_sta_list(&sta_list);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get station list: %s", esp_err_to_name(err));
        return 0;
    }

    // Get DHCP lease information (static to avoid stack overflow)
    #define MAX_DHCP_LEASES 8  // ESP32 AP supports max 8 connections
    static dhcp_lease_info_t leases[MAX_DHCP_LEASES];
    int lease_count = dhcps_get_active_leases(leases, MAX_DHCP_LEASES);

    int count = 0;
    for (int i = 0; i < sta_list.num && count < max_clients; i++) {
        wifi_sta_info_t *sta = &sta_list.sta[i];

        // Copy MAC address
        memcpy(clients[count].mac, sta->mac, 6);
        clients[count].ip = 0;
        clients[count].has_ip = false;
        clients[count].name[0] = '\0';

        // Look up IP address and hostname in DHCP leases
        for (int j = 0; j < lease_count; j++) {
            if (memcmp(leases[j].mac, sta->mac, 6) == 0) {
                clients[count].ip = leases[j].ip;
                clients[count].has_ip = true;
                // Use DHCP hostname if provided by client
                if (leases[j].hostname[0] != '\0') {
                    strncpy(clients[count].name, leases[j].hostname,
                            DHCP_RESERVATION_NAME_LEN - 1);
                    clients[count].name[DHCP_RESERVATION_NAME_LEN - 1] = '\0';
                }
                break;
            }
        }

        // Look up device name in DHCP reservations (overrides DHCP hostname)
        // Reads the shared reservation table, so serialize with the mutex.
        reservations_lock();
        for (int j = 0; j < MAX_DHCP_RESERVATIONS; j++) {
            if (dhcp_reservations[j].valid &&
                memcmp(dhcp_reservations[j].mac, sta->mac, 6) == 0) {
                // Reservation name takes priority over DHCP hostname
                if (dhcp_reservations[j].name[0] != '\0') {
                    strncpy(clients[count].name, dhcp_reservations[j].name,
                            DHCP_RESERVATION_NAME_LEN - 1);
                    clients[count].name[DHCP_RESERVATION_NAME_LEN - 1] = '\0';
                }
                break;
            }
        }
        reservations_unlock();

        count++;
    }

    return count;
}
