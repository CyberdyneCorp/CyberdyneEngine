//! The Model Context Protocol server, and the connection it answers over.
//!
//! --- WHAT THIS FILE IS ALLOWED TO DECIDE ------------------------------------------------------------
//!
//! Almost nothing. Every tool is [`cy_editor_agent::project`]'s output, every resource is
//! [`cy_editor_agent::AgentSession`]'s, every refusal is the `Problem` the registry produced, and
//! every mutation is the transaction the command opened. There is no list of tools in this file and
//! no place to add one: `tools/list` is a `map` over the projection, and a hand-written entry would
//! have to be inserted into that iterator, which is the shape of edit a reviewer notices.
//!
//! What it does decide is the **wire**: how a tool's typed parameters become a JSON Schema, how an
//! outcome becomes content, how an image becomes base64, and which JSON-RPC code a refusal carries.
//! That is the whole of the "engine-owned interface" the specification asks for — the seam is
//! [`cy_editor_agent::AgentTransport`], and [`Connection`] is the one implementation of it.
//!
//! --- WHY OBSERVATION IS A RESOURCE AND NOT A TOOL ---------------------------------------------------
//!
//! Because the tools are the registry, and a `viewport.observe` tool would be the hand-maintained
//! entry the requirement forbids — arrived at by the back door, and the more tempting for being
//! obviously useful. A viewport is something an agent *reads*, so it is a resource: `viewport:` for
//! the shipping frame, `viewport:overlays` for the editor's own image, `viewport:<view>` for a
//! buffer. The protocol's resource contents already carry a base64 `blob` with a media type, which
//! is exactly the shape an image needs, so nothing had to be invented for it either.
//!
//! --- WHY A REFUSAL IS A RESULT AND NOT AN ERROR -----------------------------------------------------
//!
//! For `tools/call` only, and because the protocol says so: a tool that fails returns `isError:
//! true` with the reason as content, so that the *model* sees it and can act, rather than the
//! transport swallowing it. A protocol-level failure — an unknown method, a malformed request — is a
//! JSON-RPC error, because no agent can act on it.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::io::{BufRead, Write};
use std::time::Instant;

use cy_editor_agent::observe::Observation;
use cy_editor_agent::session::{AgentSession, Confirmer, Reading};
use cy_editor_agent::tool::ToolDescriptor;
use cy_editor_agent::transport::{AgentRequest, AgentResponse, AgentTransport};
use cy_editor_commands::registry::Registry;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::ValueKind;
use cy_editor_services::editor::Editor;

use crate::json::{Json, base64};
use crate::rpc::{self, Incoming};

/// What this server calls itself when a client asks.
pub const SERVER_NAME: &str = "cyberdyne-editor";

/// One agent's connection, and the one implementation of the engine-owned transport interface.
///
/// Generic over the writer so that a test writes into a `Vec<u8>` and the binary writes into
/// standard output, with no branch between them.
pub struct Connection<W: Write> {
    writer: W,
    next: u64,
    pending: BTreeMap<u64, Json>,
    addressing: Json,
    connected: bool,
}

impl<W: Write> Connection<W> {
    /// A connection that writes its answers into `writer`.
    pub const fn new(writer: W) -> Self {
        Self {
            writer,
            next: 0,
            pending: BTreeMap::new(),
            addressing: Json::Null,
            connected: true,
        }
    }

    /// Note the wire identifier the next submitted request is answering.
    ///
    /// Separate from [`AgentTransport::submit`] because the trait carries no wire identifier — it
    /// cannot, without the protocol reaching through the seam — so the correlation is the
    /// transport's own business and is kept here.
    pub fn addressing(&mut self, id: Json) {
        self.addressing = id;
    }

    /// The peer went away.
    pub const fn closed(&mut self) {
        self.connected = false;
    }

    /// How many requests have been submitted and not yet answered.
    #[must_use]
    pub fn outstanding(&self) -> usize {
        self.pending.len()
    }

    /// Write one line, and refuse to keep going once the peer has gone.
    fn line(&mut self, text: &str) -> Result<()> {
        writeln!(self.writer, "{text}").map_err(|error| {
            self.connected = false;
            Problem::new("write to the agent", error.to_string())
                .with_remedy("the agent disconnected; nothing is lost, the editor keeps running")
        })?;
        self.writer.flush().map_err(|error| {
            self.connected = false;
            Problem::new("flush to the agent", error.to_string())
        })
    }
}

