//! JSON-RPC 2.0, in the shape the Model Context Protocol's stdio transport uses.
//!
//! One document per line, no `Content-Length` header — that is what the stdio transport specifies,
//! and it is why [`crate::json::parse`] refuses a line carrying two documents rather than reading
//! the first: a desynchronised stream produces failures that name something else entirely.
//!
//! # Notifications are not requests
//!
//! A message with no `id` is a notification and **must not be answered**. Replying to one is the
//! commonest JSON-RPC defect and the symptom at the far end is a response that matches no request,
//! which most clients log and ignore — so it is invisible until something stricter arrives.
//! [`Incoming::id`] is `None` for a notification and [`is_notification`] is what every path checks.

use crate::json::Json;

/// The protocol version this build speaks.
///
/// Stated as a constant rather than echoed from the client's `initialize`, because echoing is how a
/// server ends up claiming to speak a version it does not: the client asks for next year's, the
/// server agrees, and the first message with a new field in it fails somewhere unrelated.
pub const PROTOCOL_VERSION: &str = "2025-06-18";

/// One decoded message.
#[derive(Clone, PartialEq, Debug)]
pub struct Incoming {
    /// The request identifier, or `None` for a notification.
    pub id: Option<Json>,
    /// Which method.
    pub method: String,
    /// Its parameters, or `Json::Null`.
    pub params: Json,
}

impl Incoming {
    /// Whether this must go unanswered.
    #[must_use]
    pub const fn is_notification(&self) -> bool {
        self.id.is_none()
    }
}

/// Read one line as a JSON-RPC message.
///
/// Returns `Ok(None)` for a blank line, which a well-behaved peer does not send and a shell
/// pipeline produces constantly.
///
/// # Errors
///
/// When the line is not JSON, or is JSON that is not a request.
pub fn decode(line: &str) -> cy_editor_core::problem::Result<Option<Incoming>> {
    if line.trim().is_empty() {
        return Ok(None);
    }
    let value = crate::json::parse(line)?;
    let method = value.get("method").as_text().ok_or_else(|| {
        cy_editor_core::problem::Problem::new("read a JSON-RPC message", "it has no method")
            .with_remedy("send {\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}")
    })?;
    let id = value.get("id");
    Ok(Some(Incoming {
        id: if id.is_null() { None } else { Some(id.clone()) },
        method: method.to_string(),
        params: value.get("params").clone(),
    }))
}

/// A successful response.
#[must_use]
pub fn result(id: &Json, result: Json) -> String {
    Json::object([
        ("jsonrpc", Json::text("2.0")),
        ("id", id.clone()),
        ("result", result),
    ])
    .render()
}

/// A failed response.
///
/// The message is the whole `Problem` rather than a code, because `editor-agent-interface` requires
/// that "an error SHALL state what went wrong and what would make it succeed", and a numeric code
/// states neither. The code is present because the protocol requires one; the sentence is what an
/// agent acts on, and the remedy is carried separately in `data` so a caller can show it apart from
/// the failure.
#[must_use]
#[allow(
    clippy::cast_precision_loss,
    reason = "JSON-RPC's own codes are five-digit negative integers; f64 represents every one of \
              them exactly, and JSON has no other numeric type to put them in"
)]
pub fn error(id: &Json, code: i64, because: &str, remedy: Option<&str>) -> String {
    let mut data = std::collections::BTreeMap::new();
    if let Some(remedy) = remedy {
        data.insert("remedy".to_string(), Json::text(remedy));
    }
    let mut failure = std::collections::BTreeMap::new();
    failure.insert("code".to_string(), Json::Number(code as f64));
    failure.insert("message".to_string(), Json::text(because));
    if !data.is_empty() {
        failure.insert("data".to_string(), Json::Object(data));
    }
    Json::object([
        ("jsonrpc", Json::text("2.0")),
        ("id", id.clone()),
        ("error", Json::Object(failure)),
    ])
    .render()
}

/// A notification this server sends, which carries no identifier and expects no answer.
#[must_use]
pub fn notification(method: &str, params: Json) -> String {
    Json::object([
        ("jsonrpc", Json::text("2.0")),
        ("method", Json::text(method)),
        ("params", params),
    ])
    .render()
}

/// JSON-RPC's own code for a method the server does not have.
pub const METHOD_NOT_FOUND: i64 = -32_601;

/// JSON-RPC's own code for arguments the server cannot use.
pub const INVALID_PARAMS: i64 = -32_602;

/// JSON-RPC's own code for anything else that went wrong on this side.
pub const INTERNAL_ERROR: i64 = -32_603;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_request_and_a_notification_are_told_apart() {
        // Answering a notification is the commonest JSON-RPC defect, and its symptom at the far end
        // is a response matching no request — which most clients log and ignore.
        let request = decode(r#"{"jsonrpc":"2.0","id":1,"method":"tools/list"}"#)
            .unwrap()
            .unwrap();
        assert!(!request.is_notification());
        assert_eq!(request.method, "tools/list");

        let note = decode(r#"{"jsonrpc":"2.0","method":"notifications/initialized"}"#)
            .unwrap()
            .unwrap();
        assert!(note.is_notification());
    }

    #[test]
    fn a_blank_line_is_not_a_message() {
        assert!(decode("").unwrap().is_none());
        assert!(decode("   \n").unwrap().is_none());
    }

    #[test]
    fn a_line_that_is_not_a_request_is_refused_with_what_one_looks_like() {
        let refused = decode(r#"{"jsonrpc":"2.0","id":1}"#).unwrap_err();
        assert!(refused.remedy.as_deref().unwrap().contains("tools/list"));
    }

    #[test]
    fn a_failure_carries_the_sentence_and_the_remedy_separately() {
        let rendered = error(
            &Json::Number(4.0),
            INVALID_PARAMS,
            "no document is open",
            Some("open a document first"),
        );
        let decoded = crate::json::parse(&rendered).unwrap();
        assert_eq!(decoded.get("id"), &Json::Number(4.0));
        assert_eq!(
            decoded.get("error").get("message").as_text().unwrap(),
            "no document is open"
        );
        assert_eq!(
            decoded
                .get("error")
                .get("data")
                .get("remedy")
                .as_text()
                .unwrap(),
            "open a document first"
        );
    }

    #[test]
    fn an_identifier_comes_back_exactly_as_it_arrived() {
        // A client may use a string identifier, and one that got a number back would be entitled to
        // say the reply matched nothing it sent.
        for id in [Json::Number(7.0), Json::text("abc")] {
            let rendered = result(&id, Json::object([]));
            assert_eq!(crate::json::parse(&rendered).unwrap().get("id"), &id);
        }
    }
}
