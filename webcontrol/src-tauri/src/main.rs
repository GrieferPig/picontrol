#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::Serialize;
use serialport::SerialPort;
use std::io::{Read, Write};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::thread::JoinHandle;
use std::time::{Duration, Instant};
use tauri::Emitter;

const HEADER_SIZE: usize = 5;
const CHECKSUM_SIZE: usize = 2;
const MIN_MESSAGE_SIZE: usize = HEADER_SIZE + CHECKSUM_SIZE;
const MAX_MESSAGE_SIZE: usize = 8192;

#[derive(Default)]
struct AppState {
    connection: Mutex<Option<SerialConnection>>,
}

struct SerialConnection {
    writer: Arc<Mutex<Box<dyn SerialPort + Send>>>,
    stop_flag: Arc<AtomicBool>,
    reader_thread: Option<JoinHandle<()>>,
}

#[derive(Serialize)]
struct SerialPortInfoDto {
    name: String,
    port_type: String,
}

fn to_port_type_label(port_type: &serialport::SerialPortType) -> String {
    match port_type {
        serialport::SerialPortType::UsbPort(info) => {
            format!("USB {:04x}:{:04x}", info.vid, info.pid)
        }
        serialport::SerialPortType::BluetoothPort => "Bluetooth".to_string(),
        serialport::SerialPortType::PciPort => "PCI".to_string(),
        serialport::SerialPortType::Unknown => "Unknown".to_string(),
    }
}

fn crc16_update(mut crc: u16, byte: u8) -> u16 {
    crc ^= (byte as u16) << 8;
    for _ in 0..8 {
        if (crc & 0x8000) != 0 {
            crc = (crc << 1) ^ 0x1021;
        } else {
            crc <<= 1;
        }
    }
    crc
}

fn calculate_crc16(data: &[u8]) -> u16 {
    let mut crc = 0xFFFFu16;
    for byte in data {
        crc = crc16_update(crc, *byte);
    }
    crc
}

fn has_valid_firmware_frame(buffer: &[u8]) -> bool {
    if buffer.len() < MIN_MESSAGE_SIZE {
        return false;
    }

    for offset in 0..=(buffer.len() - MIN_MESSAGE_SIZE) {
        let msg_type = buffer[offset];
        if msg_type != 1 && msg_type != 2 {
            continue;
        }

        let length = (buffer[offset + 3] as usize) | ((buffer[offset + 4] as usize) << 8);
        let expected_total = HEADER_SIZE + CHECKSUM_SIZE + length;

        if expected_total > MAX_MESSAGE_SIZE {
            continue;
        }

        if offset + expected_total > buffer.len() {
            continue;
        }

        let checksum_offset = offset + 5;
        let checksum = u16::from_le_bytes([buffer[checksum_offset], buffer[checksum_offset + 1]]);

        let mut crc = calculate_crc16(&buffer[offset..offset + HEADER_SIZE]);
        let payload_start = offset + HEADER_SIZE + CHECKSUM_SIZE;
        let payload_end = payload_start + length;
        for byte in &buffer[payload_start..payload_end] {
            crc = crc16_update(crc, *byte);
        }

        if crc == checksum {
            return true;
        }
    }

    false
}

fn probe_port_once(
    port_name: &str,
    baud_rate: u32,
    probe_data: &[u8],
    timeout: Duration,
) -> Result<bool, String> {
    let mut port = serialport::new(port_name, baud_rate)
        .timeout(Duration::from_millis(60))
        .open()
        .map_err(|err| err.to_string())?;

    let _ = port.write_data_terminal_ready(true);
    let _ = port.write_request_to_send(true);

    let drain_start = Instant::now();
    let mut drain_buf = [0_u8; 256];
    while drain_start.elapsed() < Duration::from_millis(160) {
        match port.read(&mut drain_buf) {
            Ok(_) => {}
            Err(err) if err.kind() == std::io::ErrorKind::TimedOut => break,
            Err(_) => break,
        }
    }

    port.write_all(probe_data).map_err(|err| err.to_string())?;
    port.flush().map_err(|err| err.to_string())?;

    let started = Instant::now();
    let mut last_probe_at = started;
    let mut probes_sent = 1u8;
    let mut read_buf = [0_u8; 512];
    let mut acc: Vec<u8> = Vec::with_capacity(1024);

    while started.elapsed() < timeout {
        if probes_sent < 4 && last_probe_at.elapsed() >= Duration::from_millis(220) {
            port.write_all(probe_data).map_err(|err| err.to_string())?;
            port.flush().map_err(|err| err.to_string())?;
            last_probe_at = Instant::now();
            probes_sent += 1;
        }

        match port.read(&mut read_buf) {
            Ok(bytes_read) if bytes_read > 0 => {
                acc.extend_from_slice(&read_buf[..bytes_read]);
                if has_valid_firmware_frame(&acc) {
                    return Ok(true);
                }
                if acc.len() > MAX_MESSAGE_SIZE {
                    let keep = MIN_MESSAGE_SIZE.saturating_sub(1);
                    let tail_start = acc.len().saturating_sub(keep);
                    acc.drain(0..tail_start);
                }
            }
            Ok(_) => {}
            Err(err) if err.kind() == std::io::ErrorKind::TimedOut => {}
            Err(_) => return Ok(false),
        }
    }

    Ok(false)
}

fn com_number(port_name: &str) -> Option<u32> {
    let upper = port_name.to_ascii_uppercase();
    if let Some(rest) = upper.strip_prefix("COM") {
        return rest.parse::<u32>().ok();
    }
    None
}

