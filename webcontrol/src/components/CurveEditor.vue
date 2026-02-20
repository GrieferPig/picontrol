<script setup lang="ts">
import { ref, computed, watch } from 'vue';
import type { Curve } from '../types';

const props = defineProps<{
  modelValue?: Curve;
  xLabel?: string;
  yLabel?: string;
  showThreshold?: boolean;
  xSteps?: number;
  ySteps?: number;
  maxPoints?: number;
  xDisplayMax?: number | string;
  yDisplayMin?: number | string;
  yDisplayMax?: number | string;
}>();

const emit = defineEmits<{
  (e: 'update:modelValue', value: Curve): void;
}>();

// Default curve: Linear (h = 16384 = 0.5 in Q15)
const defaultCurve: Curve = { h: 16384 };

const localCurve = ref<Curve>({ ...defaultCurve });

watch(() => props.modelValue, (newVal) => {
  if (newVal) {
    localCurve.value = { ...newVal };
  } else {
    localCurve.value = { ...defaultCurve };
  }
}, { immediate: true, deep: true });

// Slider value: map h (0..32767) to a -100..+100 display range
// h=16384 -> 0 (linear), h=0 -> -100 (concave), h=32767 -> +100 (convex)
const sliderValue = computed({
  get() {
    return Math.round(((localCurve.value.h - 16384) / 16384) * 100);
  },
  set(val: number) {
    const h = Math.round(16384 + (val / 100) * 16384);
    localCurve.value.h = Math.max(1, Math.min(32767, h));
    emitUpdate();
  }
});

function emitUpdate() {
  emit('update:modelValue', { ...localCurve.value });
}

function resetCurve() {
  localCurve.value.h = 16384;
  emitUpdate();
}

// Evaluate curve for preview: y = x*h / (x*h + (1-x)*(1-h))
function evalCurve(xNorm: number): number {
  const h = localCurve.value.h / 32768;
  if (xNorm <= 0) return 0;
  if (xNorm >= 1) return 1;
  const term1 = xNorm * h;
  const term2 = (1 - xNorm) * (1 - h);
  const denom = term1 + term2;
  if (denom === 0) return 0;
  return term1 / denom;
}

// Generate SVG polyline points for curve preview
const curvePoints = computed(() => {
  const steps = 64;
  const pts: string[] = [];
  for (let i = 0; i <= steps; i++) {
    const xNorm = i / steps;
    const yNorm = evalCurve(xNorm);
    const svgX = xNorm * 255;
    const svgY = 255 - yNorm * 255;
    pts.push(`${svgX.toFixed(1)},${svgY.toFixed(1)}`);
  }
  return pts.join(' ');
});

const shapeLabel = computed(() => {
  const v = sliderValue.value;
  if (v === 0) return 'Linear';
  if (v < 0) return `Concave (${v}%)`;
  return `Convex (+${v}%)`;
});
</script>

<template>
  <div class="curve-editor">
    <div class="svg-wrapper">
      <div class="y-axis-label" v-if="yLabel">{{ yLabel }}</div>
      <div class="svg-container">
        <svg viewBox="-10 -10 275 275" preserveAspectRatio="xMidYMid meet">
          <!-- Grid Background -->
          <rect x="0" y="0" width="255" height="255" fill="var(--panel-strong)" stroke="var(--border)" />
          
          <!-- Grid Lines -->
          <line x1="0" y1="64" x2="255" y2="64" stroke="var(--border)" stroke-width="0.5" />
          <line x1="0" y1="128" x2="255" y2="128" stroke="var(--border)" stroke-width="0.5" />
          <line x1="0" y1="192" x2="255" y2="192" stroke="var(--border)" stroke-width="0.5" />
          <line x1="64" y1="0" x2="64" y2="255" stroke="var(--border)" stroke-width="0.5" />
          <line x1="128" y1="0" x2="128" y2="255" stroke="var(--border)" stroke-width="0.5" />
          <line x1="192" y1="0" x2="192" y2="255" stroke="var(--border)" stroke-width="0.5" />

          <!-- Diagonal reference (linear) -->
          <line x1="0" y1="255" x2="255" y2="0" stroke="var(--muted)" stroke-dasharray="4,4" opacity="0.3" />

          <!-- Threshold Line -->
          <line v-if="showThreshold" x1="0" y1="127" x2="255" y2="127" stroke="var(--accent-2)" stroke-dasharray="4,4" opacity="0.5" />
          
          <!-- Curve -->
          <polyline :points="curvePoints" fill="none" stroke="var(--accent)" stroke-width="3" />
        </svg>
        
        <!-- Axis Values -->
        <div class="axis-val y-max">{{ yDisplayMax ?? 255 }}</div>
        <div class="axis-val y-min">{{ yDisplayMin ?? 0 }}</div>
        <div class="axis-val x-min">0</div>
        <div class="axis-val x-max">{{ xDisplayMax ?? 255 }}</div>
      </div>
    </div>
    <div class="x-axis-label" v-if="xLabel">{{ xLabel }}</div>
    
    <div class="controls">
      <label class="slider-label">Shape: {{ shapeLabel }}</label>
      <input type="range" min="-100" max="100" step="1" v-model.number="sliderValue" class="shape-slider" />
      <button class="ghost" @click="resetCurve" title="Reset to Linear">Reset</button>
    </div>
  </div>
</template>

<style scoped>
.curve-editor {
  display: flex;
  flex-direction: column;
  gap: 4px;
  width: 100%;
  max-width: 400px;
  margin-top: 8px;
}

.svg-wrapper {
  display: flex;
  gap: 8px;
  align-items: center;
}

.y-axis-label {
  writing-mode: vertical-rl;
  transform: rotate(180deg);
  font-size: 12px;
  color: var(--muted);
  text-align: center;
}

.x-axis-label {
  text-align: center;
  font-size: 12px;
  color: var(--muted);
}

.svg-container {
  position: relative;
  width: 100%;
  aspect-ratio: 1;
  border: 1px solid var(--border);
  background: var(--panel);
  border-radius: 4px;
}

.axis-val {
  position: absolute;
  font-size: 10px;
  color: var(--muted);
  pointer-events: none;
}

.y-max { top: 2px; left: 4px; }
.y-min { bottom: 2px; left: 4px; }
.x-min { bottom: 2px; left: 4px; display: none; }
.x-max { bottom: 2px; right: 4px; }

.controls {
  display: flex;
  gap: 10px;
  align-items: center;
  flex-wrap: wrap;
}

.slider-label {
  font-size: 12px;
  color: var(--muted);
  white-space: nowrap;
}

.shape-slider {
  flex: 1;
  min-width: 100px;
}

button {
  padding: 2px 8px;
  font-size: 14px;
}
</style>
