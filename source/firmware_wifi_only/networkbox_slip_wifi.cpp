#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "pico/flash.h"
#include "wifi_config.h"
#include "dhcpserver.h"
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include "lwip/init.h"
#include "lwip/ip.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "lwip/tcp.h"
extern "C" {
#include "wizchip_conf.h"
#include "socket.h"
#include "w5x00_spi.h"
#include "w5x00_lwip.h"
}


// UART defines
// By default the stdout UART is `uart0`, so we will use the second one
#define UART_ID uart0
#define BAUD_RATE 19200

// Use pins 4 and 5 for UART1
// Pins can be changed, see the GPIO function select table in the datasheet for information on GPIO assignments
#define UART_TX_PIN 0
#define UART_RX_PIN 1

// Bouton de sélection : GP4 relié à GND quand il est appuyé.
// Une résistance de tirage interne maintient l'entrée à 1 au repos.
#define MODE_BUTTON_PIN 4

// LEDs : vert = activité, blanc = RJ45, rouge = Wi-Fi.
#define LED_ACTIVITY_PIN 5
#define LED_ETHERNET_PIN 6
#define LED_WIFI_PIN 7

static constexpr uint8_t SLIP_END = 0xC0;
static constexpr uint8_t SLIP_ESC = 0xDB;
static constexpr uint8_t SLIP_ESC_END = 0xDC;
static constexpr uint8_t SLIP_ESC_ESC = 0xDD;
static constexpr size_t SLIP_MAX_FRAME = 1600;
// UART ISR buffers incoming bytes while lwIP/Wi-Fi callbacks are busy.
static constexpr uint32_t UART_RX_RING_SIZE = 4096;
static constexpr uint32_t UART_RX_RING_MASK = UART_RX_RING_SIZE - 1;
static_assert((UART_RX_RING_SIZE & UART_RX_RING_MASK) == 0,
              "UART RX ring size must be a power of two");
static uint8_t uart_rx_ring[UART_RX_RING_SIZE];
static volatile uint32_t uart_rx_head = 0;
static volatile uint32_t uart_rx_tail = 0;
static volatile uint32_t uart_rx_overflow = 0;

static uint8_t slip_frame[SLIP_MAX_FRAME];
static size_t slip_length = 0;
static bool slip_in_frame = false;
static bool slip_escaped = false;
static struct netif slip_netif;
static struct netif ethernet_netif;
static struct netif *active_network_netif = nullptr;
static bool ethernet_active = false;
static uint8_t ethernet_mac[6] = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56};
static netif_output_fn original_wifi_output = nullptr;
static netif_input_fn original_wifi_input = nullptr;
static bool nat_icmp_active = false;
static uint8_t nat_icmp_client_ip[4] = {0};
static uint16_t nat_icmp_id = 0;
static uint16_t nat_icmp_sequence = 0;
static bool nat_dns_active = false;
static uint8_t nat_dns_client_ip[4] = {0};
static uint8_t nat_dns_server_ip[4] = {0};
static uint16_t nat_dns_client_port = 0;
static uint16_t nat_dns_transaction_id = 0;
// FTP clients such as Litchi briefly keep several control and data channels
// alive at once. Eight entries were too few during a transfer.
static constexpr size_t NAT_TCP_FLOW_COUNT = 16;
struct NatTcpFlow {
    bool active;
    bool client_fin_seen;
    bool server_fin_seen;
    bool reset_seen;
    uint8_t client_ip[4];
    uint8_t server_ip[4];
    uint16_t client_port;
    uint16_t server_port;
    uint32_t last_activity;
    uint32_t closed_at;
};
static NatTcpFlow nat_tcp_flows[NAT_TCP_FLOW_COUNT] = {};
static uint16_t checksum16(const uint8_t *data, size_t length);
static void slip_send_frame(const uint8_t *data, size_t length);
static err_t wifi_output_trace(struct netif *netif, struct pbuf *p,
                               const ip4_addr_t *ipaddr);
static err_t wifi_input_trace(struct pbuf *p, struct netif *netif);

// ---------------------------------------------------------------------------
// Persistent Wi-Fi configuration and the temporary NetworkBox-Setup web page.
// The last flash sector is deliberately reserved for this small record. UF2
// program images do not contain this sector, so ordinary firmware updates keep
// the saved credentials. A factory-reset feature can erase it later.
// ---------------------------------------------------------------------------
static constexpr uint32_t WIFI_CONFIG_MAGIC = 0x4e425746; // "NBWF"
static constexpr uint16_t WIFI_CONFIG_VERSION = 1;
static constexpr uint32_t WIFI_CONFIG_FLASH_OFFSET =
    // The Pico SDK places mandatory RP2350 metadata in the final flash sector.
    // Keep settings one full sector below it so an UF2 update never overwrites
    // saved Wi-Fi credentials.
    PICO_FLASH_SIZE_BYTES - (2 * FLASH_SECTOR_SIZE);
static constexpr char SETUP_AP_SSID[] = "NetworkBox-Setup";
static constexpr char SETUP_AP_PASSWORD[] = "networkbox";

struct WifiCredentials {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    char ssid[33];       // IEEE 802.11 maximum: 32 bytes plus NUL.
    char password[65];   // WPA2 maximum: 63 bytes plus NUL.
    uint32_t checksum;
};
static_assert(sizeof(WifiCredentials) <= FLASH_PAGE_SIZE,
              "Wi-Fi credentials must fit in one flash page");

static bool station_connected = false;
static bool setup_mode = false;
static uint32_t reboot_at_ms = 0;
static uint32_t button_press_started_ms = 0;
static bool button_long_press_armed = true;
static uint32_t activity_led_until_ms = 0;
static struct tcp_pcb *setup_server = nullptr;
static dhcp_server_t setup_dhcp_server = {};
static uint8_t ethernet_rx_frame[ETHERNET_MTU + 64];
static constexpr size_t WIFI_SCAN_MAX_NETWORKS = 10;
struct WifiScanNetwork { char ssid[33]; int16_t rssi; };
static WifiScanNetwork wifi_scan_networks[WIFI_SCAN_MAX_NETWORKS] = {};
static size_t wifi_scan_network_count = 0;

static void init_status_leds() {
    const uint8_t led_pins[] = {LED_ACTIVITY_PIN, LED_ETHERNET_PIN, LED_WIFI_PIN};
    for (uint8_t pin : led_pins) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_put(pin, 0);
    }
}

static void mark_network_activity() {
    gpio_put(LED_ACTIVITY_PIN, 1);
    activity_led_until_ms = to_ms_since_boot(get_absolute_time()) + 80;
}

static void update_status_leds(uint32_t now) {
    gpio_put(LED_ETHERNET_PIN, ethernet_active ? 1 : 0);
    if (setup_mode) {
        // Clignotement visible, environ 2 fois par seconde.
        gpio_put(LED_WIFI_PIN, ((now / 250) & 1u) ? 1 : 0);
    } else {
        gpio_put(LED_WIFI_PIN, station_connected ? 1 : 0);
    }
    if (activity_led_until_ms && now >= activity_led_until_ms) {
        gpio_put(LED_ACTIVITY_PIN, 0);
        activity_led_until_ms = 0;
    }
}

