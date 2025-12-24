<script setup lang="ts">
import { ref, computed, watch, onMounted } from 'vue';
import type { Curve, Point } from '../types';

const props = defineProps<{
  modelValue?: Curve;
  xLabel?: string;
  yLabel?: string;
  showThreshold?: boolean;
  xSteps?: number;
  ySteps?: number;
  maxPoints?: number;
  xDisplayMax?: number | string;
  yDisplayMax?: number | string;
}>();

const emit = defineEmits<{
  (e: 'update:modelValue', value: Curve): void;
}>();

// Default curve: Linear (0,0) -> (255,255)
const defaultCurve: Curve = {
  count: 2,
  points: [{ x: 0, y: 0 }, { x: 255, y: 255 }],
  controls: [{ x: 128, y: 128 }] // Midpoint for linear-ish
};

const localCurve = ref<Curve>(JSON.parse(JSON.stringify(defaultCurve)));

watch(() => props.modelValue, (newVal) => {
  if (newVal) {
    localCurve.value = JSON.parse(JSON.stringify(newVal));
  } else {
    localCurve.value = JSON.parse(JSON.stringify(defaultCurve));
  }
}, { immediate: true, deep: true });

// SVG Interaction
const svgRef = ref<SVGSVGElement | null>(null);
const dragging = ref<{ type: 'point' | 'control', index: number } | null>(null);

function snapValue(val: number, steps?: number): number {
  if (!steps || steps <= 0 || steps >= 256) return val;
  const stepSize = 255 / (steps - 1);
  const step = Math.round(val / stepSize);
  return Math.round(step * stepSize);
}

function getMousePos(e: MouseEvent | TouchEvent): Point {
  if (!svgRef.value) return { x: 0, y: 0 };
  const rect = svgRef.value.getBoundingClientRect();
  const clientX = 'touches' in e ? e.touches[0].clientX : e.clientX;
  const clientY = 'touches' in e ? e.touches[0].clientY : e.clientY;
  
  let x = (clientX - rect.left) / rect.width * 255;
  let y = 255 - (clientY - rect.top) / rect.height * 255; // Invert Y for display
  
  x = Math.max(0, Math.min(255, x));
  y = Math.max(0, Math.min(255, y));
  
  return {
    x: Math.round(x),
    y: Math.round(y)
  };
}

function startDrag(type: 'point' | 'control', index: number, e: MouseEvent | TouchEvent) {
  e.preventDefault();
  dragging.value = { type, index };
  window.addEventListener('mousemove', onDrag);
  window.addEventListener('mouseup', stopDrag);
  window.addEventListener('touchmove', onDrag);
  window.addEventListener('touchend', stopDrag);
}

function onDrag(e: MouseEvent | TouchEvent) {
  if (!dragging.value) return;
  let pos = getMousePos(e);
  
  // Apply snapping
  if (dragging.value.type === 'point') {
      pos.x = snapValue(pos.x, props.xSteps);
      pos.y = snapValue(pos.y, props.ySteps);
  } else {
      // Control points: maybe snap Y? X is less critical but consistent to snap.
      // Let's snap both for consistency if grid is coarse.
      pos.x = snapValue(pos.x, props.xSteps);
      pos.y = snapValue(pos.y, props.ySteps);
  }
  
  if (dragging.value.type === 'point') {
    // Constraints: x must be > prev point and < next point
    const idx = dragging.value.index;
    
    // First and last points are fixed to x=0 and x=255
    if (idx === 0) {
        pos.x = 0;
    } else if (idx === localCurve.value.points.length - 1) {
        pos.x = 255;
    } else {
        let minX = 0;
        let maxX = 255;
        
        if (idx > 0) minX = localCurve.value.points[idx - 1].x;
        if (idx < localCurve.value.points.length - 1) maxX = localCurve.value.points[idx + 1].x;
        
        // Enforce order
        if (pos.x < minX) pos.x = minX;
        if (pos.x > maxX) pos.x = maxX;
    }
    
    localCurve.value.points[idx] = pos;
  } else {
    // Control point
    // Constrain X to be between P_i and P_{i+1} to ensure function property
    const idx = dragging.value.index;
    const pStart = localCurve.value.points[idx];
    const pEnd = localCurve.value.points[idx+1];
    
    if (pos.x < pStart.x) pos.x = pStart.x;
    if (pos.x > pEnd.x) pos.x = pEnd.x;
    
    localCurve.value.controls[idx] = pos;
  }
  
  emitUpdate();
}

function stopDrag() {
  dragging.value = null;
  window.removeEventListener('mousemove', onDrag);
  window.removeEventListener('mouseup', stopDrag);
  window.removeEventListener('touchmove', onDrag);
  window.removeEventListener('touchend', stopDrag);
}

function emitUpdate() {
  emit('update:modelValue', JSON.parse(JSON.stringify(localCurve.value)));
}

function addPoint() {
  const limit = props.maxPoints ?? 5;
  if (localCurve.value.count >= limit) return;
  
  // Insert a point in the middle of the largest gap
  let maxGap = 0;
  let gapIdx = 0;
  for (let i = 0; i < localCurve.value.count - 1; i++) {
    const gap = localCurve.value.points[i+1].x - localCurve.value.points[i].x;
    if (gap > maxGap) {
      maxGap = gap;
      gapIdx = i;
    }
  }
  
  const p1 = localCurve.value.points[gapIdx];
  const p2 = localCurve.value.points[gapIdx+1];
  const newX = Math.round((p1.x + p2.x) / 2);
  const newY = Math.round((p1.y + p2.y) / 2);
  
  // Insert point
  localCurve.value.points.splice(gapIdx + 1, 0, { x: newX, y: newY });
  // Insert control point (duplicate the existing one or interpolate)
  localCurve.value.controls.splice(gapIdx, 0, { x: Math.round((p1.x + newX)/2), y: Math.round((p1.y + newY)/2) });
  // Update the next control point
  localCurve.value.controls[gapIdx+1] = { x: Math.round((newX + p2.x)/2), y: Math.round((newY + p2.y)/2) };
  
  localCurve.value.count++;
  emitUpdate();
}

