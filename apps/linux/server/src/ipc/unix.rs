//! Unix domain socket 传输：在 `$XDG_RUNTIME_DIR/qingjian.sock` 上服务 fcitx5 插件客户端，字节模式，帧由协议 codec 切。
//!
//! fcitx5 里每个输入上下文各开一条连接且失焦后连接仍在，所以不能串行服务：
//! 后台接受循环每来一个客户端就新建实例、起一条线程；[`Router`] 不跨线程，
//! 留在调用线程跑工人循环，各连接经通道把消息转给它串行处理。

use std::io;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::Path;
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, Sender};
use std::thread;
use std::time::Instant;

use qingjian_platform::protocol::{ClientMessage, read_message, write_message};

use super::Work;
use crate::dispatch::Router;

/// 在 Unix domain socket 上服务多个客户端。当前线程独占 [`Router`] 跑工人循环，正常不返回。
/// `work` 通道由调用方建，这里拿一份发送端给各连接。
/// 已有一个 Server 在跑（socket 已存在且被占用）时 bind 失败，直接报错退出
/// （两个 Server 会各持一份 Engine 状态）。
pub fn serve_unix(
    path: &Path,
    router: &mut Router,
    sender: Sender<Work>,
    receiver: Receiver<Work>,
) -> io::Result<()> {
    // 上次崩溃可能留下孤儿 socket 文件：先清掉再 bind（bind 一个已存在路径会失败）。
    if path.exists() {
        std::fs::remove_file(path).map_err(|error| {
            io::Error::other(format!("清理旧 socket {} 失败：{error}", path.display()))
        })?;
    }
    let listener = UnixListener::bind(path)?;
    thread::spawn(move || accept_loop(listener, sender));
    tracing::info!(path = %path.display(), "Unix socket 监听中");
    // 按 Router 的节拍来 tick：在等本地整句模型就几十毫秒一次，否则一秒看一次配置文件。
    // 到点时间是绝对的，不随消息重新计时——前台输入法进程隔几百毫秒就问一次切模式（SyncMode），
    // 若每收一条消息就重等一秒，tick 永远到不了，热加载与模型接入都会停摆。
    let mut due = Instant::now() + router.next_tick();
    loop {
        let now = Instant::now();
        if now >= due {
            router.tick();
            due = Instant::now() + router.next_tick();
            continue;
        }
        match receiver.recv_timeout(due - now) {
            Ok(Work::Client(message, reply)) => {
                let _ = reply.send(router.handle(message));
            }
            Ok(Work::Status(event)) => router.handle_status_event(event),
            Err(RecvTimeoutError::Timeout) => continue,
            Err(RecvTimeoutError::Disconnected) => break,
        }
        // 处理完消息节拍可能变短了（按键起了防抖）：到点时间只提前不推后。
        due = std::cmp::min(due, Instant::now() + router.next_tick());
    }
    Ok(())
}

/// 后台接受循环：每来一个客户端起一条线程，各连接互不阻塞。
fn accept_loop(listener: UnixListener, sender: Sender<Work>) {
    for conn in listener.incoming() {
        match conn {
            Ok(stream) => {
                let sender = sender.clone();
                thread::spawn(move || serve_client(stream, sender));
            }
            Err(error) => tracing::warn!(%error, "accept 失败"),
        }
    }
}

/// 一条连接的消息循环：读帧、交给工人循环、写回，直到对端在帧边界关闭。
fn serve_client(stream: UnixStream, sender: Sender<Work>) {
    let mut reader = match stream.try_clone() {
        Ok(reader) => reader,
        Err(error) => {
            tracing::warn!(%error, "克隆 socket 读端失败");
            return;
        }
    };
    let mut writer = stream;
    loop {
        match read_message::<_, ClientMessage>(&mut reader) {
            Ok(Some(message)) => {
                let (reply_tx, reply_rx) = mpsc::channel();
                if sender.send(Work::Client(message, reply_tx)).is_err() {
                    break;
                }
                match reply_rx.recv() {
                    Ok(Some(response)) => {
                        if write_message(&mut writer, &response).is_err() {
                            break;
                        }
                    }
                    Ok(None) => {}
                    Err(_) => break,
                }
            }
            Ok(None) => break,
            Err(error) => {
                tracing::debug!(%error, "连接读写错误，关闭");
                break;
            }
        }
    }
}