impl<W: Write> AgentTransport for Connection<W> {
    fn submit(&mut self, _request: AgentRequest) -> Result<u64> {
        self.next += 1;
        self.pending.insert(self.next, self.addressing.clone());
        Ok(self.next)
    }

    fn respond(&mut self, request: u64, response: AgentResponse) -> Result<()> {
        // Progress does not close a request: the protocol's progress facility is a notification, and
        // removing the entry here would leave the eventual result with nowhere to be addressed.
        if let AgentResponse::Progress {
            request: which,
            step,
            fraction,
        } = &response
        {
            let params = Json::object([
                ("progressToken", Json::Number(number(*which))),
                ("message", Json::text(step)),
                (
                    "progress",
                    fraction.map_or(Json::Null, |value| Json::Number(f64::from(value))),
                ),
            ]);
            return self.line(&rpc::notification("notifications/progress", params));
        }
        let id = self.pending.remove(&request).ok_or_else(|| {
            Problem::new(
                format!("answer request {request}"),
                "no request with that identity is outstanding",
            )
            .with_remedy("a request is answered once; a second answer is a defect in the caller")
        })?;
        let line = match render(&response) {
            Ok(result) => rpc::result(&id, result),
            Err(problem) => rpc::error(
                &id,
                rpc::INTERNAL_ERROR,
                &problem.because,
                problem.remedy.as_deref(),
            ),
        };
        self.line(&line)
    }

    fn cancel(&mut self, request: u64) -> Result<()> {
        self.pending.remove(&request);
        Ok(())
    }

    fn is_connected(&self) -> bool {
        self.connected
    }
}

/// The server: a connection, the agent's session, and the clock the budget is measured on.
pub struct McpServer<W: Write> {
    connection: Connection<W>,
    session: AgentSession,
    started: Instant,
    initialized: bool,
}

impl<W: Write> McpServer<W> {
    /// A server that answers `session`'s agent over `writer`.
    #[must_use]
    pub fn new(writer: W, session: AgentSession) -> Self {
        Self {
            connection: Connection::new(writer),
            session,
            started: Instant::now(),
            initialized: false,
        }
    }

    /// The connection, so that a human can see that an agent is attached.
    #[must_use]
    pub const fn connection(&self) -> &Connection<W> {
        &self.connection
    }

    /// The session, so that a human can see what it is doing, pause it, or revoke it.
    ///
    /// `editor-agent-interface`: "The human SHALL be able to see that an agent is connected, see
    /// what it is currently doing, pause it, and revoke its access mid-session, without restarting
    /// the editor or losing work." All four are methods on the session, and this is how the window
    /// reaches them.
    pub const fn session_mut(&mut self) -> &mut AgentSession {
        &mut self.session
    }

    /// The session, for reading.
    #[must_use]
    pub const fn session(&self) -> &AgentSession {
        &self.session
    }

    /// Milliseconds since the server started, which is what the budget's window is measured in.
    ///
    /// A monotonic reading rather than a wall clock: a budget that could be widened by changing the
    /// system time would not be one.
    fn now(&self) -> u64 {
        u64::try_from(self.started.elapsed().as_millis()).unwrap_or(u64::MAX)
    }

    /// Handle one line from the agent.
    ///
    /// Never fails for anything the agent did: a malformed message, an unknown method and a refused
    /// command all produce a reply. It returns `Err` only when the *connection* broke, which is what
    /// ends the loop.
    pub fn handle(
        &mut self,
        line: &str,
        editor: &mut Editor,
        registry: &Registry,
        confirmer: &mut dyn Confirmer,
    ) -> Result<()> {
        let message = match rpc::decode(line) {
            Ok(None) => return Ok(()),
            Ok(Some(message)) => message,
            Err(problem) => {
                // A malformed line has no identifier to answer, so the reply carries a null one,
                // which JSON-RPC provides for exactly this case.
                return self.connection.line(&rpc::error(
                    &Json::Null,
                    rpc::INVALID_PARAMS,
                    &problem.because,
                    problem.remedy.as_deref(),
                ));
            }
        };
        let Some(id) = message.id.clone() else {
            // A notification. Nothing to answer, and the only one that means anything is the
            // client's own "initialized", which needs no work on this side.
            return Ok(());
        };
        self.dispatch(&id, &message, editor, registry, confirmer)
    }