// Initialise le W5500 uniquement si un câble RJ45 est déjà relié au démarrage.
// Le Wi-Fi reste alors le repli automatique. Le futur bouton choisira l'interface.
static bool try_start_w5500() {
    printf("Checking W5500 Ethernet...\n");
    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
    wizchip_initialize();
    wizchip_check();
    setSHAR(ethernet_mac);
    printf("W5500 chip ready, version=%u\n", getVERSIONR());
    uint8_t phy_link = PHY_LINK_OFF;
    // Après un reset le PHY met un court instant à négocier le lien.
    // La version W5500 validée attendait implicitement ici ; on patiente au
    // plus trois secondes avant d'adopter le Wi-Fi comme repli.
    for (int attempt = 0; attempt < 30 && phy_link == PHY_LINK_OFF; ++attempt) {
        ctlwizchip(CW_GET_PHYLINK, &phy_link);
        if (phy_link == PHY_LINK_OFF) sleep_ms(100);
    }
    if (phy_link == PHY_LINK_OFF) {
        printf("W5500 cable not connected; using Wi-Fi.\n");
        return false;
    }
    if (socket(0, Sn_MR_MACRAW, 0, 0) < 0) {
        printf("W5500 MACRAW socket failed; using Wi-Fi.\n");
        return false;
    }
    netif_add(&ethernet_netif, IP4_ADDR_ANY, IP4_ADDR_ANY, IP4_ADDR_ANY,
              nullptr, netif_initialize, netif_input);
    ethernet_netif.name[0] = 'e';
    ethernet_netif.name[1] = '0';
    netif_set_link_callback(&ethernet_netif, netif_link_callback);
    original_wifi_output = ethernet_netif.output;
    ethernet_netif.output = wifi_output_trace;
    original_wifi_input = ethernet_netif.input;
    ethernet_netif.input = wifi_input_trace;
    netif_set_link_up(&ethernet_netif);
    netif_set_up(&ethernet_netif);
    err_t dhcp_result = dhcp_start(&ethernet_netif);
    printf("W5500 DHCP start: %d\n", (int)dhcp_result);
    active_network_netif = &ethernet_netif;
    ethernet_active = true;
    return true;
}

static void poll_w5500_frames() {
    if (!ethernet_active) return;
    uint16_t available = 0;
    getsockopt(0, SO_RECVBUF, &available);
    if (available == 0) return;
    int32_t frame_length = recv_lwip(0, ethernet_rx_frame, sizeof(ethernet_rx_frame));
    if (frame_length <= 0 || frame_length > (int32_t)sizeof(ethernet_rx_frame)) return;
    struct pbuf *frame = pbuf_alloc(PBUF_RAW, (u16_t)frame_length, PBUF_POOL);
    if (!frame) {
        printf("W5500 RX: lwIP pbuf allocation failed\n");
        return;
    }
    pbuf_take(frame, ethernet_rx_frame, (u16_t)frame_length);
    err_t result = ethernet_netif.input(frame, &ethernet_netif);
    if (result != ERR_OK) pbuf_free(frame);
}

static uint32_t credentials_checksum(const WifiCredentials &credentials) {
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&credentials);
    const size_t count = offsetof(WifiCredentials, checksum);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < count; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static bool credentials_are_valid(const WifiCredentials &credentials) {
    if (credentials.magic != WIFI_CONFIG_MAGIC ||
        credentials.version != WIFI_CONFIG_VERSION ||
        credentials.checksum != credentials_checksum(credentials)) {
        return false;
    }
    const size_t ssid_length = strnlen(credentials.ssid, sizeof(credentials.ssid));
    const size_t password_length = strnlen(credentials.password, sizeof(credentials.password));
    return ssid_length > 0 && ssid_length < sizeof(credentials.ssid) &&
           password_length >= 8 && password_length < sizeof(credentials.password);
}

static bool load_credentials(WifiCredentials *credentials) {
    const WifiCredentials *stored = reinterpret_cast<const WifiCredentials *>(
        XIP_BASE + WIFI_CONFIG_FLASH_OFFSET);
    memcpy(credentials, stored, sizeof(*credentials));
    return credentials_are_valid(*credentials);
}

struct FlashWriteRequest {
    uint8_t page[FLASH_PAGE_SIZE];
};

