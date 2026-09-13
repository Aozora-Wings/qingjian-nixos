//! Linux Server 进程入口：读配置、装配 Engine、在 Unix domain socket 上服务 fcitx5 插件。
//! 逻辑在库部分（[`qingjian_linux_server`]），这里只装配与启动。
//!
//! 布局与 Windows 版一致（见 `apps/windows/README.md`）：Engine 只此一份跑在独立进程，
//! fcitx5 插件（C++，`apps/linux/fcitx5/`）加载进输入法框架进程，只做按键转发与候选显示。

use std::path::{Path, PathBuf};

use qingjian_core::{Engine, Language};
use qingjian_linux_server::{
    AssemblySpec, LanguageModelFiles, Router, RouterConfig, ServerError, assembly, dispatch,
};
use qingjian_platform::{Config, LogLevel, resources};

/// 用户数据目录：`$XDG_DATA_HOME/qingjian`，缺省 `~/.local/share/qingjian`。
fn user_dir() -> Option<PathBuf> {
    std::env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".local/share")))
        .map(|dir| dir.join("qingjian"))
}

/// 配置文件：`$XDG_CONFIG_HOME/qingjian/config.toml`，缺省 `~/.config/qingjian/config.toml`。
fn config_path() -> Option<PathBuf> {
    std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".config")))
        .map(|dir| dir.join("qingjian").join("config.toml"))
}

/// 文件不存在按默认值；解析失败记错误退回默认。
fn load_config() -> Config {
    match config_path() {
        Some(path) => Config::load(&path).unwrap_or_else(|error| {
            tracing::error!(%error, path = %path.display(), "配置解析失败，用默认值");
            Config::default()
        }),
        None => Config::default(),
    }
}

/// 读密钥：工作目录 `.env`，再叠加用户配置目录 `.env`；不覆盖已有环境变量。
fn load_env() {
    let _ = dotenvy::dotenv();
    if let Some(env_file) = config_path().and_then(|path| {
        path.parent()
            .map(|dir| dir.join(".env"))
            .filter(|path| path.is_file())
    }) {
        let _ = dotenvy::from_path(&env_file);
    }
}

fn learning_language(config: &Config) -> Language {
    let code = &config.general.learning_language;
    code.parse().unwrap_or_else(|_| {
        tracing::warn!(code, "不认识的学习语言，按英文");
        Language::English
    })
}

/// `<root>/data/generated/<name>`，不存在为 `None`。
fn generated(root: &Path, name: &str) -> Option<PathBuf> {
    existing(root.join("data/generated").join(name))
}

/// `<root>/assets/<rel>`，不存在为 `None`。
fn asset(root: &Path, rel: &str) -> Option<PathBuf> {
    existing(root.join("assets").join(rel))
}

fn existing(path: PathBuf) -> Option<PathBuf> {
    path.is_file().then_some(path)
}

/// 正式词库，没有就回落手写样例。
fn default_dict(root: &Path) -> PathBuf {
    generated(root, "dict.qj").unwrap_or_else(|| sample_dict(root))
}

fn sample_dict(root: &Path) -> PathBuf {
    root.join("assets/sample/dict.tsv")
}

/// 某语言的释义表：打包过的优先，否则随 git 的 TSV。
fn glossary_file(root: &Path, language: Language) -> Option<PathBuf> {
    let code = language.code();
    generated(root, &format!("glossary-{code}.qj"))
        .or_else(|| asset(root, &format!("glossary/glossary-{code}.tsv")))
}

/// 正式词库装配失败回落样例词库，连样例都装不起来才报错。
fn assemble_with_fallback(mut spec: AssemblySpec, root: &Path) -> Result<Engine, ServerError> {
    assembly::assemble(&spec).or_else(|error| {
        tracing::error!(%error, dict = %spec.dict.display(), "正式词库装配失败，回落样例词库");
        spec.dict = sample_dict(root);
        assembly::assemble(&spec)
    })
}

fn log_dir() -> Option<PathBuf> {
    let dir = user_dir()?.join("logs");
    std::fs::create_dir_all(&dir).ok()?;
    Some(dir)
}

/// 级别按 `[general] log_level`（`RUST_LOG` 可覆盖），同时写 stderr 与按天滚动的文件（留 7 天）。
/// 返回的 guard 要活到进程结束，否则缓冲的日志不落盘。
fn init_logging(config: &Config) -> Option<tracing_appender::non_blocking::WorkerGuard> {
    use tracing_subscriber::fmt::writer::MakeWriterExt;
    let level = if config.general.log_level == LogLevel::Debug {
        "debug"
    } else {
        "info"
    };
    let filter = tracing_subscriber::EnvFilter::try_from_default_env()
        .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new(level));
    match log_dir() {
        Some(dir) => {
            let appender = tracing_appender::rolling::RollingFileAppender::builder()
                .rotation(tracing_appender::rolling::Rotation::DAILY)
                .filename_prefix("qingjian-server")
                .filename_suffix("log")
                .max_log_files(7)
                .build(&dir)
                .expect("构建滚动日志文件");
            let (writer, guard) = tracing_appender::non_blocking(appender);
            tracing_subscriber::fmt()
                .with_env_filter(filter)
                .with_ansi(false)
                .with_writer(writer.and(std::io::stderr))
                .init();
            Some(guard)
        }
        None => {
            tracing_subscriber::fmt().with_env_filter(filter).init();
            None
        }
    }
}