    fn dispatch(
        &mut self,
        id: &Json,
        message: &Incoming,
        editor: &mut Editor,
        registry: &Registry,
        confirmer: &mut dyn Confirmer,
    ) -> Result<()> {
        match message.method.as_str() {
            "initialize" => {
                self.initialized = true;
                let announced = Self::initialize_result();
                self.connection.line(&rpc::result(id, announced))
            }
            "ping" => self.connection.line(&rpc::result(id, Json::object([]))),
            "tools/list" | "tools/call" | "resources/list" | "resources/read" => {
                // The protocol's own rule, and worth enforcing rather than tolerating: a client
                // that starts calling tools without initialising has not agreed a protocol version,
                // so neither side knows what the other's messages mean. `ping` is exempt because it
                // carries nothing that could be misread.
                if !self.initialized {
                    return self.connection.line(&rpc::error(
                        id,
                        rpc::INVALID_PARAMS,
                        "this connection has not been initialised",
                        Some(
                            "send initialize first, and read the protocol version it answers with",
                        ),
                    ));
                }
                self.projected(id, message, editor, registry, confirmer)
            }
            other => self.connection.line(&rpc::error(
                id,
                rpc::METHOD_NOT_FOUND,
                &format!("this server has no method {other:?}"),
                Some(
                    "it speaks initialize, ping, tools/list, tools/call, resources/list and \
                     resources/read",
                ),
            )),
        }
    }

    /// The four methods that are a projection of the editor, routed through the seam.
    ///
    /// They go through [`AgentTransport::submit`] and [`AgentTransport::respond`] rather than being
    /// answered directly, so that the interface the specification requires is the one actually
    /// carrying traffic rather than a shape declared beside the code that ignores it.
    fn projected(
        &mut self,
        id: &Json,
        message: &Incoming,
        editor: &mut Editor,
        registry: &Registry,
        confirmer: &mut dyn Confirmer,
    ) -> Result<()> {
        let request = match Self::as_request(message, registry) {
            Ok(request) => request,
            Err(problem) => {
                return self.connection.line(&rpc::error(
                    id,
                    rpc::INVALID_PARAMS,
                    &problem.because,
                    problem.remedy.as_deref(),
                ));
            }
        };
        self.connection.addressing(id.clone());
        let ticket = self.connection.submit(request.clone())?;
        let response = self.answer(&request, editor, registry, confirmer);
        self.connection.respond(ticket, response)
    }

    /// One request, turned into what the projection understands.
    fn as_request(message: &Incoming, registry: &Registry) -> Result<AgentRequest> {
        match message.method.as_str() {
            "tools/list" => Ok(AgentRequest::ListTools),
            "resources/list" => Ok(AgentRequest::ListResources),
            "resources/read" => {
                let uri = message.params.get("uri").as_text().ok_or_else(|| {
                    Problem::new("read a resource", "no uri was given")
                        .with_remedy("list the resources to see the addresses that exist")
                })?;
                Ok(AgentRequest::ReadResource {
                    uri: uri.to_string(),
                })
            }
            _ => {
                let command = message.params.get("name").as_text().ok_or_else(|| {
                    Problem::new("call a tool", "no name was given")
                        .with_remedy("list the tools to see what there is")
                })?;
                let arguments = rendered(message.params.get("arguments"), command, registry)?;
                Ok(AgentRequest::Invoke {
                    command: command.to_string(),
                    arguments,
                })
            }
        }
    }