static void write_credentials_to_flash(void *argument) {
    const FlashWriteRequest *request = static_cast<const FlashWriteRequest *>(argument);
    flash_range_erase(WIFI_CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(WIFI_CONFIG_FLASH_OFFSET, request->page, FLASH_PAGE_SIZE);
}

static bool save_credentials(const char *ssid, const char *password) {
    WifiCredentials credentials = {};
    credentials.magic = WIFI_CONFIG_MAGIC;
    credentials.version = WIFI_CONFIG_VERSION;
    strncpy(credentials.ssid, ssid, sizeof(credentials.ssid) - 1);
    strncpy(credentials.password, password, sizeof(credentials.password) - 1);
    credentials.checksum = credentials_checksum(credentials);

    FlashWriteRequest request = {};
    memcpy(request.page, &credentials, sizeof(credentials));
    int result = flash_safe_execute(write_credentials_to_flash, &request, 5000);
    if (result != PICO_OK) {
        printf("Wi-Fi credentials could not be saved: %d\n", result);
        return false;
    }
    WifiCredentials checked = {};
    const bool saved = load_credentials(&checked);
    printf(saved ? "Wi-Fi credentials saved\n" : "Wi-Fi credential verification failed\n");
    return saved;
}

static int hex_value(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool url_decode(const char *input, size_t length, char *output, size_t output_size) {
    if (output_size == 0) return false;
    size_t written = 0;
    for (size_t i = 0; i < length; ++i) {
        char value = input[i];
        if (value == '+') {
            value = ' ';
        } else if (value == '%') {
            if (i + 2 >= length) return false;
            const int high = hex_value(input[++i]);
            const int low = hex_value(input[++i]);
            if (high < 0 || low < 0) return false;
            value = static_cast<char>((high << 4) | low);
        }
        if (value == '\0' || written + 1 >= output_size) return false;
        output[written++] = value;
    }
    output[written] = '\0';
    return true;
}

static bool get_query_value(const char *query, const char *key,
                            char *output, size_t output_size) {
    const size_t key_length = strlen(key);
    const char *cursor = query;
    while (*cursor && *cursor != ' ') {
        const char *field_end = strchr(cursor, '&');
        if (!field_end) field_end = strchr(cursor, ' ');
        if (!field_end) field_end = cursor + strlen(cursor);
        if (static_cast<size_t>(field_end - cursor) > key_length &&
            !memcmp(cursor, key, key_length) && cursor[key_length] == '=') {
            return url_decode(cursor + key_length + 1,
                              static_cast<size_t>(field_end - cursor - key_length - 1),
                              output, output_size);
        }
        if (*field_end != '&') break;
        cursor = field_end + 1;
    }
    return false;
}

static void setup_send_response(struct tcp_pcb *pcb, const char *body) {
    char response[2200];
    const int length = snprintf(response, sizeof(response),
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\nCache-Control: no-store\r\n\r\n%s", body);
    if (length > 0 && static_cast<size_t>(length) < sizeof(response)) {
        tcp_write(pcb, response, static_cast<u16_t>(length), TCP_WRITE_FLAG_COPY);
        tcp_output(pcb);
    }
}

static int wifi_scan_result(void *, const cyw43_ev_scan_result_t *result) {
    if (!result || result->ssid_len == 0 || wifi_scan_network_count >= WIFI_SCAN_MAX_NETWORKS) return 0;
    const size_t length = result->ssid_len < 32 ? result->ssid_len : 32;
    for (size_t i = 0; i < wifi_scan_network_count; ++i) {
        if (!strncmp(wifi_scan_networks[i].ssid, reinterpret_cast<const char *>(result->ssid), length) &&
            wifi_scan_networks[i].ssid[length] == '\0') return 0;
    }
    WifiScanNetwork &network = wifi_scan_networks[wifi_scan_network_count++];
    memcpy(network.ssid, result->ssid, length);
    network.ssid[length] = '\0';
    network.rssi = result->rssi;
    printf("Wi-Fi scan: %s (%d dBm)\n", network.ssid, network.rssi);
    return 0;
}

static void start_wifi_scan() {
    if (cyw43_wifi_scan_active(&cyw43_state)) return;
    wifi_scan_network_count = 0;
    cyw43_wifi_scan_options_t options = {};
    const int result = cyw43_wifi_scan(&cyw43_state, &options, nullptr, wifi_scan_result);
    printf(result == 0 ? "Wi-Fi scan started\n" : "Wi-Fi scan failed: %d\n", result);
}

static void build_setup_page(char *body, size_t body_size) {
    int used = snprintf(body, body_size,
        "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width\">"
        "<title>NetworkBox Wi-Fi</title></head><body><h2>NetworkBox - Wi-Fi</h2>"
        "<p>Choisissez votre reseau, ou saisissez un SSID masque.</p>"
        "<p><a href=\"/scan\">Rechercher les reseaux Wi-Fi</a></p>"
        "<form action=\"/save\" method=\"get\"><label>Reseau Wi-Fi detecte<br>"
        "<select name=\"ssid\"><option value=\"\">-- Choisir un reseau --</option>");
    for (size_t i = 0; i < wifi_scan_network_count && used > 0 && static_cast<size_t>(used) < body_size; ++i) {
        used += snprintf(body + used, body_size - static_cast<size_t>(used),
                         "<option value=\"%s\">%s (%d dBm)</option>",
                         wifi_scan_networks[i].ssid, wifi_scan_networks[i].ssid,
                         wifi_scan_networks[i].rssi);
    }
    if (used > 0 && static_cast<size_t>(used) < body_size) {
        snprintf(body + used, body_size - static_cast<size_t>(used),
                 "</select></label><br><br><label>Ou SSID manuel / reseau cache<br>"
                 "<input name=\"ssid_manual\" maxlength=\"32\"></label><br><br><label>Mot de passe<br>"
                 "<input name=\"password\" type=\"password\" maxlength=\"63\" required></label><br><br>"
                 "<button type=\"submit\">Enregistrer et redemarrer</button></form></body></html>");
    }
}

static const char SETUP_SCANNING_PAGE[] =
    "<!doctype html><html><head><meta http-equiv=\"refresh\" content=\"6;url=/\">"
    "<meta name=\"viewport\" content=\"width=device-width\"><title>NetworkBox</title></head>"
    "<body><h2>Recherche Wi-Fi...</h2><p>Retour a la liste dans quelques secondes.</p></body></html>";

static const char SETUP_SAVED_PAGE[] =
    "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width\">"
    "<title>NetworkBox</title></head><body><h2>Configuration enregistree</h2>"
    "<p>La NetworkBox redemarre et rejoint le Wi-Fi dans quelques secondes.</p></body></html>";

static const char SETUP_ERROR_PAGE[] =
    "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width\">"
    "<title>NetworkBox</title></head><body><h2>Erreur</h2>"
    "<p>SSID requis ; mot de passe WPA2 entre 8 et 63 caracteres.</p>"
    "<p><a href=\"/\">Retour</a></p></body></html>";

static err_t setup_http_recv(void *, struct tcp_pcb *pcb, struct pbuf *packet, err_t error) {
    if (!packet) {
        tcp_close(pcb);
        return ERR_OK;
    }
    if (error != ERR_OK) {
        pbuf_free(packet);
        return error;
    }

    char request[600] = {};
    const u16_t copied = packet->tot_len < sizeof(request) - 1 ?
        packet->tot_len : sizeof(request) - 1;
    pbuf_copy_partial(packet, request, copied, 0);
    tcp_recved(pcb, packet->tot_len);
    pbuf_free(packet);

    if (!strncmp(request, "GET /scan", 9)) {
        start_wifi_scan();
        setup_send_response(pcb, SETUP_SCANNING_PAGE);
    } else if (!strncmp(request, "GET /save?", 10)) {
        char ssid[33] = {};
        char manual_ssid[33] = {};
        char password[65] = {};
        get_query_value(request + 10, "ssid", ssid, sizeof(ssid));
        get_query_value(request + 10, "ssid_manual", manual_ssid, sizeof(manual_ssid));
        const bool have_password = get_query_value(request + 10, "password", password, sizeof(password));
        if (manual_ssid[0]) strncpy(ssid, manual_ssid, sizeof(ssid) - 1);
        const size_t password_length = strnlen(password, sizeof(password));
        if (have_password && ssid[0] &&
            password_length >= 8 && password_length < sizeof(password) &&
            save_credentials(ssid, password)) {
            setup_send_response(pcb, SETUP_SAVED_PAGE);
            reboot_at_ms = to_ms_since_boot(get_absolute_time()) + 1500;
        } else {
            setup_send_response(pcb, SETUP_ERROR_PAGE);
        }
    } else {
        char page[1800] = {};
        build_setup_page(page, sizeof(page));
        setup_send_response(pcb, page);
    }
    tcp_close(pcb);
    return ERR_OK;
}

static err_t setup_http_accept(void *, struct tcp_pcb *pcb, err_t error) {
    if (error != ERR_OK) return error;
    tcp_recv(pcb, setup_http_recv);
    return ERR_OK;
}

static bool start_setup_portal() {
    setup_mode = true;
    // The CYW43 scan runs through its station interface. It can be enabled
    // alongside the configuration access point without joining a network.
    cyw43_arch_enable_sta_mode();
    cyw43_arch_enable_ap_mode(SETUP_AP_SSID, SETUP_AP_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);

    // The Pico SDK does not bundle a DHCP server by default. This small MIT
    // licensed server comes from Raspberry Pi's official access-point example.
    // Without it, phones can join the radio but receive no IPv4 address.
    ip4_addr_t ap_gateway, ap_mask;
    IP4_ADDR(&ap_gateway, 192, 168, 4, 1);
    IP4_ADDR(&ap_mask, 255, 255, 255, 0);
    cyw43_arch_lwip_begin();
    dhcp_server_init(&setup_dhcp_server,
                     &cyw43_state.netif[CYW43_ITF_AP],
                     reinterpret_cast<ip_addr_t *>(&ap_gateway),
                     reinterpret_cast<ip_addr_t *>(&ap_mask));
    setup_server = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!setup_server || tcp_bind(setup_server, IP_ANY_TYPE, 80) != ERR_OK) {
        cyw43_arch_lwip_end();
        printf("Setup web server failed to start\n");
        return false;
    }
    setup_server = tcp_listen_with_backlog(setup_server, 2);
    if (!setup_server) {
        cyw43_arch_lwip_end();
        printf("Setup web server could not listen\n");
        return false;
    }
    tcp_accept(setup_server, setup_http_accept);
    cyw43_arch_lwip_end();
    start_wifi_scan();
    printf("NetworkBox setup mode\n");
    printf("Wi-Fi: %s  password: %s\n", SETUP_AP_SSID, SETUP_AP_PASSWORD);
    printf("Open http://192.168.4.1\n");
    return true;
}

static void check_wifi_setup_button() {
    // En RJ45, le bouton est lu seulement au démarrage. En Wi-Fi, un appui
    // maintenu ouvre le portail pour corriger ou changer les identifiants.
    if (ethernet_active || !station_connected || setup_mode) return;

    const bool pressed = !gpio_get(MODE_BUTTON_PIN);
    if (!button_long_press_armed) {
        if (!pressed) button_long_press_armed = true;
        return;
    }
    if (!pressed) {
        button_press_started_ms = 0;
        return;
    }
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    if (button_press_started_ms == 0) {
        button_press_started_ms = now;
        return;
    }
    if (now - button_press_started_ms < 3000) return;

    printf("Mode button held 3 seconds: opening Wi-Fi setup.\n");
    button_press_started_ms = 0;
    station_connected = false;
    active_network_netif = nullptr;
    cyw43_arch_disable_sta_mode();
    if (!start_setup_portal()) {
        printf("Could not start Wi-Fi setup portal. Restart Pico.\n");
    }
}

static void uart_rx_irq_handler() {
    while (uart_is_readable(UART_ID)) {
        uint8_t byte = (uint8_t)uart_getc(UART_ID);
        uint32_t head = uart_rx_head;
        uint32_t next = (head + 1) & UART_RX_RING_MASK;
        if (next == uart_rx_tail) {
            ++uart_rx_overflow;
            continue;
        }
        uart_rx_ring[head] = byte;
        __dmb();
        uart_rx_head = next;
    }
}

static bool tcp_flow_expired(NatTcpFlow &flow, uint32_t now) {
    if (!flow.active) return true;
    if (flow.closed_at && now - flow.closed_at >= 2000) return true;
    if ((flow.client_fin_seen || flow.server_fin_seen) &&
        now - flow.last_activity >= 15000) return true;
    return now - flow.last_activity >= 120000;
}

static void update_tcp_flow(NatTcpFlow *flow, uint8_t flags,
                            bool from_client, uint32_t now) {
    if (!flow) return;
    flow->last_activity = now;
    if (flags & 0x04) { // RST
        flow->reset_seen = true;
        flow->closed_at = now;
    }
    if (flags & 0x01) { // FIN
        if (from_client) flow->client_fin_seen = true;
        else flow->server_fin_seen = true;
        if (flow->client_fin_seen && flow->server_fin_seen && !flow->closed_at)
            flow->closed_at = now;
    }
}

static void recalc_icmp_checksums(uint8_t *packet, size_t length) {
    if (length < 28 || (packet[0] >> 4) != 4) return;
    size_t ihl = (packet[0] & 0x0f) * 4;
    if (ihl < 20 || length < ihl + 8) return;
    packet[10] = packet[11] = 0;
    uint16_t ip_sum = checksum16(packet, ihl);
    packet[10] = (uint8_t)(ip_sum >> 8);
    packet[11] = (uint8_t)ip_sum;
    packet[ihl + 2] = packet[ihl + 3] = 0;
    uint16_t icmp_sum = checksum16(packet + ihl, length - ihl);
    packet[ihl + 2] = (uint8_t)(icmp_sum >> 8);
    packet[ihl + 3] = (uint8_t)icmp_sum;
}

static void recalc_udp_checksum(uint8_t *packet, size_t ip_length) {
    size_t ihl = (packet[0] & 0x0f) * 4;
    if (ihl < 20 || ip_length < ihl + 8 || packet[9] != 17) return;
    size_t udp_length = ((size_t)packet[ihl + 4] << 8) | packet[ihl + 5];
    if (udp_length < 8 || udp_length > ip_length - ihl) return;

    packet[ihl + 6] = packet[ihl + 7] = 0;
    uint32_t sum = 0;
    auto add_words = [&sum](const uint8_t *data, size_t length) {
        while (length > 1) {
            sum += ((uint16_t)data[0] << 8) | data[1];
            data += 2;
            length -= 2;
        }
        if (length) sum += (uint16_t)data[0] << 8;
    };
    add_words(packet + 12, 8); // IPv4 source + destination
    sum += 17;                 // zero byte + protocol number
    sum += (uint16_t)udp_length;
    add_words(packet + ihl, udp_length);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    uint16_t result = (uint16_t)~sum;
    if (result == 0) result = 0xffff;
    packet[ihl + 6] = (uint8_t)(result >> 8);
    packet[ihl + 7] = (uint8_t)result;
}

static void recalc_tcp_checksum(uint8_t *packet, size_t ip_length) {
    size_t ihl = (packet[0] & 0x0f) * 4;
    if (ihl < 20 || ip_length < ihl + 20 || packet[9] != 6) return;
    size_t tcp_length = ip_length - ihl;
    packet[ihl + 16] = packet[ihl + 17] = 0;
    uint32_t sum = 0;
    auto add_words = [&sum](const uint8_t *data, size_t length) {
        while (length > 1) {
            sum += ((uint16_t)data[0] << 8) | data[1];
            data += 2;
            length -= 2;
        }
        if (length) sum += (uint16_t)data[0] << 8;
    };
    add_words(packet + 12, 8); // IPv4 source + destination
    sum += 6;                  // zero byte + protocol number
    sum += (uint16_t)tcp_length;
    add_words(packet + ihl, tcp_length);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    uint16_t result = (uint16_t)~sum;
    packet[ihl + 16] = (uint8_t)(result >> 8);
    packet[ihl + 17] = (uint8_t)result;
}

static NatTcpFlow *find_tcp_flow(const uint8_t *client_ip, const uint8_t *server_ip,
                                 uint16_t client_port, uint16_t server_port) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    for (auto &flow : nat_tcp_flows) {
        if (tcp_flow_expired(flow, now)) flow.active = false;
        if (flow.active && flow.client_port == client_port &&
            flow.server_port == server_port &&
            memcmp(flow.client_ip, client_ip, 4) == 0 &&
            memcmp(flow.server_ip, server_ip, 4) == 0) return &flow;
    }
    return nullptr;
}

static NatTcpFlow *find_tcp_reverse_flow(const uint8_t *server_ip,
                                         uint16_t client_port,
                                         uint16_t server_port) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    for (auto &flow : nat_tcp_flows) {
        if (tcp_flow_expired(flow, now)) flow.active = false;
        if (flow.active && flow.client_port == client_port &&
            flow.server_port == server_port &&
            memcmp(flow.server_ip, server_ip, 4) == 0) return &flow;
    }
    return nullptr;
}

