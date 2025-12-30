use std::ffi::CString;
use std::fs::File;
use std::os::unix::io::FromRawFd;
use std::thread;
use std::time::Duration;

use memmap2::MmapOptions;

use daw_bridge::layout::ShmHeader;
use daw_bridge::reader::SeqlockReader;

fn default_shm_name() -> String {
    if let Ok(name) = std::env::var("DAW_UI_SHM_NAME") {
        if name.starts_with('/') {
            return name;
        }
        return format!("/{}", name);
    }
    if let Ok(name) = std::env::var("DAW_SHM_NAME") {
        if name.starts_with('/') {
            return name;
        }
        return format!("/{}", name);
    }
    "/daw_engine_ui".to_string()
}

fn main() {
    let name = default_shm_name();
    let c_name = CString::new(name.clone()).unwrap_or_else(|_| {
        eprintln!("Invalid SHM name: {:?}", name);
        std::process::exit(1);
    });

    let fd = unsafe { libc::shm_open(c_name.as_ptr(), libc::O_RDONLY, 0) };
    if fd < 0 {
        eprintln!("Failed to open SHM {}: {}", name, std::io::Error::last_os_error());
        std::process::exit(1);
    }
    let file = unsafe { File::from_raw_fd(fd) };

    let mmap = unsafe {
        MmapOptions::new()
            .map(&file)
            .unwrap_or_else(|err| {
                eprintln!("Failed to map SHM: {}", err);
                std::process::exit(1);
            })
    };

    let header = mmap.as_ptr() as *const ShmHeader;
    let reader = SeqlockReader::new(header);

    loop {
        if let Some(snapshot) = reader.read_snapshot() {
            println!(
                "uiVersion={} playhead={} rms0={}",
                snapshot.version,
                snapshot.ui_global_nanotick_playhead,
                snapshot.ui_track_peak_rms[0]
            );
        }
        thread::sleep(Duration::from_millis(16));
    }
}
