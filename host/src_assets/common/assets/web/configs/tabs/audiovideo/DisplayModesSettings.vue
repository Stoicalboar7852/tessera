<script setup>
import { ref } from 'vue'
import Checkbox from '../../../Checkbox.vue'
const props = defineProps({
  platform: String,
  config: Object,
})
const config = ref(props.config)
</script>

<template>
  <!--max_bitrate-->
  <div class="mb-3">
    <label for="max_bitrate" class="form-label">{{ $t("config.max_bitrate") }}</label>
    <input type="number" class="form-control" id="max_bitrate" placeholder="0" v-model="config.max_bitrate" />
    <div class="form-text">{{ $t("config.max_bitrate_desc") }}</div>
  </div>

  <!--minimum_fps_target-->
  <div class="mb-3">
    <label for="minimum_fps_target" class="form-label">{{ $t("config.minimum_fps_target") }}</label>
    <input type="number" min="0" max="1000" class="form-control" id="minimum_fps_target" placeholder="0" v-model="config.minimum_fps_target" />
    <div class="form-text">{{ $t("config.minimum_fps_target_desc") }}</div>
  </div>

  <!-- [fork] Tessera: multi-display streaming -->
  <template v-if="platform === 'windows'">
    <!--tessera_max_streams-->
    <div class="mb-3">
      <label for="tessera_max_streams" class="form-label">{{ $t("config.tessera_max_streams") }}</label>
      <input type="number" min="1" max="4" class="form-control" id="tessera_max_streams" placeholder="4" v-model="config.tessera_max_streams" />
      <div class="form-text">{{ $t("config.tessera_max_streams_desc") }}</div>
    </div>

    <!--tessera_display_source-->
    <div class="mb-3">
      <label for="tessera_display_source" class="form-label">{{ $t("config.tessera_display_source") }}</label>
      <select id="tessera_display_source" class="form-select" v-model="config.tessera_display_source">
        <option value="virtual">{{ $t("config.tessera_display_source_virtual") }}</option>
        <option value="physical">{{ $t("config.tessera_display_source_physical") }}</option>
      </select>
      <div class="form-text">{{ $t("config.tessera_display_source_desc") }}</div>
    </div>

    <!--tessera_make_primary-->
    <Checkbox class="mb-3"
              id="tessera_make_primary"
              locale-prefix="config"
              v-model="config.tessera_make_primary"
              default="true"
    ></Checkbox>
  </template>
</template>

<style scoped>
.ms-item {
  background-color: var(--bs-dark-bg-subtle);
  font-size: 12px;
  font-weight: bold;
}
</style>
