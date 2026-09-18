export interface ZeroExportConfig {
    zeroexport_enabled: boolean;
    zeroexport_setpoint: number;
    zeroexport_minimal_production: number;
    zeroexport_update_interval: number;
    zeroexport_source: number;
    zeroexport_shelly_lnm_addr: string;
    zeroexport_shelly_lnm_port: number;
    zeroexport_mqtt_grid_power_topic: string;
}