// Reuse a closed TCP flow immediately for a new SYN. Still-active flows are
// never evicted, so an ongoing transfer remains protected.
static NatTcpFlow *allocate_tcp_flow(uint32_t now) {
    NatTcpFlow *oldest_closed = nullptr;
    for (auto &candidate : nat_tcp_flows) {
        if (tcp_flow_expired(candidate, now)) {
            candidate.active = false;
            return &candidate;
        }
        if (candidate.closed_at &&
            (!oldest_closed || candidate.closed_at < oldest_closed->closed_at)) {
            oldest_closed = &candidate;
        }
    }
    return oldest_closed;
}

static err_t wifi_output_trace(struct netif *netif, struct pbuf *p,
                               const ip4_addr_t *ipaddr) {
    mark_network_activity();
    const char *transport = ethernet_active ? "W5500" : "Wi-Fi";
    if (ipaddr) {
        printf("%s TX to %u.%u.%u.%u (%u bytes)\n", transport,
               ip4_addr1(ipaddr), ip4_addr2(ipaddr),
               ip4_addr3(ipaddr), ip4_addr4(ipaddr),
               (unsigned)p->tot_len);
    }
    err_t result = original_wifi_output(netif, p, ipaddr);
    printf("%s output result: %d (up=%d link=%d)\n", transport,
           (int)result, netif_is_up(netif) ? 1 : 0,
           netif_is_link_up(netif) ? 1 : 0);
    return result;
}

