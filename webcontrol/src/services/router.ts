import { useSerial } from './serial';

export function useRouter() {
    const { send: serialSend } = useSerial();

    async function send(cmd: string) {
        await serialSend(cmd);
    }

    return { send };
}