    /// Answer one request out of the editor.
    fn answer(
        &mut self,
        request: &AgentRequest,
        editor: &mut Editor,
        registry: &Registry,
        confirmer: &mut dyn Confirmer,
    ) -> AgentResponse {
        let now = self.now();
        match request {
            AgentRequest::ListTools => AgentResponse::Tools(self.session.tools(registry)),
            AgentRequest::ListResources => AgentResponse::Resources(
                self.session
                    .resources(editor)
                    .into_iter()
                    .map(|(uri, _, description)| (uri, description))
                    .collect(),
            ),
            AgentRequest::ReadResource { uri } => match self.session.read(editor, uri, now) {
                Ok(Reading::Text(resource)) => AgentResponse::Content(Box::new(resource)),
                Ok(Reading::Image(observation)) => AgentResponse::Image(observation),
                Err(problem) => refused(format!("read {uri}"), &problem),
            },
            AgentRequest::Observe(viewport) => match self.session.observe(editor, viewport, now) {
                Ok(observation) => AgentResponse::Image(Box::new(observation)),
                Err(problem) => refused("observe a viewport", &problem),
            },
            AgentRequest::Invoke { command, arguments } => {
                let built = cy_editor_agent::tool::arguments(registry, command, arguments);
                let outcome = built.and_then(|arguments| {
                    self.session
                        .invoke(editor, registry, confirmer, command, &arguments, now)
                });
                match outcome {
                    Ok(outcome) => AgentResponse::Outcome {
                        summary: outcome.summary,
                        values: outcome
                            .values
                            .into_iter()
                            .map(|(name, value)| (name, value.to_string()))
                            .collect(),
                    },
                    Err(problem) => refused(format!("invoke {command}"), &problem),
                }
            }
        }
    }

    /// What this server says it is and what it can do.
    fn initialize_result() -> Json {
        Json::object([
            ("protocolVersion", Json::text(rpc::PROTOCOL_VERSION)),
            (
                "capabilities",
                Json::object([
                    ("tools", Json::object([])),
                    // `subscribe` is absent deliberately: nothing here pushes a resource update, and
                    // declaring a capability the server does not have is how a client ends up
                    // waiting for a notification that never comes.
                    ("resources", Json::object([])),
                ]),
            ),
            (
                "serverInfo",
                Json::object([
                    ("name", Json::text(SERVER_NAME)),
                    ("version", Json::text(env!("CARGO_PKG_VERSION"))),
                ]),
            ),
            (
                "instructions",
                Json::text(
                    "Every tool is one of the editor's own commands and does exactly what the same \
                     command does from its menu. Read resources/list first: hierarchy: and node: \
                     describe the scene, sources: the project's scripts, history: who changed what \
                     and why, budget: what this connection may spend, and viewport: the engine's \
                     own rendered image — look at it after an edit rather than assuming. Every \
                     mutation is one undoable transaction attributed to this session.",
                ),
            ),
        ])
    }
}

/// A refusal, in the shape `editor-agent-interface` requires: what, why, and what would help.
fn refused(what: impl Into<String>, problem: &Problem) -> AgentResponse {
    AgentResponse::Refused {
        what: what.into(),
        because: problem.because.clone(),
        remedy: problem.remedy.clone(),
    }
}

/// The `arguments` object of a `tools/call`, rendered for the seam.
///
/// Every value becomes text, because that is what [`AgentRequest::Invoke`] carries and its own
/// comment says why. The *kind* is looked up from the command's declared parameters so that a JSON
/// number destined for a `Vec3` is rendered in the form the projection reads back — a caller may
/// send `[1,2,3]` or `"(1, 2, 3)"` and both arrive as the same value.
fn rendered(arguments: &Json, command: &str, registry: &Registry) -> Result<Vec<(String, String)>> {
    let Some(members) = arguments.as_object() else {
        return Ok(Vec::new());
    };
    let metadata = registry.metadata(command).ok_or_else(|| {
        Problem::not_found(format!("a command named {command:?}"))
            .with_remedy("list the tools to see what there is")
    })?;
    let mut supplied = Vec::new();
    for (name, value) in members {
        let kind = metadata
            .parameters
            .iter()
            .find(|parameter| parameter.name == *name)
            .map(|parameter| parameter.kind);
        supplied.push((name.clone(), flatten(value, kind)));
    }
    Ok(supplied)
}

/// One JSON value as the text the projection reads back.
fn flatten(value: &Json, kind: Option<ValueKind>) -> String {
    match value {
        Json::Text(text) => text.clone(),
        Json::Bool(flag) => flag.to_string(),
        Json::Number(value) => {
            if value.fract() == 0.0 && matches!(kind, Some(ValueKind::Int | ValueKind::Entity)) {
                whole(*value)
            } else {
                value.to_string()
            }
        }
        Json::Array(items) => items
            .iter()
            .map(|item| flatten(item, None))
            .collect::<Vec<_>>()
            .join(", "),
        Json::Null => String::new(),
        Json::Object(_) => value.render(),
    }
}