static err_t wifi_input_trace(struct pbuf *p, struct netif *netif) {
    mark_network_activity();
    const char *transport = ethernet_active ? "W5500" : "Wi-Fi";
    printf("%s input pbuf: %u bytes\n", transport, p ? (unsigned)p->tot_len : 0u);
    constexpr size_t ETH_HEADER_LEN = 14;
    if (p && p->tot_len >= ETH_HEADER_LEN + 20) {
        uint8_t header[60] = {};
        size_t available_ip_bytes = p->tot_len - ETH_HEADER_LEN;
        size_t header_bytes = available_ip_bytes < sizeof(header) ?
                              available_ip_bytes : sizeof(header);
        pbuf_copy_partial(p, header, header_bytes, ETH_HEADER_LEN);
        if ((header[0] >> 4) == 4) {
            size_t ihl = (header[0] & 0x0f) * 4;
            printf("%s RX IPv4 %u.%u.%u.%u -> %u.%u.%u.%u (%u bytes)\n", transport,
                   header[12], header[13], header[14], header[15],
                   header[16], header[17], header[18], header[19],
                   (unsigned)p->tot_len);

            // Return TCP segments to an Atari flow registered by an outbound SYN.
            if (header[9] == 6 && ihl >= 20 &&
                p->tot_len >= ETH_HEADER_LEN + ihl + 20 &&
                header[16] == ip4_addr1(&netif->ip_addr) &&
                header[17] == ip4_addr2(&netif->ip_addr) &&
                header[18] == ip4_addr3(&netif->ip_addr) &&
                header[19] == ip4_addr4(&netif->ip_addr)) {
                uint8_t tcp_head[20];
                pbuf_copy_partial(p, tcp_head, sizeof(tcp_head), ETH_HEADER_LEN + ihl);
                uint16_t source_port = ((uint16_t)tcp_head[0] << 8) | tcp_head[1];
                uint16_t destination_port = ((uint16_t)tcp_head[2] << 8) | tcp_head[3];
                NatTcpFlow *flow = find_tcp_reverse_flow(header + 12,
                                                         destination_port, source_port);
                if (flow) {
                    uint8_t reply[SLIP_MAX_FRAME];
                    size_t ip_length = ((size_t)header[2] << 8) | header[3];
                    if (ip_length <= sizeof(reply) &&
                        ip_length <= p->tot_len - ETH_HEADER_LEN) {
                        pbuf_copy_partial(p, reply, ip_length, ETH_HEADER_LEN);
                        memcpy(reply + 16, flow->client_ip, 4);
                        reply[10] = reply[11] = 0;
                        uint16_t ip_sum = checksum16(reply, ihl);
                        reply[10] = (uint8_t)(ip_sum >> 8);
                        reply[11] = (uint8_t)ip_sum;
                        recalc_tcp_checksum(reply, ip_length);
                        slip_send_frame(reply, ip_length);
                        update_tcp_flow(flow, tcp_head[13], false,
                                        to_ms_since_boot(get_absolute_time()));
                        printf("NAT TCP reply -> Atari (%u bytes, %u -> %u)\n",
                               (unsigned)ip_length, source_port, destination_port);
                        pbuf_free(p);
                        return ERR_OK;
                    }
                }
            }

            // Minimal ICMP NAT: restore a reply to the Atari SLIP client.
            if (header[9] == 1 && ihl >= 20 && ihl + 8 <= sizeof(header)) {
                printf("ICMP RX type=%u id=%04X seq=%04X nat=%d\n",
                       header[ihl],
                       ((unsigned)header[ihl + 4] << 8) | header[ihl + 5],
                       ((unsigned)header[ihl + 6] << 8) | header[ihl + 7],
                       nat_icmp_active ? 1 : 0);
            }

            // Minimal DNS UDP NAT: restore the response destination to Atari.
            if (nat_dns_active && header[9] == 17 && ihl >= 20 &&
                p->tot_len >= ETH_HEADER_LEN + ihl + 20 &&
                header[16] == ip4_addr1(&netif->ip_addr) &&
                header[17] == ip4_addr2(&netif->ip_addr) &&
                header[18] == ip4_addr3(&netif->ip_addr) &&
                header[19] == ip4_addr4(&netif->ip_addr)) {
                uint8_t udp_head[12];
                pbuf_copy_partial(p, udp_head, sizeof(udp_head), ETH_HEADER_LEN + ihl);
                uint16_t source_port = ((uint16_t)udp_head[0] << 8) | udp_head[1];
                uint16_t destination_port = ((uint16_t)udp_head[2] << 8) | udp_head[3];
                uint16_t transaction_id = ((uint16_t)udp_head[8] << 8) | udp_head[9];
                if (source_port == 53 && destination_port == nat_dns_client_port &&
                    transaction_id == nat_dns_transaction_id &&
                    memcmp(header + 12, nat_dns_server_ip, 4) == 0) {
                    uint8_t reply[SLIP_MAX_FRAME];
                    size_t ip_length = ((size_t)header[2] << 8) | header[3];
                    if (ip_length <= sizeof(reply) &&
                        ip_length <= p->tot_len - ETH_HEADER_LEN) {
                        pbuf_copy_partial(p, reply, ip_length, ETH_HEADER_LEN);
                        memcpy(reply + 16, nat_dns_client_ip, 4);
                        reply[10] = reply[11] = 0;
                        uint16_t ip_sum = checksum16(reply, ihl);
                        reply[10] = (uint8_t)(ip_sum >> 8);
                        reply[11] = (uint8_t)ip_sum;
                        recalc_udp_checksum(reply, ip_length);
                        slip_send_frame(reply, ip_length);
                        printf("NAT DNS reply -> Atari (%u bytes, ID=%04X)\n",
                               (unsigned)ip_length, transaction_id);
                        nat_dns_active = false;
                        pbuf_free(p);
                        return ERR_OK;
                    }
                }
            }
            if (nat_icmp_active && p->tot_len >= ihl + 8 &&
                header[9] == 1 && header[ihl] == 0 &&
                header[16] == ip4_addr1(&netif->ip_addr) &&
                header[17] == ip4_addr2(&netif->ip_addr) &&
                header[18] == ip4_addr3(&netif->ip_addr) &&
                header[19] == ip4_addr4(&netif->ip_addr)) {
                uint16_t id = ((uint16_t)header[ihl + 4] << 8) | header[ihl + 5];
                uint16_t seq = ((uint16_t)header[ihl + 6] << 8) | header[ihl + 7];
                // PING.PRG émet une rafale : plusieurs réponses peuvent revenir
                // après que la requête suivante a déjà mis à jour la séquence.
                // L'identifiant ICMP et l'adresse Atari suffisent ici, puisqu'il
                // n'y a qu'un seul client SLIP.
                if (id == nat_icmp_id) {
                    uint8_t reply[SLIP_MAX_FRAME];
                    size_t ip_length = p->tot_len - ETH_HEADER_LEN;
                    if (ip_length <= sizeof(reply)) {
                        pbuf_copy_partial(p, reply, ip_length, ETH_HEADER_LEN);
                        memcpy(reply + 16, nat_icmp_client_ip, 4);
                        recalc_icmp_checksums(reply, ip_length);
                        slip_send_frame(reply, ip_length);
                        printf("NAT ICMP reply -> Atari (%u bytes)\n", (unsigned)ip_length);
                        pbuf_free(p);
                        return ERR_OK;
                    }
                }
            }
        }
    }
    return original_wifi_input(p, netif);
}

