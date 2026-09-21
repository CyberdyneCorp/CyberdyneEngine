// SPDX-License-Identifier: MIT
//! Opt-in proof against the installed Swift toolchain.

#![forbid(unsafe_code)]

use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use cy_editor_sourcekit::{ClientOptions, SourceKitClient};

struct SwiftPackage(PathBuf);

impl SwiftPackage {
    fn create() -> Self {
        let root = std::env::temp_dir().join(format!(
            "cy-sourcekit-live-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(root.join("Sources/Game")).unwrap();
        std::fs::write(
            root.join("Package.swift"),
            "// swift-tools-version: 6.0\nimport PackageDescription\nlet package = Package(name: \"Game\", targets: [.executableTarget(name: \"Game\")])\n",
        )
        .unwrap();
        std::fs::write(
            root.join("Sources/Game/main.swift"),
            "let value: Int = 1\nprint(value)\n",
        )
        .unwrap();
        Self(root)
    }
}

impl Drop for SwiftPackage {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn file_uri(path: &Path) -> String {
    format!("file://{}", path.to_string_lossy())
}

fn tool_exists(tool: &str) -> bool {
    Command::new(tool)
        .arg("--help")
        .output()
        .is_ok_and(|output| output.status.success())
}

#[test]
#[ignore = "requires CY_RUN_SOURCEKIT_LIVE=1 and an installed Swift toolchain"]
fn installed_sourcekit_reports_a_temporary_package_diagnostic() {
    assert_eq!(std::env::var("CY_RUN_SOURCEKIT_LIVE").as_deref(), Ok("1"));
    assert!(
        tool_exists("sourcekit-lsp"),
        "sourcekit-lsp is not installed"
    );
    assert!(tool_exists("swift"), "swift is not installed");

    let package = SwiftPackage::create();
    let mut options = ClientOptions::new(file_uri(&package.0));
    options.request_timeout = Duration::from_secs(30);
    let mut client = SourceKitClient::launch(options).expect("initialize installed sourcekit-lsp");
    let source = package.0.join("Sources/Game/main.swift");
    let uri = file_uri(&source);
    client
        .open_document(&uri, "let value: Int = \"wrong\"\nprint(value)\n", 1)
        .expect("open temporary Swift source");

    let deadline = Instant::now() + Duration::from_secs(30);
    while Instant::now() < deadline {
        let _ = client.poll(Duration::from_millis(250));
        if client
            .diagnostics(&uri)
            .is_some_and(|diagnostics| !diagnostics.items.is_empty())
        {
            client.shutdown().expect("shutdown installed sourcekit-lsp");
            return;
        }
    }
    panic!("installed sourcekit-lsp published no diagnostic within 30 seconds");
}