fn teardown_connection(connection: &mut Option<SerialConnection>) {
    if let Some(mut conn) = connection.take() {
        conn.stop_flag.store(true, Ordering::SeqCst);
        if let Some(handle) = conn.reader_thread.take() {
            let _ = handle.join();
        }
    }
}

#[tauri::command]
fn list_serial_ports() -> Result<Vec<SerialPortInfoDto>, String> {
    let ports = serialport::available_ports().map_err(|err| err.to_string())?;

    let out = ports
        .into_iter()
        .map(|p| SerialPortInfoDto {
            name: p.port_name,
            port_type: to_port_type_label(&p.port_type),
        })
        .collect::<Vec<_>>();

    Ok(out)
}

#[tauri::command]
async fn find_responsive_serial_port(
    probe_data: Vec<u8>,
    baud_rate: Option<u32>,
    timeout_ms: Option<u64>,
    scan_attempts: Option<u32>,
    scan_interval_ms: Option<u64>,
) -> Result<Option<SerialPortInfoDto>, String> {
    if probe_data.is_empty() {
        return Err("probe_data must not be empty".to_string());
    }

    let baud = baud_rate.unwrap_or(115_200);
    let timeout = Duration::from_millis(timeout_ms.unwrap_or(1200));
    let attempts = scan_attempts.unwrap_or(6).max(1);
    let interval = Duration::from_millis(scan_interval_ms.unwrap_or(220));

    for attempt in 0..attempts {
        let mut ports = serialport::available_ports().map_err(|err| err.to_string())?;
        ports.sort_by(|a, b| {
            match (com_number(&a.port_name), com_number(&b.port_name)) {
                (Some(an), Some(bn)) => bn.cmp(&an),
                _ => a.port_name.cmp(&b.port_name),
            }
        });

        for port in ports {
            let info = SerialPortInfoDto {
                name: port.port_name.clone(),
                port_type: to_port_type_label(&port.port_type),
            };

            match probe_port_once(&port.port_name, baud, &probe_data, timeout) {
                Ok(true) => return Ok(Some(info)),
                Ok(false) => {}
                Err(_) => {}
            }
        }

        if attempt + 1 < attempts {
            thread::sleep(interval);
        }
    }

    Ok(None)
}

#[tauri::command]
fn connect_serial(
    app: tauri::AppHandle,
    state: tauri::State<AppState>,
    port_name: String,
    baud_rate: Option<u32>,
) -> Result<(), String> {
    let mut guard = state.connection.lock().map_err(|_| "state lock poisoned")?;
    teardown_connection(&mut guard);

    let port = serialport::new(port_name, baud_rate.unwrap_or(115_200))
        .flow_control(serialport::FlowControl::None)
        .timeout(Duration::from_millis(50))
        .open()
        .map_err(|err| err.to_string())?;

    let mut port = port;
    let _ = port.write_data_terminal_ready(true);
    let _ = port.write_request_to_send(true);
    thread::sleep(Duration::from_millis(120));

    let mut reader_port = port.try_clone().map_err(|err| err.to_string())?;
    let _ = reader_port.write_data_terminal_ready(true);
    let _ = reader_port.write_request_to_send(true);
    let writer_port: Box<dyn SerialPort + Send> = port;

    let writer = Arc::new(Mutex::new(writer_port));
    let stop_flag = Arc::new(AtomicBool::new(false));

    let app_handle = app.clone();
    let read_stop_flag = stop_flag.clone();

    let reader_thread = thread::spawn(move || {
        let mut read_buf = [0_u8; 1024];

        while !read_stop_flag.load(Ordering::Relaxed) {
            match reader_port.read(&mut read_buf) {
                Ok(bytes_read) if bytes_read > 0 => {
                    let payload = read_buf[..bytes_read].to_vec();
                    let _ = app_handle.emit("serial-data", payload);
                }
                Ok(_) => {}
                Err(err) if err.kind() == std::io::ErrorKind::TimedOut => {}
                Err(_) => break,
            }
            thread::sleep(Duration::from_millis(30));
        }
        // always notify UI that the port is no longer usable
        let _ = app_handle.emit("serial-disconnected", ());
    });

    *guard = Some(SerialConnection {
        writer,
        stop_flag,
        reader_thread: Some(reader_thread),
    });

    Ok(())
}

#[tauri::command]
fn disconnect_serial(state: tauri::State<AppState>, app: tauri::AppHandle) -> Result<(), String> {
    let mut guard = state.connection.lock().map_err(|_| "state lock poisoned")?;
    teardown_connection(&mut guard);
    // notify frontend that connection is gone (useful for UI cleanup)
    let _ = app.emit("serial-disconnected", ());
    Ok(())
}

#[tauri::command]
fn write_serial(state: tauri::State<AppState>, data: Vec<u8>) -> Result<(), String> {
    let guard = state.connection.lock().map_err(|_| "state lock poisoned")?;
    let connection = guard.as_ref().ok_or("not connected")?;

    let mut writer = connection
        .writer
        .lock()
        .map_err(|_| "serial writer lock poisoned")?;

    writer.write_all(&data).map_err(|err| err.to_string())?;
    writer.flush().map_err(|err| err.to_string())?;
    Ok(())
}

#[tauri::command]
fn is_serial_connected(state: tauri::State<AppState>) -> Result<bool, String> {
    let guard = state.connection.lock().map_err(|_| "state lock poisoned")?;
    Ok(guard.is_some())
}

fn main() {
    tauri::Builder::default()
        .manage(AppState::default())
        .invoke_handler(tauri::generate_handler![
            list_serial_ports,
            find_responsive_serial_port,
            connect_serial,
            disconnect_serial,
            write_serial,
            is_serial_connected
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