/// Unix domain socket 路径：`$XDG_RUNTIME_DIR/qingjian.sock`，缺省 `/tmp/qingjian-<uid>.sock`。
fn socket_path() -> PathBuf {
    if let Some(runtime) = std::env::var_os("XDG_RUNTIME_DIR") {
        return PathBuf::from(runtime).join("qingjian.sock");
    }
    let uid = unsafe { libc::getuid() };
    PathBuf::from(format!("/tmp/qingjian-{uid}.sock"))
}

fn main() {
    load_env();

    // 日志级别取自配置，所以先读配置再装日志。
    let config = load_config();
    let _log_guard = init_logging(&config);
    let language = learning_language(&config);
    // 装机布局与可执行文件同级（`share/qingjian/` 下的 data/assets 由打包方布置），开发布局是仓库根。
    let root = resources::bundled_root().unwrap_or_else(|| PathBuf::from("."));
    let dict = std::env::var_os("QINGJIAN_DICT")
        .map(PathBuf::from)
        .unwrap_or_else(|| default_dict(&root));
    let glossary = std::env::var_os("QINGJIAN_GLOSSARY")
        .map(PathBuf::from)
        .or_else(|| glossary_file(&root, language))
        .filter(|path| path.is_file());
    let bundled_dicts_dir = Some(root.join("data/generated/dicts")).filter(|dir| dir.is_dir());
    let spec = AssemblySpec {
        glossary: glossary.clone().map(|path| (language, path)),
        english_glossary: glossary_file(&root, Language::Chinese),
        english: generated(&root, "english.tsv"),
        emoji: ["emoji-zh.tsv", "emoji-en.tsv"]
            .into_iter()
            .filter_map(|name| asset(&root, &format!("emoji/{name}")))
            .collect(),
        language_model: LanguageModelFiles::find(&root.join("data/generated")),
        bundled_dicts_dir: bundled_dicts_dir.clone(),
        dictionaries: config.dictionaries.clone(),
        levels_dir: Some(root.join("assets/levels")),
        user_dir: user_dir(),
        input_log: config.general.input_log,
        ..AssemblySpec::new(&dict)
    };
    let mut engine = match assemble_with_fallback(spec, &root) {
        Ok(engine) => engine,
        Err(error) => {
            tracing::error!(%error, "样例词库也装配失败");
            std::process::exit(1);
        }
    };
    engine.set_fuzzy(config.fuzzy);
    engine.set_shuangpin(config.general.shuangpin());
    engine.set_mode_keys(config.shortcut.mode);
    engine.log_session(env!("CARGO_PKG_VERSION"), "linux");
    dispatch::attach_cloud(&mut engine, &config.predict);
    let router_config = RouterConfig::from(&config);
    let mut router = Router::new(engine, router_config.clone());
    let model_path = dispatch::find_model(user_dir().as_deref(), &root);
    router.configure_local_model(model_path.clone(), &config.model);
    if let Some(path) = config_path() {
        router.watch_config(&config, path, bundled_dicts_dir, user_dir());
    }
    tracing::info!(
        dict = %dict.display(),
        glossary = glossary.as_deref().map(|p| p.display().to_string()).unwrap_or_default(),
        language = language.code(),
        page_size = router_config.page_size,
        page_keys = %format!("{}{}", router_config.page_keys.0, router_config.page_keys.1),
        layout = router_config.layout.key(),
        theme = router_config.theme.key(),
        shuangpin = config.general.shuangpin().map(|s| s.key()).unwrap_or("全拼"),
        fuzzy = config.fuzzy.any(),
        cloud = config.predict.enabled,
        model = model_path.as_deref().map(|p| p.display().to_string()).unwrap_or_default(),
        model_enabled = config.model.enabled,
        sessions = router.session_count(),
        "青简 Linux Server 就绪"
    );

    serve(router);
}

/// 起 Unix socket 服务到进程结束。候选窗口由 fcitx5 插件显示（Server 不自绘），
/// 所以候选 / 状态条输出端用 Router 默认的 NoopSink。
fn serve(mut router: Router) {
    use qingjian_linux_server::ipc::{Work, unix};
    use std::sync::mpsc;
    let (work_tx, work_rx) = mpsc::channel::<Work>();
    if let Err(error) = unix::serve_unix(&socket_path(), &mut router, work_tx, work_rx) {
        tracing::error!(%error, "Unix socket 服务退出");
        std::process::exit(1);
    }
}
