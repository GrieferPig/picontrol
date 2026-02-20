import { useSerial } from './serial';
import type { Curve } from '../types';
import {
    buildModulesListCmd,
    buildMapListCmd,
    buildMapSetCmd,
    buildMapSetCurveCmd,
    buildMapDelCmd,
    buildMapClearCmd,
    buildParamSetCmd,
    buildCalibSetCmd,
} from './protocol';

export function useRouter() {
    const { sendBinary } = useSerial();

    async function listModules() {
        await sendBinary(buildModulesListCmd());
    }

    async function listMappings() {
        await sendBinary(buildMapListCmd());
    }

    async function setMapping(
        row: number, col: number, paramId: number,
        actionType: number, d1: number, d2: number,
    ) {
        await sendBinary(buildMapSetCmd(row, col, paramId, actionType, d1, d2));
    }

    async function setCurve(
        row: number, col: number, paramId: number, curve: Curve,
    ) {
        await sendBinary(buildMapSetCurveCmd(row, col, paramId, curve));
    }

    async function deleteMapping(row: number, col: number, paramId: number) {
        await sendBinary(buildMapDelCmd(row, col, paramId));
    }

    async function clearMappings() {
        await sendBinary(buildMapClearCmd());
    }

    async function setParameter(
        row: number, col: number, paramId: number,
        dataType: number, valueStr: string,
    ) {
        await sendBinary(buildParamSetCmd(row, col, paramId, dataType, valueStr));
    }

    async function setCalibration(
        row: number, col: number, paramId: number,
        minValue: number, maxValue: number,
    ) {
        await sendBinary(buildCalibSetCmd(row, col, paramId, minValue, maxValue));
    }

    return {
        listModules,
        listMappings,
        setMapping,
        setCurve,
        deleteMapping,
        clearMappings,
        setParameter,
        setCalibration,
    };
}
