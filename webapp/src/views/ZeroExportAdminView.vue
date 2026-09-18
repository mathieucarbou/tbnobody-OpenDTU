<template>
    <BasePage :title="$t('zeroexportadmin.ZeroExportSettings')" :isLoading="dataLoading">
        <BootstrapAlert
            v-model="alert.show"
            dismissible
            :variant="alert.type"
            :auto-dismiss="alert.type != 'success' ? 0 : 5000"
        >
            {{ alert.message }}
        </BootstrapAlert>

        <form @submit="saveConfig">
            <CardElement :text="$t('zeroexportadmin.ZeroExportConfiguration')" textVariant="text-bg-primary">
                <InputElement
                    :label="$t('zeroexportadmin.EnableZeroExport')"
                    v-model="cfg.zeroexport_enabled"
                    type="checkbox"
                    wide
                />

                <InputElement
                    :label="$t('zeroexportadmin.SetPoint')"
                    v-model="cfg.zeroexport_setpoint"
                    type="number"
                    step="1"
                    :postfix="$t('zeroexportadmin.Watts')"
                    :tooltip="$t('zeroexportadmin.SetPointHint')"
                    wide
                />

                <InputElement
                    :label="$t('zeroexportadmin.MinimalProduction')"
                    v-model="cfg.zeroexport_minimal_production"
                    type="number"
                    min="0"
                    step="1"
                    :postfix="$t('zeroexportadmin.Watts')"
                    :tooltip="$t('zeroexportadmin.MinimalProductionHint')"
                    wide
                />

                <InputElement
                    :label="$t('zeroexportadmin.UpdateInterval')"
                    v-model="cfg.zeroexport_update_interval"
                    type="number"
                    min="5"
                    max="3600"
                    :postfix="$t('zeroexportadmin.Seconds')"
                    :tooltip="$t('zeroexportadmin.UpdateIntervalHint')"
                    wide
                />
            </CardElement>

            <CardElement :text="$t('zeroexportadmin.GridSource')" textVariant="text-bg-primary" add-space>
                <div class="row mb-3">
                    <label for="inputSource" class="col-sm-4 col-form-label">
                        {{ $t('zeroexportadmin.Source') }}
                        <BIconInfoCircle v-tooltip :title="$t('zeroexportadmin.SourceHint')" />
                    </label>
                    <div class="col-sm-8">
                        <select id="inputSource" class="form-select" v-model="cfg.zeroexport_source">
                            <option v-for="source in sourceList" :key="source.key" :value="source.key">
                                {{ $t(`zeroexportadmin.` + source.value) }}
                            </option>
                        </select>
                    </div>
                </div>

                <div class="border rounded p-3" v-if="cfg.zeroexport_source == 1">
                    <h5>{{ $t('zeroexportadmin.ShellyLnmConfiguration') }}</h5>
                    <InputElement
                        :label="$t('zeroexportadmin.ShellyLnmAddr')"
                        v-model="cfg.zeroexport_shelly_lnm_addr"
                        type="text"
                        maxlength="15"
                        :tooltip="$t('zeroexportadmin.ShellyLnmAddrHint')"
                        wide
                    />
                    <InputElement
                        :label="$t('zeroexportadmin.ShellyLnmPort')"
                        v-model="cfg.zeroexport_shelly_lnm_port"
                        type="number"
                        min="1"
                        max="65535"
                        wide
                    />
                </div>

                <div class="border rounded p-3" v-if="cfg.zeroexport_source == 2">
                    <h5>{{ $t('zeroexportadmin.MqttConfiguration') }}</h5>
                    <InputElement
                        :label="$t('zeroexportadmin.MqttGridPowerTopic')"
                        v-model="cfg.zeroexport_mqtt_grid_power_topic"
                        type="text"
                        maxlength="127"
                        :tooltip="$t('zeroexportadmin.MqttGridPowerTopicHint')"
                        wide
                    />
                </div>
            </CardElement>

            <FormFooter @reload="getConfig" />
        </form>

        <CardElement :text="$t('zeroexportadmin.ZeroExportStatus')" textVariant="text-bg-primary" add-space>
            <div class="row mb-3">
                <label class="col-sm-4 col-form-label">{{ $t('zeroexportadmin.GridPower') }}</label>
                <div class="col-sm-8">
                    <input class="form-control" :value="status.zeroexport_grid_power + ' W'" disabled />
                </div>
            </div>
            <div class="row mb-3">
                <label class="col-sm-4 col-form-label">
                    {{ $t('zeroexportadmin.ProductionLimit') }}
                    <BIconInfoCircle v-tooltip :title="$t('zeroexportadmin.ProductionLimitHint')" />
                </label>
                <div class="col-sm-8">
                    <input class="form-control" :value="status.zeroexport_production_limit + ' W'" disabled />
                </div>
            </div>
        </CardElement>
    </BasePage>
</template>

<script lang="ts">
import BasePage from '@/components/BasePage.vue';
import BootstrapAlert from '@/components/BootstrapAlert.vue';
import CardElement from '@/components/CardElement.vue';
import InputElement from '@/components/InputElement.vue';
import FormFooter from '@/components/FormFooter.vue';
import type { AlertResponse } from '@/types/AlertResponse';
import type { ZeroExportConfig } from '@/types/ZeroExportConfig';
import { authHeader, handleResponse } from '@/utils/authentication';
import { defineComponent } from 'vue';
import { BIconInfoCircle } from 'bootstrap-icons-vue';

export default defineComponent({
    components: {
        BasePage,
        BootstrapAlert,
        CardElement,
        FormFooter,
        InputElement,
        BIconInfoCircle,
    },
    data() {
        return {
            dataLoading: true,
            statusLoading: false,
            cfg: {} as ZeroExportConfig,
            status: {} as Record<string, number | string>,
            alert: {} as AlertResponse,
            sourceList: [
                { key: 1, value: 'SourceShellyLnm' },
                { key: 2, value: 'SourceMqtt' },
            ],
            statusRefreshTimer: undefined as number | undefined,
        };
    },
    created() {
        this.getConfig();
        this.getStatus();
        this.statusRefreshTimer = window.setInterval(() => this.getStatus(), 5000);
    },
    beforeUnmount() {
        if (this.statusRefreshTimer !== undefined) {
            window.clearInterval(this.statusRefreshTimer);
        }
    },
    methods: {
        getConfig() {
            this.dataLoading = true;
            fetch('/api/zeroexport/config', { headers: authHeader() })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((data) => {
                    this.cfg = data;
                    this.dataLoading = false;
                });
        },
        getStatus() {
            if (this.statusLoading) {
                return;
            }
            this.statusLoading = true;
            fetch('/api/zeroexport/status', { headers: authHeader() })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((data) => {
                    this.status = data;
                    this.statusLoading = false;
                });
        },
        saveConfig(e: Event) {
            e.preventDefault();

            const formData = new FormData();
            formData.append('data', JSON.stringify(this.cfg));

            fetch('/api/zeroexport/config', {
                method: 'POST',
                headers: authHeader(),
                body: formData,
            })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((response) => {
                    this.alert.message = this.$t('apiresponse.' + response.code, response.param);
                    this.alert.type = response.type;
                    this.alert.show = true;
                })
                .then(() => {
                    this.getConfig();
                });
        },
    },
});
</script>