/// One response, as the protocol's own result shape.
fn render(response: &AgentResponse) -> Result<Json> {
    Ok(match response {
        AgentResponse::Tools(tools) => {
            Json::object([("tools", Json::Array(tools.iter().map(tool_entry).collect()))])
        }
        AgentResponse::Resources(resources) => Json::object([(
            "resources",
            Json::Array(
                resources
                    .iter()
                    .map(|(uri, description)| {
                        Json::object([
                            ("uri", Json::text(uri.clone())),
                            ("name", Json::text(uri.clone())),
                            ("description", Json::text(description.clone())),
                            ("mimeType", Json::text("text/plain")),
                        ])
                    })
                    .collect(),
            ),
        )]),
        AgentResponse::Content(resource) => Json::object([(
            "contents",
            Json::Array(vec![Json::object([
                ("uri", Json::text(resource.uri.clone())),
                ("mimeType", Json::text("text/plain")),
                ("text", Json::text(resource.content.clone())),
            ])]),
        )]),
        AgentResponse::Image(observation) => image_result(observation),
        AgentResponse::Outcome { summary, values } => {
            let mut lines = summary.clone();
            for (name, value) in values {
                let _ = write!(lines, "\n{name} = {value}");
            }
            Json::object([
                ("content", Json::Array(vec![text_content(&lines)])),
                ("isError", Json::Bool(false)),
                // The structured half beside the prose one, because an agent that has to parse a
                // sentence to learn which entity was created is an agent that will get it wrong.
                (
                    "structuredContent",
                    Json::Object(
                        values
                            .iter()
                            .map(|(name, value)| (name.clone(), Json::text(value.clone())))
                            .collect(),
                    ),
                ),
            ])
        }
        AgentResponse::Refused {
            what,
            because,
            remedy,
        } => {
            let mut lines = format!("Could not {what}: {because}");
            if let Some(remedy) = remedy {
                let _ = write!(lines, "\nWhat would help: {remedy}");
            }
            Json::object([
                ("content", Json::Array(vec![text_content(&lines)])),
                ("isError", Json::Bool(true)),
            ])
        }
        AgentResponse::Progress { .. } => {
            return Err(Problem::new(
                "render a response",
                "progress is a notification and has no result shape",
            ));
        }
    })
}

/// A resource read that answered with an image.
///
/// A `blob` when the delivery actually carried bytes, and text saying what it is when it did not:
/// a shared texture cost no copy to deliver, and an empty blob would be the substituted
/// representation `editor-agent-interface` forbids. Either way the reply **states what the image
/// is**, which is the honesty the requirement asks for in as many words.
fn image_result(observation: &Observation) -> Json {
    let description = observation.describe();
    let entry = match observation.bytes() {
        Some(bytes) => Json::object([
            ("uri", Json::text(uri_of(observation))),
            ("mimeType", Json::text(observation.media_type())),
            ("blob", Json::text(base64(bytes))),
        ]),
        None => Json::object([
            ("uri", Json::text(uri_of(observation))),
            ("mimeType", Json::text("text/plain")),
            (
                "text",
                Json::text(format!(
                    "{description}\n\nThe image is on the graphics device and was delivered \
                     without a copy, so there are no bytes to carry here. Ask the runtime for an \
                     encoded frame if the pixels are what you need.",
                )),
            ),
        ]),
    };
    Json::object([("contents", Json::Array(vec![entry]))])
}

/// The address an observation came back from.
fn uri_of(observation: &Observation) -> String {
    match observation.kind {
        cy_editor_agent::observe::ObservationKind::ShippingFrame => "viewport:".to_string(),
        cy_editor_agent::observe::ObservationKind::EditorFrame => "viewport:overlays".to_string(),
        cy_editor_agent::observe::ObservationKind::DebugView(mode) => {
            format!("viewport:{}", mode.engine_name())
        }
    }
}

/// A count as a JSON number.
///
/// Every identifier this server allocates is a small counter, so the precision an f64 loses above
/// 2^53 is unreachable — and JSON has no other numeric type to put one in.
#[allow(
    clippy::cast_precision_loss,
    reason = "the values are request counters, far below the 2^53 an f64 holds exactly"
)]
fn number(value: u64) -> f64 {
    value as f64
}

