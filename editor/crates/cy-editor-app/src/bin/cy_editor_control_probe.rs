//! Process-level probe for the editor control socket.
//!
//! It sends the same resize-aware gizmo intent and frame-addressed pick as the window, then checks
//! that the C++ runtime answers both. This deliberately talks to a separately launched runtime: an
//! in-process test cannot prove the Darwin Unix-socket bridge or its reconnect behaviour.

#[cfg(unix)]
#[allow(clippy::too_many_lines)] // Linear CLI protocol script; splitting obscures request ordering.
fn main() {
    use std::time::Duration;

    use cy_editor_protocol::{FrameId, Message, Session, SessionEvent};
    use cy_editor_services::gizmo::Request as GizmoRequest;
    use cy_editor_viewport::gizmo::{GizmoMode, GizmoSpace, Pivot};
    use cy_editor_viewport::viewport::ViewportId;
    use cy_editor_viewport::{PickIntent, PickRequest, PickResponse};

    let arguments: Vec<String> = std::env::args().collect();
    let value = |key: &str| {
        arguments
            .iter()
            .position(|argument| argument == key)
            .and_then(|index| arguments.get(index + 1))
    };
    let endpoint = value("--host").map_or("/tmp/cy-editor-host.sock", String::as_str);
    let width = value("--width")
        .and_then(|text| text.parse::<u32>().ok())
        .unwrap_or(721);
    let height = value("--height")
        .and_then(|text| text.parse::<u32>().ok())
        .unwrap_or(413);

    let session = Session::connect_unix(endpoint).unwrap_or_else(|problem| {
        eprintln!("[control-probe] {problem}");
        std::process::exit(2);
    });
    session
        .send(&Message::Hello {
            abi_major: cy_editor_sdk::abi::MAJOR,
            abi_minor: cy_editor_sdk::abi::MINOR,
            editor: env!("CARGO_PKG_VERSION").to_owned(),
        })
        .expect("send the editor hello");
    let welcome = session.block_until(Duration::from_secs(3), |event| match event {
        SessionEvent::Message(Message::Welcome { runtime, .. }) => Some(runtime.clone()),
        _ => None,
    });
    let Some(runtime) = welcome else {
        eprintln!("[control-probe] runtime did not welcome the editor");
        std::process::exit(1);
    };

    let gizmo_request = session.next_request();
    let gizmo = GizmoRequest {
        frame: FrameId::from_raw(0),
        mode: GizmoMode::Translate,
        space: GizmoSpace::World,
        pivot: Pivot::Pivot,
        identities: Vec::new(),
        width,
        height,
        camera_position: [0.0, 2.0, 6.0],
        camera_rotation: [0.0, 0.0, 0.0, 1.0],
        fov_y_radians: std::f32::consts::FRAC_PI_3,
        near: 0.1,
    };
    session
        .send(&Message::GizmoIntent {
            request: gizmo_request,
            viewport: 1,
            intent: gizmo.encode(),
        })
        .expect("send the resize-aware gizmo intent");
    let geometry = session.block_until(Duration::from_secs(3), |event| match event {
        SessionEvent::Message(Message::GizmoGeometry { request, layout })
            if *request == gizmo_request =>
        {
            Some(layout.clone())
        }
        _ => None,
    });
    if geometry.is_none() {
        eprintln!("[control-probe] runtime did not answer the {width}x{height} viewport intent");
        std::process::exit(1);
    }

    let points = [
        (0.50, 0.50),
        (0.35, 0.50),
        (0.65, 0.50),
        (0.50, 0.35),
        (0.50, 0.65),
        (0.35, 0.35),
        (0.65, 0.35),
        (0.35, 0.65),
        (0.65, 0.65),
    ];
    let mut identity = None;
    let mut answered_count = 0_u32;
    let width_pixels = f32::from(u16::try_from(width).unwrap_or(u16::MAX));
    let height_pixels = f32::from(u16::try_from(height).unwrap_or(u16::MAX));
    for (x, y) in points {
        let pick = PickRequest::for_frame(
            ViewportId::from_raw(1),
            FrameId::from_raw(0),
            PickIntent::Click {
                x: x * width_pixels,
                y: y * height_pixels,
            },
        );
        let request = session
            .pick(FrameId::from_raw(0), pick.encode())
            .expect("send a frame-addressed pick");
        let response = session.block_until(Duration::from_secs(3), |event| match event {
            SessionEvent::Message(Message::Picked {
                request: answered,
                candidates,
            }) if *answered == request => PickResponse::decode(candidates).ok(),
            _ => None,
        });
        let Some(response) = response else {
            eprintln!(
                "[control-probe] runtime did not answer pick request {}",
                request.as_u64()
            );
            std::process::exit(1);
        };
        answered_count += 1;
        if let Some(candidate) = response.candidates.first() {
            identity = Some(candidate.identity);
            break;
        }
    }

    println!(
        "[control-probe] runtime={runtime}; viewport={width}x{height}; picks_answered={answered_count}; stable_identity={}",
        identity.map_or_else(|| "none (sky)".to_owned(), |value| value.to_string())
    );
}

#[cfg(not(unix))]
fn main() {
    eprintln!("cy-editor-control-probe requires a Unix domain socket");
    std::process::exit(2);
}
