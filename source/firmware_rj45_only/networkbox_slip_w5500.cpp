#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
extern "C" {
#include "wizchip_conf.h"
#include "socket.h"
#include "w5x00_spi.h"
#include "w5x00_lwip.h"
}
#include <string.h>
#include "lwip/init.h"
#include "lwip/ip.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/timeouts.h"

extern "C" uint8_t mac[6];


// UART defines
// By default the stdout UART is `uart0`, so we will use the second one
#define UART_ID uart0
#define BAUD_RATE 19200

// Use pins 4 and 5 for UART1
// Pins can be changed, see the GPIO function select table in the datasheet for information on GPIO assignments
#define UART_TX_PIN 0
#define UART_RX_PIN 1

// Optional front-panel LEDs used by the NetworkBox enclosure.
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
static netif_output_fn original_ethernet_output = nullptr;
static netif_input_fn original_ethernet_input = nullptr;
static uint8_t ethernet_rx_frame[ETHERNET_MTU + 64];
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
static uint32_t activity_led_until_ms = 0;
static uint16_t checksum16(const uint8_t *data, size_t length);
static void slip_send_frame(const uint8_t *data, size_t length);

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
    gpio_put(LED_ETHERNET_PIN, 1); // This firmware is RJ45 only.
    gpio_put(LED_WIFI_PIN, 0);
    if (activity_led_until_ms && now >= activity_led_until_ms) {
        gpio_put(LED_ACTIVITY_PIN, 0);
        activity_led_until_ms = 0;
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

static err_t ethernet_output_trace(struct netif *netif, struct pbuf *p,
                               const ip4_addr_t *ipaddr) {
    mark_network_activity();
    if (ipaddr) {
        printf("W5500 TX to %u.%u.%u.%u (%u bytes)\n",
               ip4_addr1(ipaddr), ip4_addr2(ipaddr),
               ip4_addr3(ipaddr), ip4_addr4(ipaddr),
               (unsigned)p->tot_len);
    }
    err_t result = original_ethernet_output(netif, p, ipaddr);
    printf("W5500 output result: %d (up=%d link=%d)\n",
           (int)result, netif_is_up(netif) ? 1 : 0,
           netif_is_link_up(netif) ? 1 : 0);
    if (w5500_lwip_take_tx_timeout()) {
        printf("W5500 TX stalled: automatic restart scheduled.\n");
        watchdog_reboot(0, 0, 100);
    }
    return result;
}

static err_t network_output_ip_packet(struct netif *netif, struct pbuf *packet,
                                      const ip4_addr_t *destination) {
    ip4_addr_t next_hop = *destination;
    if (!ip4_addr_netcmp(destination, &netif->ip_addr, &netif->netmask))
        next_hop = netif->gw;
    return netif->output(netif, packet, &next_hop);
}

static err_t ethernet_input_trace(struct pbuf *p, struct netif *netif) {
    mark_network_activity();
    printf("W5500 input frame: %u bytes\n", p ? (unsigned)p->tot_len : 0u);
    constexpr size_t ETH_HEADER_LEN = 14;
    if (p && p->tot_len >= ETH_HEADER_LEN + 20) {
        uint8_t header[60] = {};
        size_t available_ip_bytes = p->tot_len - ETH_HEADER_LEN;
        size_t header_bytes = available_ip_bytes < sizeof(header) ?
                              available_ip_bytes : sizeof(header);
        pbuf_copy_partial(p, header, header_bytes, ETH_HEADER_LEN);
        if ((header[0] >> 4) == 4) {
            size_t ihl = (header[0] & 0x0f) * 4;
            printf("W5500 RX IPv4 %u.%u.%u.%u -> %u.%u.%u.%u (%u bytes)\n",
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
                if (id == nat_icmp_id && seq == nat_icmp_sequence) {
                    uint8_t reply[SLIP_MAX_FRAME];
                    size_t ip_length = p->tot_len - ETH_HEADER_LEN;
                    if (ip_length <= sizeof(reply)) {
                        pbuf_copy_partial(p, reply, ip_length, ETH_HEADER_LEN);
                        memcpy(reply + 16, nat_icmp_client_ip, 4);
                        recalc_icmp_checksums(reply, ip_length);
                        slip_send_frame(reply, ip_length);
                        printf("NAT ICMP reply -> Atari (%u bytes)\n", (unsigned)ip_length);
                        nat_icmp_active = false;
                        pbuf_free(p);
                        return ERR_OK;
                    }
                }
            }
        }
    }
    return original_ethernet_input(p, netif);
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

static bool nat_send_icmp_request(struct pbuf *input, struct netif *network_netif) {
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
    memcpy(packet + 12, &network_netif->ip_addr.addr, 4);
    recalc_icmp_checksums(packet, input->tot_len);
    // PBUF_LINK réserve la place nécessaire à l'entête Ethernet du W5500.
    struct pbuf *out = pbuf_alloc(PBUF_LINK, input->tot_len, PBUF_RAM);
    if (!out) return false;
    pbuf_take(out, packet, input->tot_len);
    ip4_addr_t destination;
    memcpy(&destination.addr, packet + 16, 4);
    err_t result = network_output_ip_packet(network_netif, out, &destination);
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

static bool nat_send_dns_request(struct pbuf *input, struct netif *network_netif) {
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
    memcpy(packet + 12, &network_netif->ip_addr.addr, 4);
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
    printf("NAT DNS query ID=%04X -> %u.%u.%u.%u\n",
           nat_dns_transaction_id, packet[16], packet[17], packet[18], packet[19]);
    err_t result = network_output_ip_packet(network_netif, out, &destination);
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

static bool nat_send_tcp_packet(struct pbuf *input, struct netif *network_netif) {
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
    memcpy(packet + 12, &network_netif->ip_addr.addr, 4);
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
    err_t result = network_output_ip_packet(network_netif, out, &destination);
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
    struct pbuf *p = pbuf_alloc(PBUF_RAW, slip_length, PBUF_POOL);
    if (!p) {
        printf("SLIP lwIP allocation failed\n");
        return;
    }
    pbuf_take(p, slip_frame, slip_length);
    struct netif *network_netif = &ethernet_netif;
    if (nat_send_dns_request(p, network_netif)) {
        pbuf_free(p);
        return;
    }
    if (nat_send_tcp_packet(p, network_netif)) {
        pbuf_free(p);
        return;
    }
    if (nat_send_icmp_request(p, network_netif)) {
        pbuf_free(p);
        return;
    }
    err_t result = slip_netif.input(p, &slip_netif);
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



int main() {
    stdio_init_all();
    init_status_leds();
    sleep_ms(2000); // USB CDC monitor enumeration.
    printf("NetworkBox W5500 / SLIP boot\n");

    // UART RS-232 side: GP0 TX, GP1 RX, 19200 8N1.
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    irq_set_exclusive_handler(UART0_IRQ, uart_rx_irq_handler);
    irq_set_enabled(UART0_IRQ, true);
    uart_set_irq_enables(UART_ID, true, false);

    // W5500 SPI0 wiring already used by the Network Box:
    // MISO GP16, CS GP17, SCK GP18, MOSI GP19, RESET GP20.
    printf("Initialising W5500 on SPI0...\n");
    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
    wizchip_initialize();
    wizchip_check();
    setSHAR(mac);
    printf("W5500 chip ready, version=%u\n", getVERSIONR());
    uint8_t phy_status = PHY_LINK_OFF;
    while (phy_status == PHY_LINK_OFF) {
        ctlwizchip(CW_GET_PHYLINK, &phy_status);
        if (phy_status == PHY_LINK_OFF) {
            printf("W5500: waiting for Ethernet link (check RJ45 cable)\n");
            sleep_ms(1000);
        }
    }
    printf("W5500 Ethernet link up\n");

    int8_t socket_result = socket(0, Sn_MR_MACRAW, 0, 0);
    if (socket_result < 0) {
        printf("W5500 MACRAW socket failed: %d\n", (int)socket_result);
        return 1;
    }
    printf("W5500 MACRAW socket ready\n");

    lwip_init();
    netif_add(&ethernet_netif, IP4_ADDR_ANY, IP4_ADDR_ANY, IP4_ADDR_ANY,
              nullptr, netif_initialize, netif_input);
    ethernet_netif.name[0] = 'e';
    ethernet_netif.name[1] = '0';
    netif_set_link_callback(&ethernet_netif, netif_link_callback);
    original_ethernet_output = ethernet_netif.output;
    ethernet_netif.output = ethernet_output_trace;
    original_ethernet_input = ethernet_netif.input;
    ethernet_netif.input = ethernet_input_trace;
    netif_set_default(&ethernet_netif);
    netif_set_link_up(&ethernet_netif);
    netif_set_up(&ethernet_netif);
    err_t dhcp_result = dhcp_start(&ethernet_netif);
    printf("W5500 DHCP start: %d\n", (int)dhcp_result);

    ip4_addr_t slip_ip, slip_mask, slip_gw;
    IP4_ADDR(&slip_ip, 192, 168, 7, 1);
    IP4_ADDR(&slip_mask, 255, 255, 255, 0);
    IP4_ADDR(&slip_gw, 0, 0, 0, 0);
    netif_add(&slip_netif, &slip_ip, &slip_mask, &slip_gw,
              nullptr, slip_netif_init, ip_input);
    netif_set_up(&slip_netif);
    netif_set_link_up(&slip_netif);
    printf("lwIP SLIP interface ready: 192.168.7.1\n");

    uint32_t reported_uart_overflow = 0;
    bool dhcp_reported = false;
    watchdog_enable(5000, true);
    watchdog_update();
    printf("NetworkBox watchdog enabled (5 s).\n");
    while (true) {
        watchdog_update();
        // Drain Ethernet frames from W5500 socket 0 into lwIP.
        uint16_t available = 0;
        getsockopt(0, SO_RECVBUF, &available);
        if (available > 0) {
            int32_t frame_length = recv_lwip(0, ethernet_rx_frame,
                                             sizeof(ethernet_rx_frame));
            if (frame_length > 0 && frame_length <= (int32_t)sizeof(ethernet_rx_frame)) {
                struct pbuf *frame = pbuf_alloc(PBUF_RAW, (u16_t)frame_length, PBUF_POOL);
                if (frame) {
                    pbuf_take(frame, ethernet_rx_frame, (u16_t)frame_length);
                    err_t input_result = ethernet_netif.input(frame, &ethernet_netif);
                    if (input_result != ERR_OK) pbuf_free(frame);
                } else {
                    printf("W5500 RX: lwIP pbuf allocation failed\n");
                }
            }
        }

        // Process Atari SLIP bytes; UART RX interrupt keeps buffering meanwhile.
        while (uart_rx_tail != uart_rx_head) {
            uint32_t tail = uart_rx_tail;
            uint8_t byte = uart_rx_ring[tail];
            __dmb();
            uart_rx_tail = (tail + 1) & UART_RX_RING_MASK;
            slip_receive_byte(byte);
        }

        sys_check_timeouts(); // DHCP, ARP, TCP and UDP timers.
        uint32_t overflow = uart_rx_overflow;
        if (overflow != reported_uart_overflow) {
            printf("UART RX ring overflow: %u bytes dropped total\n", overflow);
            reported_uart_overflow = overflow;
        }
        uint32_t now = to_ms_since_boot(get_absolute_time());
        update_status_leds(now);
        if (!dhcp_reported && !ip4_addr_isany_val(ethernet_netif.ip_addr)) {
            printf("NetworkBox active - W5500 IP %s\n",
                   ip4addr_ntoa(&ethernet_netif.ip_addr));
            dhcp_reported = true;
        }
        tight_loop_contents();
    }
}
