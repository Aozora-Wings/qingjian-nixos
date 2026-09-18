//! 配置热加载记的状态。

use std::path::PathBuf;
use std::time::{Instant, SystemTime};

use qingjian_platform::DictionariesConfig;
use qingjian_predict::PredictConfig;

/// 热加载状态。
pub(crate) struct ConfigReload {
    /// `config.toml` 路径。
    pub(super) config_path: PathBuf,

    /// fcitx5 插件保存的覆盖层 `config.fcitx5.toml`；没有时为 `None`。
    pub(super) fcitx5_path: Option<PathBuf>,

    /// 上次看文件的时间（节流用）。
    pub(super) last_check: Instant,

    /// 随包领域词库目录。
    pub(super) bundled_dicts_dir: Option<PathBuf>,

    /// 用户数据目录（导入词库在其 `dicts/` 下）。
    pub(super) user_dir: Option<PathBuf>,

    /// 上次看到的 mtime（主文件）。
    pub(super) last_mtime: Option<SystemTime>,

    /// 上次看到的 mtime（fcitx5 覆盖层）。
    pub(super) fcitx5_mtime: Option<SystemTime>,

    /// 已应用的 `[predict]`。
    pub(super) applied_predict: PredictConfig,

    /// 已应用的 `[dictionaries]`。
    pub(super) applied_dictionaries: DictionariesConfig,
}