static uint16_t checksum16(const uint8_t *data, size_t length) {
    uint32_t sum = 0;
    while (length > 1) {
        sum += ((uint16_t)data[0] << 8) | data[1];
        data += 2;
        length -= 2;
    }
    if (length) sum += (uint16_t)data[0] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static void slip_send_frame(const uint8_t *data, size_t length) {
    // La liaison RS-232 est full-duplex : inutile d'ajouter une pause fixe.
    uart_putc(UART_ID, SLIP_END);
    for (size_t i = 0; i < length; ++i) {
        if (data[i] == SLIP_END) {
            uart_putc(UART_ID, SLIP_ESC); uart_putc(UART_ID, SLIP_ESC_END);
        } else if (data[i] == SLIP_ESC) {
            uart_putc(UART_ID, SLIP_ESC); uart_putc(UART_ID, SLIP_ESC_ESC);
        } else {
            uart_putc(UART_ID, data[i]);
        }
    }
    uart_putc(UART_ID, SLIP_END);
}

static err_t slip_netif_output(struct netif *netif, struct pbuf *p,
                               const ip4_addr_t *ipaddr) {
    (void)netif;
    (void)ipaddr;
    if (p->tot_len > SLIP_MAX_FRAME) return ERR_MEM;
    pbuf_copy_partial(p, slip_frame, p->tot_len, 0);
    printf("SLIP TX to Atari (%u bytes)\n", (unsigned)p->tot_len);
    slip_send_frame(slip_frame, p->tot_len);
    return ERR_OK;
}

static bool nat_send_icmp_request(struct pbuf *input, struct netif *wifi_netif) {
    if (!input || input->tot_len < 28) return false;
    uint8_t packet[SLIP_MAX_FRAME];
    if (input->tot_len > sizeof(packet)) return false;
    pbuf_copy_partial(input, packet, input->tot_len, 0);
    size_t ihl = (packet[0] & 0x0f) * 4;
    if ((packet[0] >> 4) != 4 || ihl < 20 || input->tot_len < ihl + 8 ||
        packet[9] != 1 || packet[ihl] != 8) return false;
    memcpy(nat_icmp_client_ip, packet + 12, 4);
    nat_icmp_id = ((uint16_t)packet[ihl + 4] << 8) | packet[ihl + 5];
    nat_icmp_sequence = ((uint16_t)packet[ihl + 6] << 8) | packet[ihl + 7];
    memcpy(packet + 12, &wifi_netif->ip_addr.addr, 4);
    recalc_icmp_checksums(packet, input->tot_len);
    // Le driver CYW43 exige un buffer RAM contigu pour l'émission brute.
    // PBUF_LINK réserve l'espace complet attendu par le driver Ethernet.
    struct pbuf *out = pbuf_alloc(PBUF_LINK, input->tot_len, PBUF_RAM);
    if (!out) return false;
    pbuf_take(out, packet, input->tot_len);
    ip4_addr_t destination;
    memcpy(&destination.addr, packet + 16, 4);
    cyw43_arch_lwip_begin();
    err_t arp_result = etharp_request(wifi_netif, &destination);
    printf("ARP request result: %d\n", (int)arp_result);
    cyw43_arch_lwip_end();
    // Laisser le temps au driver Wi-Fi de recevoir et mémoriser la réponse ARP.
    sleep_ms(250);
    cyw43_arch_lwip_begin();
    err_t result = wifi_netif->output(wifi_netif, out, &destination);
    cyw43_arch_lwip_end();
    pbuf_free(out);
    if (result == ERR_OK) {
        nat_icmp_active = true;
        printf("NAT ICMP request sent as %u.%u.%u.%u\n",
               packet[12], packet[13], packet[14], packet[15]);
    } else {
        printf("NAT ICMP send error: %d\n", (int)result);
    }
    return true;
}

static bool nat_send_dns_request(struct pbuf *input, struct netif *wifi_netif) {
    if (!input || input->tot_len < 20) return false;
    uint8_t packet[SLIP_MAX_FRAME];
    if (input->tot_len > sizeof(packet)) return false;
    pbuf_copy_partial(input, packet, input->tot_len, 0);
    size_t ihl = (packet[0] & 0x0f) * 4;
    size_t ip_length = ((size_t)packet[2] << 8) | packet[3];
    if ((packet[0] >> 4) != 4 || ihl < 20 || ip_length > input->tot_len ||
        ip_length < ihl + 20 || packet[9] != 17) return false;
    size_t udp_length = ((size_t)packet[ihl + 4] << 8) | packet[ihl + 5];
    uint16_t destination_port = ((uint16_t)packet[ihl + 2] << 8) | packet[ihl + 3];
    if (destination_port != 53 || udp_length < 20 || udp_length > ip_length - ihl)
        return false;

    nat_dns_client_port = ((uint16_t)packet[ihl] << 8) | packet[ihl + 1];
    nat_dns_transaction_id = ((uint16_t)packet[ihl + 8] << 8) | packet[ihl + 9];
    memcpy(nat_dns_client_ip, packet + 12, 4);
    memcpy(nat_dns_server_ip, packet + 16, 4);
    memcpy(packet + 12, &wifi_netif->ip_addr.addr, 4);
    packet[10] = packet[11] = 0;
    uint16_t ip_sum = checksum16(packet, ihl);
    packet[10] = (uint8_t)(ip_sum >> 8);
    packet[11] = (uint8_t)ip_sum;
    recalc_udp_checksum(packet, ip_length);

    struct pbuf *out = pbuf_alloc(PBUF_LINK, ip_length, PBUF_RAM);
    if (!out) {
        printf("NAT DNS: no output buffer\n");
        return true;
    }
    pbuf_take(out, packet, ip_length);
    ip4_addr_t destination;
    memcpy(&destination.addr, packet + 16, 4);
    ip4_addr_t next_hop = destination;
    if (!ip4_addr_netcmp(&destination, &wifi_netif->ip_addr, &wifi_netif->netmask))
        next_hop = wifi_netif->gw;

    cyw43_arch_lwip_begin();
    err_t arp_result = etharp_request(wifi_netif, &next_hop);
    cyw43_arch_lwip_end();
    printf("NAT DNS query ID=%04X -> %u.%u.%u.%u (ARP %d)\n",
           nat_dns_transaction_id, packet[16], packet[17], packet[18], packet[19],
           (int)arp_result);
    sleep_ms(250);
    cyw43_arch_lwip_begin();
    err_t result = wifi_netif->output(wifi_netif, out, &destination);
    cyw43_arch_lwip_end();
    pbuf_free(out);
    if (result == ERR_OK) {
        nat_dns_active = true;
        printf("NAT DNS query sent as %u.%u.%u.%u\n",
               packet[12], packet[13], packet[14], packet[15]);
    } else {
        nat_dns_active = false;
        printf("NAT DNS send error: %d\n", (int)result);
    }
    return true;
}

static bool nat_send_tcp_packet(struct pbuf *input, struct netif *wifi_netif) {
    if (!input || input->tot_len < 40 || input->tot_len > SLIP_MAX_FRAME) return false;
    uint8_t packet[SLIP_MAX_FRAME];
    pbuf_copy_partial(input, packet, input->tot_len, 0);
    size_t ihl = (packet[0] & 0x0f) * 4;
    size_t ip_length = ((size_t)packet[2] << 8) | packet[3];
    if ((packet[0] >> 4) != 4 || ihl < 20 || packet[9] != 6 ||
        ip_length < ihl + 20 || ip_length > input->tot_len) return false;
    size_t tcp_header_length = (packet[ihl + 12] >> 4) * 4;
    if (tcp_header_length < 20 || ip_length < ihl + tcp_header_length) return false;

    uint16_t client_port = ((uint16_t)packet[ihl] << 8) | packet[ihl + 1];
    uint16_t server_port = ((uint16_t)packet[ihl + 2] << 8) | packet[ihl + 3];
    uint8_t flags = packet[ihl + 13];
    NatTcpFlow *flow = find_tcp_flow(packet + 12, packet + 16,
                                     client_port, server_port);
    bool new_syn = !flow && (flags & 0x02); // First SYN for this TCP connection.
    if (new_syn) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        flow = allocate_tcp_flow(now);
        if (flow) {
            *flow = {};
            flow->active = true;
            memcpy(flow->client_ip, packet + 12, 4);
            memcpy(flow->server_ip, packet + 16, 4);
            flow->client_port = client_port;
            flow->server_port = server_port;
        }
    }
    if (!flow) {
        if (new_syn) {
            printf("NAT TCP flow table full: new connection %u -> %u dropped\n",
                   client_port, server_port);
        } else if (flags & 0x05) { // FIN or RST after its flow was released.
            printf("NAT TCP connection already closed: %u -> %u ignored\n",
                   client_port, server_port);
        } else {
            printf("NAT TCP no matching connection: %u -> %u ignored\n",
                   client_port, server_port);
        }
        return true; // Deliberately drop unsupported/untracked TCP rather than raw-forward.
    }

    update_tcp_flow(flow, flags, true, to_ms_since_boot(get_absolute_time()));
    memcpy(packet + 12, &wifi_netif->ip_addr.addr, 4);
    packet[10] = packet[11] = 0;
    uint16_t ip_sum = checksum16(packet, ihl);
    packet[10] = (uint8_t)(ip_sum >> 8);
    packet[11] = (uint8_t)ip_sum;
    recalc_tcp_checksum(packet, ip_length);

    struct pbuf *out = pbuf_alloc(PBUF_LINK, ip_length, PBUF_RAM);
    if (!out) {
        printf("NAT TCP: no output buffer\n");
        return true;
    }
    pbuf_take(out, packet, ip_length);
    ip4_addr_t destination;
    memcpy(&destination.addr, packet + 16, 4);
    cyw43_arch_lwip_begin();
    err_t result = wifi_netif->output(wifi_netif, out, &destination);
    cyw43_arch_lwip_end();
    pbuf_free(out);
    printf("NAT TCP %u -> %u flags=%02X output=%d\n",
           client_port, server_port, flags, (int)result);
    return true;
}

