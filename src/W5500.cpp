// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2024 Thomas Basler and others
 */

#include "W5500.h"

#include <SpiManager.h>
#include <driver/spi_master.h>

// Internal Arduino functions from WiFiGeneric
void tcpipInit();
void add_esp_interface_netif(esp_interface_t interface, esp_netif_t* esp_netif);

W5500::W5500(int8_t pin_mosi, int8_t pin_miso, int8_t pin_sclk, int8_t pin_cs, int8_t pin_int, int8_t pin_rst)
    : eth_handle(nullptr)
    , eth_netif(nullptr)
{
    gpio_reset_pin(static_cast<gpio_num_t>(pin_rst));
    gpio_set_level(static_cast<gpio_num_t>(pin_rst), 0);
    gpio_set_direction(static_cast<gpio_num_t>(pin_rst), GPIO_MODE_OUTPUT);

    gpio_reset_pin(static_cast<gpio_num_t>(pin_cs));
    gpio_reset_pin(static_cast<gpio_num_t>(pin_int));

    esp_err_t err = gpio_install_isr_service(ARDUINO_ISR_FLAG);
    if (err != ESP_ERR_INVALID_STATE) // don't raise an error when ISR service is already installed
        ESP_ERROR_CHECK(err);

    auto bus_config = std::make_shared<SpiBusConfig>(
        static_cast<gpio_num_t>(pin_mosi),
        static_cast<gpio_num_t>(pin_miso),
        static_cast<gpio_num_t>(pin_sclk));

    spi_device_interface_config_t device_config {
        .command_bits = 16, // actually address phase
        .address_bits = 8, // actually command phase
        .dummy_bits = 0,
        .mode = 0,
        .duty_cycle_pos = 0,
        .cs_ena_pretrans = 0, // only 0 supported
        .cs_ena_posttrans = 0, // only 0 supported
        .clock_speed_hz = 20000000, // stable with OpenDTU Fusion shield
        .input_delay_ns = 0,
        .spics_io_num = pin_cs,
        .flags = 0,
        .queue_size = 20,
        .pre_cb = nullptr,
        .post_cb = nullptr,
    };

    spi_device_handle_t spi = SpiManagerInst.alloc_device("", bus_config, device_config);
    if (!spi)
        ESP_ERROR_CHECK(ESP_FAIL);

    // Reset sequence
    delayMicroseconds(500);
    gpio_set_level(static_cast<gpio_num_t>(pin_rst), 1);
    delayMicroseconds(1000);

    // Arduino function to start networking stack if not already started
    tcpipInit();

    ESP_ERROR_CHECK(tcpip_adapter_set_default_eth_handlers());

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(spi);
    w5500_config.int_gpio_num = pin_int;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096;
    esp_eth_mac_t* mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = -1;
    esp_eth_phy_t* phy = esp_eth_phy_new_w5500(&phy_config);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    // Configure MAC address
    uint8_t mac_addr[6];
    ESP_ERROR_CHECK(esp_read_mac(mac_addr, ESP_MAC_ETH));
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr));

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    eth_netif = esp_netif_new(&netif_config);

    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    // Add to Arduino
    add_esp_interface_netif(ESP_IF_ETH, eth_netif);

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
}

W5500::~W5500()
{
    // TODO(LennartF22): support cleanup at some point?
}

String W5500::macAddress()
{
    uint8_t mac_addr[6] = {};
    esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);

    char mac_addr_str[18];
    snprintf(
        mac_addr_str, sizeof(mac_addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
        mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    return String(mac_addr_str);
}