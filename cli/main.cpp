#include "pm/commons/commons.hpp"

namespace media {
bool is_http_url(const std::string& s) {
    return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
}
}

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace {

int status_for(const std::string& code) {
    if (code == "EFORBIDDEN") return 403;
    if (code == "ENOENT") return 404;
    if (code == "EIO") return 500;
    return 400;
}

void emit(const nlohmann::json& body, bool ok) {
    std::cout << body.dump() << "\n";
    std::exit(ok ? 0 : 1);
}

void emit_ok(nlohmann::json body) {
    body["ok"] = true;
    emit(body, true);
}

void emit_err(const pm::commons::Error& err) {
    nlohmann::json body;
    body["ok"] = false;
    body["error"] = err.message;
    body["code"] = err.code.empty() ? "EFAIL" : err.code;
    body["status"] = status_for(body["code"]);
    emit(body, false);
}

void emit_err(std::string code, std::string message) {
    emit_err(pm::commons::Error::make(std::move(code), std::move(message)));
}

void emit_path_err(const std::string& err) {
    const auto colon = err.find(':');
    if (colon != std::string::npos && colon > 0 && colon < 24)
        emit_err(err.substr(0, colon), err);
    emit_err("EFORBIDDEN", err);
}

struct Args {
    std::string text;
    std::string hay;
    std::string query;
    std::string pattern;
    std::string rel;
    std::string dir;
    std::string kind;
    std::string id;
    std::string type;
    std::string payload;
    std::string name;
    std::vector<std::string> sets;
    std::vector<std::string> args;
    std::vector<std::string> select;
    std::vector<std::string> paths;
    std::vector<std::string> exts;
    bool force = false;
    bool hex = false;
    std::string replacement;
    bool lenient = false;
    bool reject_userinfo = false;
    bool is_dir = false;
    bool fold = false;
    bool file = false;
    bool alive = false;
    int count = 1;
    int priority = 0;
    std::uint32_t high = 0;
    std::uint32_t low = 0;
    int self_priority = 0;
    std::uint32_t seed = 1;
    std::int64_t pid = 0;
    std::int64_t self_pid = 0;
    std::int64_t now = 0;
};

bool alive_hook(std::int64_t, void*) { return true; }

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool take_text(const Args& opt, std::string& out) {
    if (!opt.hex) {
        out = opt.text;
        return true;
    }
    if (opt.text.size() % 2 != 0)
        return false;
    out.clear();
    out.reserve(opt.text.size() / 2);
    for (std::size_t i = 0; i < opt.text.size(); i += 2) {
        const int hi = hex_nibble(opt.text[i]);
        const int lo = hex_nibble(opt.text[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"pm-commons"};
    app.require_subcommand(1);
    Args opt;

    auto* str = app.add_subcommand("string", "UTF-8 string primitives");
    str->require_subcommand(1);
    auto* lower = str->add_subcommand("lower", "ASCII lower-case");
    lower->add_option("text", opt.text)->required();
    lower->callback([&] { emit_ok({{"value", pm::commons::str::lower_ascii(opt.text)}}); });

    auto* trim = str->add_subcommand("trim", "Trim ASCII whitespace");
    trim->add_option("text", opt.text)->required();
    trim->callback([&] { emit_ok({{"value", pm::commons::str::trim(opt.text)}}); });

    auto* leaf = str->add_subcommand("leaf", "Last path component");
    leaf->add_option("text", opt.text)->required();
    leaf->callback([&] { emit_ok({{"value", pm::commons::str::path_leaf(opt.text)}}); });

    auto* ext = str->add_subcommand("ext", "Lower-case extension");
    ext->add_option("text", opt.text)->required();
    ext->callback([&] { emit_ok({{"value", pm::commons::str::path_ext_lower(opt.text)}}); });

    auto* match = str->add_subcommand("match", "Fuzzy query coverage");
    match->add_option("--hay", opt.hay)->required();
    match->add_option("--query", opt.query)->required();
    match->callback([&] {
        emit_ok({
            {"score", pm::commons::str::match_score(opt.hay, opt.query)},
            {"matched", pm::commons::str::matches_query(opt.hay, opt.query)},
        });
    });

    auto* subst = str->add_subcommand("subst", "Substitute ${name} tokens");
    subst->add_option("text", opt.text)->required();
    subst->add_option("--set", opt.sets);
    subst->callback([&] {
        const auto resolved = pm::commons::str::substitute_variables(opt.text, [&](std::string_view key) {
            for (const auto& item : opt.sets) {
                const auto eq = item.find('=');
                if (eq == std::string::npos)
                    continue;
                if (item.substr(0, eq) == key)
                    return std::optional<std::string>(item.substr(eq + 1));
            }
            return std::optional<std::string>{};
        });
        emit_ok({{"value", resolved}});
    });

    auto* args = str->add_subcommand("args", "Expand ${CURRENT_SELECTION}");
    args->add_option("--arg", opt.args);
    args->add_option("--select", opt.select);
    args->callback([&] {
        emit_ok({{"args", pm::commons::str::normalize_command_args(opt.args, opt.select)}});
    });

    auto* uuid = app.add_subcommand("uuid", "64-bit and 32-bit identifiers");
    uuid->require_subcommand(1);
    auto* next = uuid->add_subcommand("next", "Sequential values");
    next->add_option("--seed", opt.seed);
    next->add_option("--count", opt.count);
    next->callback([&] {
        pm::commons::uuid::Generator gen;
        gen.reset_sequential(opt.seed);
        nlohmann::json values = nlohmann::json::array();
        for (int i = 0; i < opt.count; ++i)
            values.push_back(gen.sequential());
        emit_ok({{"values", values}});
    });
    auto* parse_uuid = uuid->add_subcommand("parse", "Parse a hex or standard UUID");
    parse_uuid->add_option("text", opt.text)->required();
    parse_uuid->callback([&] {
        if (opt.text.find('-') != std::string::npos) {
            const auto id = pm::commons::uuid::Uuid64::from_standard_string(opt.text, true);
            emit_ok({{"high", id.high}, {"low", id.low}});
        }
        emit_ok({{"value", pm::commons::uuid::Generator::from_hex(opt.text)}});
    });
    auto* format = uuid->add_subcommand("format", "Format a Uuid64");
    format->add_option("--high", opt.high)->required();
    format->add_option("--low", opt.low)->required();
    format->callback([&] {
        pm::commons::uuid::Uuid64 id(opt.high, opt.low);
        emit_ok({{"text", id.to_standard_string()}});
    });

    auto* path = app.add_subcommand("path", "Path policy");
    path->require_subcommand(1);
    auto add_text = [&](CLI::App* cmd) {
        cmd->add_option("text", opt.text)->required();
        cmd->add_flag("--hex", opt.hex, "Text is hex-encoded UTF-8");
    };
    auto* sanitize = path->add_subcommand("sanitize", "VFS subpath");
    add_text(sanitize);
    sanitize->callback([&] {
        std::string text;
        if (!take_text(opt, text))
            emit_err("EINVAL", "invalid hex");
        std::string err;
        const std::string value = media::path::sanitize_subpath(text, err);
        if (!err.empty())
            emit_path_err(err);
        emit_ok({{"value", value}});
    });
    auto* write_path = path->add_subcommand("write", "Write-path components");
    add_text(write_path);
    write_path->callback([&] {
        std::string text;
        if (!take_text(opt, text))
            emit_err("EINVAL", "invalid hex");
        std::string err;
        emit_ok({{"value", media::path::sanitize_write_path(text, err, false)}});
    });
    auto* filename = path->add_subcommand("filename", "One filesystem component");
    add_text(filename);
    filename->add_option("--replacement", opt.replacement, "Replacement for removed characters");
    filename->callback([&] {
        std::string text;
        if (!take_text(opt, text))
            emit_err("EINVAL", "invalid hex");
        emit_ok({{"value", media::path::sanitize_filename(text, opt.replacement)}});
    });
    auto* normalize = path->add_subcommand("normalize", "Generic UTF-8 path");
    add_text(normalize);
    normalize->add_flag("--force", opt.force);
    normalize->callback([&] {
        std::string text;
        if (!take_text(opt, text))
            emit_err("EINVAL", "invalid hex");
        emit_ok({{"value", media::path::normalize_local_path(text, opt.force)}});
    });
    auto* safe = path->add_subcommand("safe", "Safe path component");
    add_text(safe);
    safe->callback([&] {
        std::string text;
        if (!take_text(opt, text))
            emit_err("EINVAL", "invalid hex");
        emit_ok({{"safe", media::path::is_safe_path_component(text)}});
    });
    auto* selection = path->add_subcommand("selection", "User path to open");
    add_text(selection);
    selection->callback([&] {
        std::string text;
        if (!take_text(opt, text))
            emit_err("EINVAL", "invalid hex");
        std::string err;
        if (!media::path::validate_path_spec_for_input_selection(text, err))
            emit_path_err(err);
        emit_ok({{"accepted", true}});
    });

    auto* uri = app.add_subcommand("uri", "URI parse and host policy");
    uri->require_subcommand(1);
    auto* parse_uri = uri->add_subcommand("parse", "Parse a URI");
    parse_uri->add_option("text", opt.text)->required();
    parse_uri->add_flag("--lenient", opt.lenient);
    parse_uri->add_flag("--reject-userinfo", opt.reject_userinfo);
    parse_uri->callback([&] {
        pm::commons::uri::ParseOptions options;
        options.reject_userinfo = opt.reject_userinfo;
        auto parsed = opt.lenient
            ? pm::commons::uri::parse_lenient(opt.text, options)
            : pm::commons::uri::parse(opt.text, options);
        if (!parsed)
            emit_err("EINVAL", "invalid uri");
        emit_ok({
            {"scheme", parsed->scheme},
            {"host", parsed->host},
            {"port", parsed->port},
            {"path", parsed->path},
            {"query", parsed->query},
            {"fragment", parsed->fragment},
            {"userinfo", parsed->userinfo},
        });
    });
    auto* decode = uri->add_subcommand("decode", "Percent-decode");
    decode->add_option("text", opt.text)->required();
    decode->callback([&] {
        auto value = pm::commons::uri::percent_decode(opt.text, false);
        if (!value)
            emit_err("EINVAL", "invalid percent encoding");
        emit_ok({{"value", *value}});
    });
    auto* priv = uri->add_subcommand("private", "Private or invalid host");
    priv->add_option("text", opt.text)->required();
    priv->callback([&] {
        auto parsed = pm::commons::uri::parse_http(opt.text);
        if (!parsed)
            emit_err("EINVAL", "not an http(s) uri");
        std::string reason;
        const bool blocked = pm::commons::uri::host_is_private_or_invalid(*parsed, reason);
        emit_ok({{"private", blocked}, {"reason", reason}});
    });

    auto* fs = app.add_subcommand("fs", "Ignore rules and magic bytes");
    fs->require_subcommand(1);
    auto* fmatch = fs->add_subcommand("match", "Gitignore match");
    fmatch->add_option("--pattern", opt.pattern)->required();
    fmatch->add_option("--rel", opt.rel)->required();
    fmatch->add_flag("--dir", opt.is_dir);
    fmatch->add_flag("--fold", opt.fold);
    fmatch->callback([&] {
        const auto list = pm::commons::fs::parse_gitignore(opt.pattern);
        const auto* hit = pm::commons::fs::last_match(list, opt.rel, opt.is_dir, opt.fold);
        if (!hit)
            emit_ok({{"matched", false}});
        emit_ok({
            {"matched", true},
            {"negative", (hit->flags & pm::commons::fs::PatternNegative) != 0},
            {"line", hit->line},
            {"text", hit->text},
        });
    });
    auto* ignore = fs->add_subcommand("ignore", "Walk parent mask files");
    ignore->add_option("path", opt.text)->required();
    ignore->add_flag("--dir", opt.is_dir);
    ignore->add_flag("--fold", opt.fold);
    ignore->callback([&] {
        emit_ok({{"ignored", pm::commons::fs::is_ignored(
            opt.text, opt.is_dir, pm::commons::fs::MaskKind::Gitignore, opt.fold)}});
    });
    auto* probe = fs->add_subcommand("probe", "Magic-byte sniff");
    probe->add_option("path", opt.text)->required();
    probe->callback([&] {
        const auto result = pm::commons::fs::probe_file(opt.text);
        if (!result.ok)
            emit_err(result.entity == pm::commons::fs::Entity::Missing ? "ENOENT" : "EINVAL", result.error);
        emit_ok({{"kind", result.kind}, {"mime", result.mime}, {"extension", result.extension}});
    });

    auto* library = app.add_subcommand("library", "Shared library probe");
    library->require_subcommand(1);
    auto* lprobe = library->add_subcommand("probe", "Can the loader see this name");
    lprobe->add_option("name", opt.name)->required();
    lprobe->callback([&] {
        if (opt.name.empty())
            emit_err("EINVAL", "empty library name");
        emit_ok({{"found", pm::commons::library::probe(opt.name)}});
    });

    auto* which = app.add_subcommand("which", "PATH lookup");
    which->add_option("name", opt.name)->required();
    which->add_option("--path", opt.paths);
    which->add_option("--ext", opt.exts);
    which->add_flag("--file", opt.file);
    which->callback([&] {
        pm::commons::process::WhichRequest request;
        request.name = opt.name;
        request.directories = opt.paths;
        request.extensions = opt.exts;
        request.require_execute = !opt.file;
        auto hit = pm::commons::process::which(request);
        if (!hit)
            emit_err("ENOENT", "not found");
        emit_ok({{"path", hit->path}, {"kind", hit->kind}});
    });

    auto* task = app.add_subcommand("task", "Stable task ids");
    task->require_subcommand(1);
    auto* tid = task->add_subcommand("id", "Resolve a task name");
    tid->add_option("name", opt.name)->required();
    tid->callback([&] {
        const auto id = pm::commons::task::id_from_name(opt.name);
        if (id == pm::commons::task::Id::None)
            emit_err("EINVAL", "unknown task");
        const auto* def = pm::commons::task::def_for(id);
        emit_ok({
            {"id", static_cast<std::uint32_t>(id)},
            {"name", pm::commons::task::id_name(id)},
            {"offload", def && def->allows_offload},
        });
    });
    auto* catalog = task->add_subcommand("catalog", "List stable task ids");
    catalog->callback([&] {
        nlohmann::json rows = nlohmann::json::array();
        for (const auto& def : pm::commons::task::catalog()) {
            rows.push_back({
                {"id", def.name},
                {"offload", def.allows_offload},
            });
        }
        emit_ok({{"tasks", rows}});
    });

    auto* claim = app.add_subcommand("claim", "Scheduler leadership claim");
    claim->require_subcommand(1);
    auto* write = claim->add_subcommand("write", "Write claim.json");
    write->add_option("--dir", opt.dir)->required();
    write->add_option("--pid", opt.pid)->required();
    write->add_option("--kind", opt.kind)->required();
    write->add_option("--priority", opt.priority)->required();
    write->add_option("--now", opt.now)->required();
    write->callback([&] {
        pm::commons::task::Claim row;
        row.pid = opt.pid;
        row.kind = opt.kind;
        row.priority = opt.priority;
        row.updated_at_ms = opt.now;
        if (!pm::commons::task::write_claim(opt.dir, row))
            emit_err("EIO", "could not write claim");
        emit_ok({{"pid", row.pid}, {"kind", row.kind}, {"priority", row.priority}, {"updatedAt", row.updated_at_ms}});
    });
    auto* read = claim->add_subcommand("read", "Read claim.json");
    read->add_option("--dir", opt.dir)->required();
    read->callback([&] {
        pm::commons::task::Claim row;
        if (!pm::commons::task::read_claim(opt.dir, row))
            emit_err("ENOENT", "claim missing");
        emit_ok({{"pid", row.pid}, {"kind", row.kind}, {"priority", row.priority}, {"updatedAt", row.updated_at_ms}});
    });
    auto* higher = claim->add_subcommand("higher", "A fresher higher-priority holder");
    higher->add_option("--dir", opt.dir)->required();
    higher->add_option("--self-pid", opt.self_pid)->required();
    higher->add_option("--self-priority", opt.self_priority)->required();
    higher->add_option("--now", opt.now)->required();
    higher->add_flag("--alive", opt.alive);
    higher->callback([&] {
        const bool live = pm::commons::task::higher_priority_claim_live(
            opt.dir, opt.self_priority, opt.self_pid, opt.now,
            opt.alive ? alive_hook : nullptr, nullptr);
        emit_ok({{"higher", live}});
    });

    auto* ipc = app.add_subcommand("ipc", "Length-prefixed frame codec");
    ipc->require_subcommand(1);
    auto* round = ipc->add_subcommand("roundtrip", "Encode and decode one message");
    round->add_option("--id", opt.id)->required();
    round->add_option("--type", opt.type)->required();
    round->add_option("--payload", opt.payload)->required();
    round->callback([&] {
        pm::commons::ipc::Message message{opt.id, opt.type, opt.payload};
        const auto frame = pm::commons::ipc::encode(message);
        pm::commons::ipc::Message back;
        if (!pm::commons::ipc::decode_frame(frame.data(), frame.size(), back))
            emit_err("EINVAL", "frame did not decode");
        emit_ok({
            {"id", back.id},
            {"type", back.type},
            {"payload", back.payload},
            {"bytes", frame.size()},
        });
    });

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& err) {
        const int code = app.exit(err);
        return code == 0 ? 0 : 2;
    }
    return 2;
}