static err_t slip_netif_init(struct netif *netif) {
    netif->name[0] = 's';
    netif->name[1] = 'l';
    netif->output = slip_netif_output;
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

static void slip_deliver_frame() {
    if (!slip_in_frame || slip_length == 0) return;
    mark_network_activity();
    if (!station_connected && !ethernet_active) {
        printf("SLIP ignored while NetworkBox setup is active\n");
        return;
    }
    struct pbuf *p = pbuf_alloc(PBUF_RAW, slip_length, PBUF_POOL);
    if (!p) {
        printf("SLIP lwIP allocation failed\n");
        return;
    }
    pbuf_take(p, slip_frame, slip_length);
    struct netif *wifi_netif = active_network_netif;
    if (!wifi_netif) {
        printf("No active network interface; SLIP frame dropped\n");
        pbuf_free(p);
        return;
    }
    if (nat_send_dns_request(p, wifi_netif)) {
        pbuf_free(p);
        return;
    }
    if (nat_send_tcp_packet(p, wifi_netif)) {
        pbuf_free(p);
        return;
    }
    if (nat_send_icmp_request(p, wifi_netif)) {
        pbuf_free(p);
        return;
    }
    cyw43_arch_lwip_begin();
    err_t result = slip_netif.input(p, &slip_netif);
    cyw43_arch_lwip_end();
    printf("lwIP input result: %d\n", (int)result);
    if (result != ERR_OK) pbuf_free(p);
}

static void process_ip_packet(uint8_t *packet, size_t length) {
    if (length < 28 || (packet[0] >> 4) != 4) return;
    const size_t ip_header_len = (packet[0] & 0x0f) * 4;
    if (ip_header_len < 20 || length < ip_header_len + 8) return;
    const size_t ip_total_len = ((size_t)packet[2] << 8) | packet[3];
    if (ip_total_len < ip_header_len + 8 || ip_total_len > length) {
        printf("IP length mismatch: header=%u frame=%u\n",
               (unsigned)ip_total_len, (unsigned)length);
        return;
    }
    length = ip_total_len;
    if (packet[9] != 1 || packet[ip_header_len] != 8) return; // ICMP echo request

    printf("IPv4 %u.%u.%u.%u -> %u.%u.%u.%u\n",
           packet[12], packet[13], packet[14], packet[15],
           packet[16], packet[17], packet[18], packet[19]);
    uint16_t received_ip_sum = ((uint16_t)packet[10] << 8) | packet[11];
    uint16_t received_icmp_sum = ((uint16_t)packet[ip_header_len + 2] << 8) |
                                 packet[ip_header_len + 3];
    printf("Checksums request: IP=%04X ICMP=%04X\n",
           received_ip_sum, received_icmp_sum);

    // Swap IPv4 source and destination addresses.
    for (int i = 0; i < 4; ++i) {
        uint8_t temp = packet[12 + i];
        packet[12 + i] = packet[16 + i];
        packet[16 + i] = temp;
    }
    packet[ip_header_len] = 0; // ICMP echo reply
    packet[10] = packet[11] = 0;
    uint16_t ip_sum = checksum16(packet, ip_header_len);
    packet[10] = (uint8_t)(ip_sum >> 8);
    packet[11] = (uint8_t)ip_sum;
    packet[ip_header_len + 2] = packet[ip_header_len + 3] = 0;
    uint16_t icmp_sum = checksum16(packet + ip_header_len, length - ip_header_len);
    packet[ip_header_len + 2] = (uint8_t)(icmp_sum >> 8);
    packet[ip_header_len + 3] = (uint8_t)icmp_sum;
    printf("Checksums reply: IP=%04X ICMP=%04X\n", ip_sum, icmp_sum);
    slip_send_frame(packet, length);
    printf("ICMP echo reply sent (%u bytes)\n", (unsigned)length);
}

static void slip_receive_byte(uint8_t byte) {
    if (byte == SLIP_END) {
        if (slip_in_frame && slip_length > 0) {
            printf("SLIP frame received: %u bytes\n", (unsigned)slip_length);
            slip_deliver_frame();
        }
        slip_length = 0;
        slip_in_frame = true;
        slip_escaped = false;
        return;
    }

    if (!slip_in_frame) return;

    if (slip_escaped) {
        if (byte == SLIP_ESC_END) byte = SLIP_END;
        else if (byte == SLIP_ESC_ESC) byte = SLIP_ESC;
        else {
            slip_length = 0;
            slip_escaped = false;
            return;
        }
        slip_escaped = false;
    } else if (byte == SLIP_ESC) {
        slip_escaped = true;
        return;
    }

    if (slip_length < SLIP_MAX_FRAME) {
        slip_frame[slip_length++] = byte;
    } else {
        slip_length = 0;
        slip_in_frame = false;
        slip_escaped = false;
        printf("SLIP frame too large\n");
    }
}



int main()
{
    stdio_init_all();
    init_status_leds();
    // Laisser le temps au port USB CDC de s'enumerer avant les messages.
    sleep_ms(2000);

    // Initialise the Wi-Fi chip
    if (cyw43_arch_init()) {
        printf("Wi-Fi init failed\n");
        return -1;
    }

    // Bouton facultatif : GP4 vers GND. Il n'est pas necessaire au premier
    // demarrage, mais permet de reouvrir le portail Wi-Fi par appui long.
    gpio_init(MODE_BUTTON_PIN);
    gpio_set_dir(MODE_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(MODE_BUTTON_PIN);
    button_long_press_armed = true;

    WifiCredentials credentials = {};
    bool have_credentials = load_credentials(&credentials);
    if (!have_credentials && WIFI_SSID[0] && WIFI_PASSWORD[0]) {
        // This fallback is useful only to developers. Release builds leave it empty.
        credentials.magic = WIFI_CONFIG_MAGIC;
        credentials.version = WIFI_CONFIG_VERSION;
        strncpy(credentials.ssid, WIFI_SSID, sizeof(credentials.ssid) - 1);
        strncpy(credentials.password, WIFI_PASSWORD, sizeof(credentials.password) - 1);
        credentials.checksum = credentials_checksum(credentials);
        have_credentials = credentials_are_valid(credentials);
    }

    if (have_credentials) {
        cyw43_arch_enable_sta_mode();
        printf("Connecting to saved Wi-Fi...\n");
        if (cyw43_arch_wifi_connect_timeout_ms(credentials.ssid, credentials.password,
                                               WIFI_AUTH, 30000) == 0) {
            station_connected = true;
            printf("Connected.\n");
            uint32_t ip = cyw43_state.netif[0].ip_addr.addr;
            printf("IP address %lu.%lu.%lu.%lu\n",
                   ip & 0xff, (ip >> 8) & 0xff, (ip >> 16) & 0xff, (ip >> 24) & 0xff);
        } else {
            printf("Saved Wi-Fi unavailable; opening setup portal.\n");
            cyw43_arch_disable_sta_mode();
        }
    }
    if (!station_connected && !start_setup_portal()) return 1;

    // Set up our UART
    uart_init(UART_ID, BAUD_RATE);
    // Set the TX and RX pins by using the function select on the GPIO
    // Set datasheet for more information on function select
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    irq_set_exclusive_handler(UART0_IRQ, uart_rx_irq_handler);
    irq_set_enabled(UART0_IRQ, true);
    uart_set_irq_enables(UART_ID, true, false);

    ip4_addr_t slip_ip, slip_mask, slip_gw;
    IP4_ADDR(&slip_ip, 192, 168, 7, 1);
    IP4_ADDR(&slip_mask, 255, 255, 255, 0);
    IP4_ADDR(&slip_gw, 0, 0, 0, 0);
    netif_add(&slip_netif, &slip_ip, &slip_mask, &slip_gw,
              nullptr, slip_netif_init, ip_input);
    netif_set_up(&slip_netif);
    netif_set_link_up(&slip_netif);
    printf("lwIP SLIP interface ready: 192.168.7.1\n");

    if (station_connected) {
        // Utiliser le Wi-Fi comme route par défaut et tracer ses sorties.
        struct netif *wifi_netif = &cyw43_state.netif[0];
        original_wifi_output = wifi_netif->output;
        wifi_netif->output = wifi_output_trace;
        original_wifi_input = wifi_netif->input;
        wifi_netif->input = wifi_input_trace;
        active_network_netif = wifi_netif;
        netif_set_default(wifi_netif);
        printf("Wi-Fi set as default lwIP route\n");
    }
    
    // Use some the various UART functions to send out data
    // In a default system, printf will also output via the default UART
    
    // Send out a string, with CR/LF conversions
    uart_puts(UART_ID, " Hello, UART!\n");
    
    // For more examples of UART use see https://github.com/raspberrypi/pico-examples/tree/master/uart

    uint32_t reported_uart_overflow = 0;
    while (true) {
        check_wifi_setup_button();
        while (uart_rx_tail != uart_rx_head) {
            uint32_t tail = uart_rx_tail;
            uint8_t byte = uart_rx_ring[tail];
            __dmb();
            uart_rx_tail = (tail + 1) & UART_RX_RING_MASK;
            slip_receive_byte(byte);
        }
        uint32_t overflow = uart_rx_overflow;
        if (overflow != reported_uart_overflow) {
            printf("UART RX ring overflow: %u bytes dropped total\n", overflow);
            reported_uart_overflow = overflow;
        }
        if (reboot_at_ms &&
            to_ms_since_boot(get_absolute_time()) >= reboot_at_ms) {
            printf("Restarting with new Wi-Fi credentials...\n");
            sleep_ms(100);
            watchdog_reboot(0, 0, 0);
        }
        uint32_t now = to_ms_since_boot(get_absolute_time());
        update_status_leds(now);
        tight_loop_contents();
    }

}