function removePoint() {
  if (localCurve.value.count <= 2) return;
  
  // Remove the second to last point (keep start and end)
  const idx = localCurve.value.count - 2;
  localCurve.value.points.splice(idx, 1);
  localCurve.value.controls.splice(idx, 1); // Remove one control
  // Adjust the remaining control to bridge the gap
  // localCurve.value.controls[idx-1] ...
  
  localCurve.value.count--;
  emitUpdate();
}

// SVG Path generation
const pathD = computed(() => {
  if (!localCurve.value || localCurve.value.count < 2) return '';
  
  let d = `M ${localCurve.value.points[0].x} ${255 - localCurve.value.points[0].y}`;
  
  for (let i = 0; i < localCurve.value.count - 1; i++) {
    const pNext = localCurve.value.points[i+1];
    const c = localCurve.value.controls[i];
    
    // Quadratic Bezier: Q control, end
    d += ` Q ${c.x} ${255 - c.y}, ${pNext.x} ${255 - pNext.y}`;
  }
  
  return d;
});

</script>

<template>
  <div class="curve-editor">
    <div class="svg-wrapper">
      <div class="y-axis-label" v-if="yLabel">{{ yLabel }}</div>
      <div class="svg-container">
        <svg ref="svgRef" viewBox="-10 -10 275 275" preserveAspectRatio="xMidYMid meet">
          <!-- Grid Background -->
          <rect x="0" y="0" width="255" height="255" fill="var(--panel-strong)" stroke="var(--border)" />
          
          <!-- Grid Lines -->
          <line x1="0" y1="64" x2="255" y2="64" stroke="var(--border)" stroke-width="0.5" />
          <line x1="0" y1="128" x2="255" y2="128" stroke="var(--border)" stroke-width="0.5" />
          <line x1="0" y1="192" x2="255" y2="192" stroke="var(--border)" stroke-width="0.5" />
          <line x1="64" y1="0" x2="64" y2="255" stroke="var(--border)" stroke-width="0.5" />
          <line x1="128" y1="0" x2="128" y2="255" stroke="var(--border)" stroke-width="0.5" />
          <line x1="192" y1="0" x2="192" y2="255" stroke="var(--border)" stroke-width="0.5" />

          <!-- Threshold Line -->
          <line v-if="showThreshold" x1="0" y1="127" x2="255" y2="127" stroke="var(--accent-2)" stroke-dasharray="4,4" opacity="0.5" />
          
          <!-- Curve -->
          <path :d="pathD" fill="none" stroke="var(--accent)" stroke-width="3" />
          
          <!-- Control Lines -->
          <g v-for="(c, i) in localCurve.controls" :key="'cl-'+i">
            <line 
              :x1="localCurve.points[i].x" :y1="255 - localCurve.points[i].y" 
              :x2="c.x" :y2="255 - c.y" 
              stroke="var(--muted)" stroke-dasharray="2,2" opacity="0.5"
            />
            <line 
              :x1="c.x" :y1="255 - c.y" 
              :x2="localCurve.points[i+1].x" :y2="255 - localCurve.points[i+1].y" 
              stroke="var(--muted)" stroke-dasharray="2,2" opacity="0.5"
            />
          </g>
          
          <!-- Control Points -->
          <circle 
            v-for="(c, i) in localCurve.controls" :key="'c-'+i"
            :cx="c.x" :cy="255 - c.y" r="4" 
            fill="var(--accent-2)" cursor="pointer"
            @mousedown="startDrag('control', i, $event)"
            @touchstart="startDrag('control', i, $event)"
          />
          
          <!-- Points -->
          <circle 
            v-for="(p, i) in localCurve.points" :key="'p-'+i"
            :cx="p.x" :cy="255 - p.y" r="5" 
            fill="var(--fg)" cursor="pointer"
            @mousedown="startDrag('point', i, $event)"
            @touchstart="startDrag('point', i, $event)"
          />
        </svg>
        
        <!-- Axis Values -->
        <div class="axis-val y-max">{{ yDisplayMax ?? 255 }}</div>
        <div class="axis-val y-min">0</div>
        <div class="axis-val x-min">0</div>
        <div class="axis-val x-max">{{ xDisplayMax ?? 255 }}</div>
      </div>
    </div>
    <div class="x-axis-label" v-if="xLabel">{{ xLabel }}</div>
    
    <div class="controls">
      <button class="ghost" @click="addPoint" :disabled="localCurve.count >= (maxPoints ?? 5)" title="Add Point">+</button>
      <button class="ghost" @click="removePoint" :disabled="localCurve.count <= 2" title="Remove Point">-</button>
      <div class="info muted">
        {{ localCurve.count }} segments
      </div>
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
.x-min { bottom: 2px; left: 4px; display: none; } /* Overlaps y-min */
.x-max { bottom: 2px; right: 4px; }

.controls {
  display: flex;
  gap: 10px;
  align-items: center;
  justify-content: flex-end;
}

button {
  padding: 2px 8px;
  font-size: 14px;
}

.info {
  font-size: 12px;
}
</style>