/// An integral JSON number, rendered without a decimal point.
#[allow(
    clippy::cast_possible_truncation,
    reason = "the caller has already checked that the value is integral, and a value outside i64 \
              renders through the same formatting either way"
)]
fn whole(value: f64) -> String {
    format!("{}", value as i64)
}

fn text_content(text: &str) -> Json {
    Json::object([("type", Json::text("text")), ("text", Json::text(text))])
}

/// One tool, with its parameters as a JSON Schema.
///
/// Generated from the command's own metadata — the description, the parameter meanings, the effect
/// class and the exclusion all come from there and none of them is written here.
fn tool_entry(tool: &ToolDescriptor) -> Json {
    let mut properties = BTreeMap::new();
    let mut required = Vec::new();
    for parameter in &tool.parameters {
        properties.insert(
            parameter.name.clone(),
            Json::object([
                ("type", Json::text(schema_type(parameter.kind))),
                ("description", Json::text(parameter.description.clone())),
            ]),
        );
        if parameter.required {
            required.push(Json::text(parameter.name.clone()));
        }
    }

    // The effect class and the exclusion are put in the DESCRIPTION rather than only in an
    // annotation, because a model reads the description and may never see a field the protocol
    // added last year. "WHEN an agent lists the available tools THEN each SHALL state its effect
    // class" is satisfied where the agent will actually read it.
    let mut description = tool.description.clone();
    let _ = write!(
        description,
        "\n\nEffect: {} ({}){}.",
        tool.effect.name(),
        tool.effect.consequence(),
        if tool.effect_may_narrow {
            ", at most — this command works its class out from the invocation and may be narrower"
        } else {
            ""
        }
    );
    if let Some(reason) = &tool.exclusion {
        let _ = write!(
            description,
            "\n\nNOT AVAILABLE to an agent: {reason}. Invoking it will be refused."
        );
    }

    Json::object([
        ("name", Json::text(tool.name.clone())),
        ("title", Json::text(tool.title.clone())),
        ("description", Json::text(description)),
        (
            "inputSchema",
            Json::object([
                ("type", Json::text("object")),
                ("properties", Json::Object(properties)),
                ("required", Json::Array(required)),
            ]),
        ),
        (
            "annotations",
            Json::object([
                ("readOnlyHint", Json::Bool(tool.effect.name() == "read")),
                ("destructiveHint", Json::Bool(tool.needs_confirmation)),
            ]),
        ),
    ])
}

/// The JSON Schema type a parameter's kind maps onto.
///
/// A vector is an array of numbers on the wire and a `(x, y, z)` string in the projection; both
/// arrive at the same value, because `crate::server::flatten` renders an array into the form
/// `cy_editor_agent::coerce` reads.
const fn schema_type(kind: ValueKind) -> &'static str {
    match kind {
        ValueKind::Bool => "boolean",
        ValueKind::Int | ValueKind::Entity => "integer",
        ValueKind::Float | ValueKind::Double => "number",
        ValueKind::Vec2 | ValueKind::Vec3 | ValueKind::Vec4 | ValueKind::Quat => "array",
        ValueKind::Nil | ValueKind::Text | ValueKind::Bytes => "string",
    }
}

/// Read requests until the agent goes away.
///
/// One request at a time, on the caller's thread, because the caller owns the editor: everything the
/// server does is bounded and non-blocking — a build is queued rather than run — so serialising the
/// connection costs nothing and removes the whole question of what two agent requests do to one
/// document at once.
///
/// # Errors
///
/// When reading fails. A malformed message is not a failure: it is answered and the loop continues.
pub fn serve<R: BufRead, W: Write>(
    reader: R,
    server: &mut McpServer<W>,
    editor: &mut Editor,
    registry: &Registry,
    confirmer: &mut dyn Confirmer,
) -> Result<()> {
    for line in reader.lines() {
        let line = line.map_err(|error| {
            Problem::new("read from the agent", error.to_string())
                .with_remedy("the agent disconnected; the editor keeps running")
        })?;
        server.handle(&line, editor, registry, confirmer)?;
        // One frame of the editor's own housekeeping between requests: what the runtime sent is
        // drained and settled operations are forgotten, so an agent polling `operations:` sees a
        // build finish without a window having to be open.
        editor.pump();
        if server.session().is_revoked() {
            break;
        }
    }
    server.connection.closed();
    Ok(())
}
