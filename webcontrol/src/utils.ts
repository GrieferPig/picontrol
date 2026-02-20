export function midiNoteName(n: number): string {
    if (typeof n !== 'number' || n < 0 || n > 127) return String(n);
    const names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
    const name = names[n % 12];
    const octave = Math.floor(n / 12) - 1; // MIDI: C4 = 60
    return `${name}${octave}`;
}

export function noteNumberToParts(n: number) {
    if (typeof n !== 'number' || n < 0 || n > 127) return { noteIndex: 0, octave: 4, noteNumber: 60 };
    return {
        noteIndex: n % 12,
        octave: Math.floor(n / 12) - 1,
        noteNumber: n,
    };
}

export function notePartsToNumber(noteIndex: number, octave: number) {
    const idx = Math.max(0, Math.min(11, noteIndex | 0));
    const oct = Math.max(-1, Math.min(9, octave | 0));
    const n = (oct + 1) * 12 + idx;
    return Math.max(0, Math.min(127, n));
}

export function hidModifierMaskFromEvent(e: KeyboardEvent) {
    let m = 0;
    if (e.ctrlKey) m |= 0x01;
    if (e.shiftKey) m |= 0x02;
    if (e.altKey) m |= 0x04;
    if (e.metaKey) m |= 0x08;
    return m;
}

export function hidKeycodeFromKeyboardEvent(e: KeyboardEvent) {
    if (typeof e.code === 'string' && e.code.startsWith('Key') && e.code.length === 4) {
        const ch = e.code[3];
        if (ch) {
            const idx = ch.charCodeAt(0) - 'A'.charCodeAt(0);
            if (idx >= 0 && idx < 26) return 4 + idx;
        }
    }
    if (typeof e.code === 'string' && e.code.startsWith('Digit') && e.code.length === 6) {
        const ch = e.code[5];
        if (ch) {
            if (ch >= '1' && ch <= '9') return 30 + (ch.charCodeAt(0) - '1'.charCodeAt(0));
            if (ch === '0') return 39;
        }
    }

    const codeMap: Record<string, number> = {
        Enter: 40,
        Escape: 41,
        Backspace: 42,
        Tab: 43,
        Space: 44,
        Minus: 45,
        Equal: 46,
        BracketLeft: 47,
        BracketRight: 48,
        Backslash: 49,
        Semicolon: 51,
        Quote: 52,
        Backquote: 53,
        Comma: 54,
        Period: 55,
        Slash: 56,
        CapsLock: 57,
        ArrowRight: 79,
        ArrowLeft: 80,
        ArrowDown: 81,
        ArrowUp: 82,
        Delete: 76,
        Home: 74,
        End: 77,
        PageUp: 75,
        PageDown: 78,
    };
    if (e.code && codeMap[e.code] != null) return codeMap[e.code];
    return 0;
}

export function hidKeyLabel(code: number) {
    const map: Record<number, string> = {
        4: 'A', 5: 'B', 6: 'C', 7: 'D', 8: 'E', 9: 'F', 10: 'G', 11: 'H', 12: 'I', 13: 'J', 14: 'K', 15: 'L', 16: 'M', 17: 'N', 18: 'O', 19: 'P', 20: 'Q', 21: 'R', 22: 'S', 23: 'T', 24: 'U', 25: 'V', 26: 'W', 27: 'X', 28: 'Y', 29: 'Z',
        30: '1', 31: '2', 32: '3', 33: '4', 34: '5', 35: '6', 36: '7', 37: '8', 38: '9', 39: '0',
        40: 'Enter', 41: 'Esc', 42: 'Backspace', 43: 'Tab', 44: 'Space', 45: '-', 46: '=', 47: '[', 48: ']', 49: '\\', 50: '#', 51: ';', 52: '\'', 53: '`', 54: ',', 55: '.', 56: '/',
        57: 'CapsLock',
    };
    const name = map[code] || `Keycode ${code}`;
    return `${name} [${code}]`;
}

export function formatKeyComboDisplay(keycode: number, modifierMask: number) {
    const parts = [];
    if (modifierMask & 0x01) parts.push('Ctrl');
    if (modifierMask & 0x02) parts.push('Shift');
    if (modifierMask & 0x04) parts.push('Alt');
    if (modifierMask & 0x08) parts.push('Meta');
    if (keycode) {
        const label = hidKeyLabel(keycode);
        parts.push(label.replace(/\s\[\d+\]$/, ''));
    }
    return parts.join('+') || '';
}

export function midiNoteLabel(n: number) {
    return `${midiNoteName(n)} [${n}]`;
}
