#!/usr/bin/env bash
#
# setup_mac.sh - macOS (Apple Silicon, M4 Max想定) 向けワンショットセットアップ。
#
# 「インストールからセットアップまで一発」を目標に、以下を自動化します。
#   1. 事前チェック (macOS/arm64, Xcode CLT, Homebrew)
#   2. 依存インストール (cmake, onnxruntime, ffmpeg via Homebrew)
#   3. Adobe After Effects SDKの検出
#   4. cmakeビルド (upscale_core / upscale_cli / AIUpscaleプラグイン)
#   5. モデル取得 (download_models.py)
#   6. Premiere Pro MediaCoreフォルダへのインストール
#   7. スモークテスト (テスト画像で4倍化を実行し出力サイズを検証)
#
# 冪等設計: 既に完了している工程は検出してスキップします。何度実行しても
# 安全です。エラー時は即座に中断し、原因と対処法を表示します。
#
# 互換性メモ: このスクリプトは bash 3.2 (macOS標準/Appleが同梱するバージョン。
# GPLv3ライセンス回避のためAppleは bash 4系以降を同梱していません) を前提に
# 書かれています。連想配列 (declare -A)、readarray/mapfile、${var,,} などの
# bash4+専用機能は一切使用していません。zshでも動作しますが、bash 3.2で
# 動作確認することを優先しています。
#
# 使い方:
#   bash plugin/setup_mac.sh              # フルセットアップ
#   bash plugin/setup_mac.sh --check-only # 事前チェックのみ (Linux上のテスト用)
#   bash plugin/setup_mac.sh --uninstall  # MediaCoreからプラグインを削除
#   bash plugin/setup_mac.sh --help       # ヘルプ表示
#
set -euo pipefail

# ---------------------------------------------------------------------------
# 基本設定・ログ
# ---------------------------------------------------------------------------

# このスクリプト自身の場所から plugin/ ディレクトリを解決する。シンボリック
# リンク経由で呼ばれるケースは想定していない（このリポジトリでは発生しない）。
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN_DIR="${SCRIPT_DIR}"
BUILD_DIR="${PLUGIN_DIR}/build"
MODELS_DIR="${PLUGIN_DIR}/models"

TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
LOG_FILE="${PLUGIN_DIR}/setup_log_${TIMESTAMP}.log"

MEDIACORE_DIR="/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore"
PLUGIN_BUNDLE_NAME="AIUpscale.plugin"

CHECK_ONLY=0
UNINSTALL=0

# ---------------------------------------------------------------------------
# 引数パース
# ---------------------------------------------------------------------------
print_help() {
    cat <<'EOF'
使い方: bash plugin/setup_mac.sh [オプション]

オプション:
  --check-only   事前チェック(OS/アーキテクチャ/Xcode CLT/Homebrew)のみ実行し、
                 それ以外の工程(依存インストール、ビルド等)はスキップします。
                 macOS以外のOS(Linux等)でこのスクリプトの動作確認を行う際に
                 使用します。
  --uninstall    Premiere Pro/After EffectsのMediaCoreフォルダから
                 AIUpscaleプラグインを削除します。
  -h, --help     このヘルプを表示します。

引数無しで実行すると、依存インストールからビルド・モデル取得・インストール・
スモークテストまでを一括で行います。
EOF
}

for arg in "$@"; do
    case "${arg}" in
        --check-only)
            CHECK_ONLY=1
            ;;
        --uninstall)
            UNINSTALL=1
            ;;
        -h|--help)
            print_help
            exit 0
            ;;
        *)
            echo "不明なオプション: ${arg}" >&2
            print_help >&2
            exit 1
            ;;
    esac
done

# ---------------------------------------------------------------------------
# ログ関数
#
# 全出力を tee でログファイルにも書き込む (main の最後で設定)。ここでは
# メッセージ整形用の関数のみ定義する。
# ---------------------------------------------------------------------------
STEP_TOTAL=7

log_info() {
    printf '[INFO] %s\n' "$*"
}

log_step() {
    # 使い方: log_step <番号> "<説明>"
    printf '\n=== [%s/%s] %s ===\n' "$1" "${STEP_TOTAL}" "$2"
}

log_ok() {
    printf '  -> OK: %s\n' "$*"
}

log_skip() {
    printf '  -> スキップ (既に完了済み): %s\n' "$*"
}

log_warn() {
    printf '[WARN] %s\n' "$*" >&2
}

log_error() {
    printf '[ERROR] %s\n' "$*" >&2
}

# ---------------------------------------------------------------------------
# エラーハンドリング: エラー発生時に行番号と直前のコマンドを表示して中断する。
# set -e により、失敗したコマンドがあれば即座にこのtrapが呼ばれる。
# ---------------------------------------------------------------------------
on_error() {
    local exit_code=$?
    local line_no=$1
    log_error "セットアップが失敗しました (行 ${line_no}, 終了コード ${exit_code})。"
    log_error "上記のログ (${LOG_FILE}) を確認してください。"
    log_error "よくある対処法:"
    log_error "  - Xcode Command Line Toolsが未インストール: xcode-select --install"
    log_error "  - Homebrewが未インストール: https://brew.sh の指示に従い手動インストール"
    log_error "  - Adobe SDKが見つからない: 本スクリプトの案内に従い ~/AdobeSDK に展開後、再実行"
    log_error "  - ネットワークエラー: 再度スクリプトを実行してください (冪等設計のため、完了済み工程は自動的にスキップされます)"
    exit "${exit_code}"
}
trap 'on_error ${LINENO}' ERR

# ---------------------------------------------------------------------------
# [1/7] 事前チェック: OS/アーキテクチャ、Xcode CLT、Homebrew
# ---------------------------------------------------------------------------
step1_precheck() {
    log_step 1 "事前チェック (OS / アーキテクチャ / Xcode Command Line Tools / Homebrew)"

    local os_name
    os_name="$(uname -s)"
    local arch_name
    arch_name="$(uname -m)"
    log_info "検出したOS: ${os_name}, アーキテクチャ: ${arch_name}"

    if [ "${os_name}" != "Darwin" ]; then
        if [ "${CHECK_ONLY}" -eq 1 ]; then
            log_warn "macOS以外のOS上で --check-only 実行中です。実OSチェックはスキップし、以降のロジック検証のみ行います。"
            return 0
        fi
        log_error "このスクリプトはmacOS専用です (検出: ${os_name})。"
        log_error "Linux/Windowsで開発中の場合は --check-only オプションで構文/ロジック検証のみ行えます。"
        exit 1
    fi

    if [ "${arch_name}" != "arm64" ]; then
        log_error "Apple Silicon (arm64) が必要です (検出: ${arch_name})。"
        log_error "このプラグインはMacBook Pro M4 Max等のApple Siliconを主要ターゲットにしています。"
        log_error "Intel Mac上でx86_64ビルドを試す場合は、本スクリプトではなくplugin/README.mdの手動手順を参照してください。"
        exit 1
    fi
    log_ok "macOS (Darwin) / arm64 を確認しました。"

    if [ "${CHECK_ONLY}" -eq 1 ]; then
        log_info "--check-only モードのため、Xcode CLT/Homebrewのインストール誘導のみ行い、実インストールは行いません。"
    fi

    # --- Xcode Command Line Tools ---
    if xcode-select -p >/dev/null 2>&1; then
        log_ok "Xcode Command Line Tools は既にインストール済みです ($(xcode-select -p))。"
    else
        log_warn "Xcode Command Line Toolsが見つかりません。インストールを開始します。"
        log_warn "GUIダイアログが表示されるので、インストール完了後に本スクリプトを再実行してください。"
        xcode-select --install || true
        log_error "Xcode Command Line Toolsのインストールダイアログを起動しました。インストール完了後、再度このスクリプトを実行してください。"
        exit 2
    fi

    # --- Homebrew ---
    if command -v brew >/dev/null 2>&1; then
        log_ok "Homebrew は既にインストール済みです ($(command -v brew))。"
    else
        # セキュリティ上の判断: 本スクリプトはHomebrewの公式インストーラーを
        # 自動的に curl | bash で実行しません。任意のシェルスクリプトを
        # 無条件にrootに近い権限で実行することになるため、ユーザー自身が
        # 公式手順を確認・実行することを強く推奨します。
        log_error "Homebrewが見つかりません。以下のコマンドを手動で実行してインストールしてください:"
        log_error ""
        log_error '  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"'
        log_error ""
        log_error "(本スクリプトはセキュリティ上の判断として、このコマンドを自動実行しません。"
        log_error " 上記コマンドは https://brew.sh の公式手順そのものです。内容を確認の上、手動で実行してください。)"
        log_error "インストール完了後、本スクリプトを再実行してください。"
        exit 2
    fi
}

# ---------------------------------------------------------------------------
# [2/7] 依存インストール: cmake, onnxruntime, ffmpeg (Homebrew)
# ---------------------------------------------------------------------------
brew_prefix_cache=""
onnxruntime_include_dir_result=""

# onnxruntimeのヘッダ (onnxruntime_cxx_api.h) を $1/include 配下から探索する。
# Homebrewのonnxruntime formulaはバージョンによりヘッダを include/ 直下ではなく
# include/onnxruntime/ サブディレクトリ (更に深い階層のこともある) に配置する
# ため、固定パスをチェックするのではなく find で実際の設置場所を探し、見つかった
# ディレクトリ (onnxruntime_cxx_api.h を含むディレクトリそのもの) を
# onnxruntime_include_dir_result にセットする。複数見つかった場合は最初の1件を
# 採用する。見つからない場合は onnxruntime_include_dir_result を空にして
# 呼び出し元にエラーとして扱わせる。
find_onnxruntime_include_dir() {
    local prefix="$1"
    onnxruntime_include_dir_result=""

    local found
    found="$(find "${prefix}/include" -name onnxruntime_cxx_api.h -print -quit 2>/dev/null || true)"
    if [ -n "${found}" ]; then
        onnxruntime_include_dir_result="$(dirname "${found}")"
        return 0
    fi
    return 1
}

step2_dependencies() {
    log_step 2 "依存インストール (Homebrew: cmake, onnxruntime, ffmpeg)"

    local pkg
    for pkg in cmake onnxruntime ffmpeg; do
        if brew list --formula "${pkg}" >/dev/null 2>&1; then
            log_skip "${pkg} (brew list で確認済み)"
        else
            log_info "brew install ${pkg} を実行します..."
            brew install "${pkg}"
            log_ok "${pkg} をインストールしました。"
        fi
    done

    brew_prefix_cache="$(brew --prefix onnxruntime)"
    log_info "onnxruntime prefix: ${brew_prefix_cache}"

    if find_onnxruntime_include_dir "${brew_prefix_cache}"; then
        log_ok "onnxruntimeのヘッダを検出しました: ${onnxruntime_include_dir_result}/onnxruntime_cxx_api.h"
    else
        log_error "onnxruntimeのヘッダ (onnxruntime_cxx_api.h) が ${brew_prefix_cache}/include 配下に見つかりません。"
        log_error "${brew_prefix_cache}/include の内容:"
        find "${brew_prefix_cache}/include" -maxdepth 3 2>&1 | while IFS= read -r line; do
            log_error "  ${line}"
        done
        log_error "brew reinstall onnxruntime を試してください。"
        exit 1
    fi
}

# ---------------------------------------------------------------------------
# [3/7] Adobe After Effects SDK検出
# ---------------------------------------------------------------------------
ae_sdk_path_result=""

find_ae_sdk() {
    # 優先順位: 環境変数 AE_SDK_PATH -> ~/AdobeSDK -> ~/Downloads/AfterEffectsSDK*
    #
    # 呼び出し元が結果を変数 ae_sdk_path_result 経由で受け取れるよう、
    # (このファイルはbash 3.2互換のため連想配列やlocal -nは使わず)
    # グローバル変数への代入で結果を返す。
    ae_sdk_path_result=""

    if [ -n "${AE_SDK_PATH:-}" ] && [ -d "${AE_SDK_PATH}" ]; then
        ae_sdk_path_result="${AE_SDK_PATH}"
        return 0
    fi

    if [ -d "${HOME}/AdobeSDK" ]; then
        ae_sdk_path_result="${HOME}/AdobeSDK"
        return 0
    fi

    # ~/Downloads 配下の AfterEffectsSDK* ディレクトリ/zipを探索。
    # 複数マッチした場合は最新の更新日時のものを採用する。
    local candidate
    local best=""
    local best_mtime=0
    if [ -d "${HOME}/Downloads" ]; then
        for candidate in "${HOME}/Downloads"/AfterEffectsSDK*; do
            [ -d "${candidate}" ] || continue
            # BSD stat (macOS, the only supported platform for this script)
            # accepts `-f '%m'` for a file-status mtime. On non-BSD stat
            # implementations (e.g. GNU stat, encountered only when
            # sanity-checking this function on Linux) `-f` means something
            # else entirely and can print non-numeric text, so validate the
            # result is a plain integer before using it in a comparison --
            # otherwise default to 0 rather than letting `[` error out.
            local mtime
            mtime="$(stat -f '%m' "${candidate}" 2>/dev/null || echo 0)"
            case "${mtime}" in
                ''|*[!0-9]*) mtime=0 ;;
            esac
            if [ "${mtime}" -ge "${best_mtime}" ]; then
                best="${candidate}"
                best_mtime="${mtime}"
            fi
        done
    fi
    if [ -n "${best}" ]; then
        ae_sdk_path_result="${best}"
        return 0
    fi

    return 1
}

# AE_Effect.h の探索。SDKバージョンによりパスが多少異なるため固定候補を
# 先にチェックし、見つからなければfindでフォールバック探索する。
# 展開後はネスト階層が深くなる可能性があるため maxdepth は8とする。
# 見つかったパスをechoで返す (見つからない場合は空文字)。
find_ae_effect_header() {
    local sdk_path="$1"
    local header_candidates="
${sdk_path}/Examples/Headers/AE_Effect.h
${sdk_path}/Examples/Headers/SDK/AE_Effect.h
"
    local h
    for h in ${header_candidates}; do
        if [ -f "${h}" ]; then
            echo "${h}"
            return 0
        fi
    done
    find "${sdk_path}" -maxdepth 8 -name 'AE_Effect.h' -print -quit 2>/dev/null || true
}

# find_ae_effect_header() が返す絶対パスは、探索の起点として渡した
# ae_sdk_path_result (例: ダウンロードzipのトップディレクトリ ~/AdobeSDK) の
# 直下ではなく、実際にはさらにネストしたディレクトリ (例:
# ~/AdobeSDK/AfterEffectsSDK_25.6_61_mac/ae25.6_61.64bit.AfterEffectsSDK/)
# 配下に見つかることがある。CMakeLists.txt側は
# ${AE_SDK_PATH}/Examples/Headers/... という固定相対パスでヘッダを探すため、
# ae_sdk_path_result をそのまま -DAE_SDK_PATH として渡すと (このケースのように)
# AEConfig.h 等が見つからずビルドが失敗する。
#
# そこで、見つかったヘッダの絶対パスから「SDK実体のルート」(そこを起点に
# Examples/Headers/AE_Effect.h が実在するディレクトリ) を逆算する。
# find_ae_effect_header() が返し得るパスの形は次の2通り (固定候補ヒット時)、
# もしくはそれ以外の深さ (findによるフォールバック探索時) があるため、
# パス末尾のパターンで判定し、文字列除去で実体ルートを得る。
# 結果はグローバル変数 ae_sdk_effective_root_result にセットする。
ae_sdk_effective_root_result=""

derive_ae_sdk_effective_root() {
    local header_found="$1"
    ae_sdk_effective_root_result=""

    case "${header_found}" in
        */Examples/Headers/SDK/AE_Effect.h)
            ae_sdk_effective_root_result="${header_found%/Examples/Headers/SDK/AE_Effect.h}"
            ;;
        */Examples/Headers/AE_Effect.h)
            ae_sdk_effective_root_result="${header_found%/Examples/Headers/AE_Effect.h}"
            ;;
        *)
            # findによるフォールバック探索でヒットした場合など、上記2パターン
            # に一致しない深さで見つかることがある。この場合は
            # 「.../Examples/Headers/AE_Effect.h」という一般的なSDKレイアウトを
            # 前提に、ヘッダファイルから3階層上 (AE_Effect.h -> Headers ->
            # Examples -> 実体ルート) をベストエフォートで採用する。
            local d
            d="$(dirname "${header_found}")"   # .../Examples/Headers (or deeper nest)
            d="$(dirname "${d}")"               # .../Examples
            d="$(dirname "${d}")"               # 実体ルート (ベストエフォート)
            ae_sdk_effective_root_result="${d}"
            ;;
    esac
}

# ダウンロードしたAdobe SDK zipの中身が二重圧縮アーカイブ
# (*.tar.zstd.zip) のまま未展開の状態で提供されるケースに対応する。
# $1: 本体アーカイブ (*.tar.zstd.zip) の絶対パス
# アーカイブと同じディレクトリ内に展開する。中間ファイル(.tar)は
# 展開成功後に削除するが、元の .tar.zstd.zip は残す。
extract_ae_sdk_archive() {
    local archive="$1"
    local archive_dir
    archive_dir="$(cd "$(dirname "${archive}")" && pwd)"
    local archive_base
    archive_base="$(basename "${archive}")"
    local tar_zstd_name="${archive_base%.zip}"
    local tar_name="${tar_zstd_name%.zstd}"

    log_info "unzip -o ${archive_base} (${archive_dir})"
    ( cd "${archive_dir}" && unzip -o "${archive_base}" >/dev/null )

    if [ ! -f "${archive_dir}/${tar_zstd_name}" ]; then
        log_error "unzip後に ${tar_zstd_name} が見つかりません。zipの内容が想定と異なる可能性があります。"
        return 1
    fi

    # zstd実行コマンドの決定: PATH上のzstd -> SDK同梱の./zstd -> brew install zstd
    local zstd_cmd=""
    if command -v zstd >/dev/null 2>&1; then
        zstd_cmd="zstd"
    elif [ -f "${archive_dir}/zstd" ]; then
        chmod +x "${archive_dir}/zstd" || true
        xattr -d com.apple.quarantine "${archive_dir}/zstd" 2>/dev/null || true
        if "${archive_dir}/zstd" --version >/dev/null 2>&1; then
            zstd_cmd="${archive_dir}/zstd"
        fi
    fi
    if [ -z "${zstd_cmd}" ]; then
        log_warn "zstdコマンドが見つからないため brew install zstd を実行します..."
        brew install zstd
        if command -v zstd >/dev/null 2>&1; then
            zstd_cmd="zstd"
        fi
    fi
    if [ -z "${zstd_cmd}" ]; then
        log_error "zstdコマンドが利用できません。展開を中止します。"
        return 1
    fi

    log_info "${zstd_cmd} -d -f ${tar_zstd_name}"
    ( cd "${archive_dir}" && "${zstd_cmd}" -d -f "${tar_zstd_name}" )

    if [ ! -f "${archive_dir}/${tar_name}" ]; then
        log_error "zstd展開後に ${tar_name} が見つかりません。"
        return 1
    fi

    log_info "tar -xf ${tar_name} (${archive_dir})"
    ( cd "${archive_dir}" && tar -xf "${tar_name}" )
    rm -f "${archive_dir}/${tar_name}"
    log_ok "SDKアーカイブを展開しました: ${archive_dir}"
    return 0
}

step3_ae_sdk() {
    log_step 3 "Adobe After Effects SDK検出"

    if find_ae_sdk; then
        log_ok "Adobe SDKを検出しました: ${ae_sdk_path_result}"
    else
        log_error "Adobe After Effects SDKが見つかりません。"
        log_error "検索対象: \$AE_SDK_PATH, ~/AdobeSDK, ~/Downloads/AfterEffectsSDK*"
        log_error ""
        log_error "以下の手順でSDKを入手してください:"
        log_error "  1. https://developer.adobe.com/after-effects/ を開く (無償、Adobeアカウントが必要)"
        log_error "  2. 'After Effects SDK' をダウンロード"
        log_error "  3. ダウンロードしたzipを ~/AdobeSDK に展開する"
        log_error "  4. 本スクリプトを再実行する"
        if command -v open >/dev/null 2>&1; then
            open "https://developer.adobe.com/after-effects/" || true
        fi
        exit 2
    fi

    # ヘッダの存在検証 (SDKバージョンによりパスが多少異なるため複数箇所を探索)
    local header_found=""
    header_found="$(find_ae_effect_header "${ae_sdk_path_result}")"

    # ヘッダが見つからない場合、Adobe配布zip特有の未展開状態
    # (*.tar.zstd.zip / extractzstd.sh / zstd / README-HowToBuild-Mac.txt が
    # 展開されないまま置かれている状態) の可能性があるため、本体アーカイブを
    # 探索し、見つかれば自動展開を試みてから再度ヘッダを探索する。
    if [ -z "${header_found}" ]; then
        local sdk_archive
        sdk_archive="$(find "${ae_sdk_path_result}" -maxdepth 3 -name '*.tar.zstd.zip' -print -quit 2>/dev/null || true)"
        if [ -n "${sdk_archive}" ]; then
            log_warn "SDKが未展開のため自動展開します: ${sdk_archive}"
            if extract_ae_sdk_archive "${sdk_archive}"; then
                header_found="$(find_ae_effect_header "${ae_sdk_path_result}")"
            fi
        fi
    fi

    if [ -z "${header_found}" ]; then
        log_error "SDKディレクトリ (${ae_sdk_path_result}) 内に AE_Effect.h が見つかりません。"
        log_error "SDKの展開が不完全か、想定と異なるバージョン/レイアウトの可能性があります。"
        log_error "plugin/README.md の「前提条件」節を参照し、正しいSDKを再ダウンロードしてください。"
        log_error "SDK内の README-HowToBuild-Mac.txt の手順で手動展開してから再実行してください。"
        exit 1
    fi
    log_ok "AE_Effect.h を確認しました: ${header_found}"

    # ae_sdk_path_result (検出/指定されたSDKディレクトリ) がSDK実体のルートと
    # 一致するとは限らない (ダウンロードzipのトップディレクトリがそのまま
    # AE_SDK_PATHになっているケースでは、実体はさらに1〜2階層ネストした場所に
    # ある)。CMakeLists.txt は ${AE_SDK_PATH}/Examples/Headers/... という
    # 固定相対パスでヘッダを探すため、cmakeに渡すAE_SDK_PATHはヘッダから逆算
    # した実体ルートでなければならない。
    derive_ae_sdk_effective_root "${header_found}"
    if [ "${ae_sdk_effective_root_result}" != "${ae_sdk_path_result}" ]; then
        log_warn "AE_SDK_PATH (${ae_sdk_path_result}) はSDK実体のルートと異なります。"
        log_warn "cmakeへは実体ルートを渡します: ${ae_sdk_effective_root_result}"
    fi
    log_ok "SDK実体ルート (cmakeの -DAE_SDK_PATH に使用): ${ae_sdk_effective_root_result}"
}

# ---------------------------------------------------------------------------
# [4/7] ビルド
# ---------------------------------------------------------------------------
step4_build() {
    log_step 4 "cmakeビルド (upscale_core / upscale_cli / AIUpscale)"

    local ncpu
    ncpu="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

    log_info "cmake configure: -DONNXRUNTIME_ROOT=${brew_prefix_cache} -DONNXRUNTIME_INCLUDE_DIR=${onnxruntime_include_dir_result} -DAE_SDK_PATH=${ae_sdk_effective_root_result}"
    cmake -S "${PLUGIN_DIR}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DONNXRUNTIME_ROOT="${brew_prefix_cache}" \
        -DONNXRUNTIME_INCLUDE_DIR="${onnxruntime_include_dir_result}" \
        -DAE_SDK_PATH="${ae_sdk_effective_root_result}"

    log_info "cmake --build (並列度 ${ncpu})"
    cmake --build "${BUILD_DIR}" --config Release -j"${ncpu}"

    if [ ! -x "${BUILD_DIR}/upscale_cli" ]; then
        log_error "upscale_cli のビルド成果物が見つかりません (${BUILD_DIR}/upscale_cli)。"
        exit 1
    fi
    log_ok "upscale_cli をビルドしました: ${BUILD_DIR}/upscale_cli"

    # プラグインバンドルは Xcode generatorを使わない限り BUILD_DIR 直下、
    # または BUILD_DIR/Release/ (マルチコンフィグ) に生成される。両方探索する。
    local bundle_path=""
    if [ -d "${BUILD_DIR}/${PLUGIN_BUNDLE_NAME}" ]; then
        bundle_path="${BUILD_DIR}/${PLUGIN_BUNDLE_NAME}"
    elif [ -d "${BUILD_DIR}/Release/${PLUGIN_BUNDLE_NAME}" ]; then
        bundle_path="${BUILD_DIR}/Release/${PLUGIN_BUNDLE_NAME}"
    fi

    if [ -z "${bundle_path}" ]; then
        log_error "AIUpscale.plugin バンドルが見つかりません。AE_SDK_PATHの内容またはCMake出力を確認してください。"
        exit 1
    fi
    log_ok "AIUpscale.plugin をビルドしました: ${bundle_path}"
    plugin_bundle_path_result="${bundle_path}"
}
plugin_bundle_path_result=""

# ---------------------------------------------------------------------------
# [5/7] モデル取得
# ---------------------------------------------------------------------------
step5_models() {
    log_step 5 "モデル取得 (download_models.py)"

    if [ -f "${MODELS_DIR}/realesrgan-x4plus.onnx" ]; then
        log_skip "realesrgan-x4plus.onnx は既に ${MODELS_DIR} に存在します。"
    else
        log_info "python3 plugin/scripts/download_models.py を実行します..."
        log_info "注意: ダウンロードしたモデルにSHA-256の既知ハッシュが登録されていない場合、"
        log_info "      スクリプトは『検証をスキップした』という警告を表示します。これは"
        log_info "      ファイルが安全と保証されたわけではなく、単に照合対象のハッシュが"
        log_info "      まだ登録されていないことを意味します (plugin/scripts/download_models.py の"
        log_info "      KNOWN_SHA256 参照)。入手元(Hugging Face)を信頼できる場合のみ使用してください。"
        python3 "${PLUGIN_DIR}/scripts/download_models.py" --out-dir "${MODELS_DIR}" || \
            log_warn "モデルダウンロードが一部失敗しました。plugin/README.md の手動エクスポート手順を参照してください。"
    fi

    step5b_anime_onnx_convert
}

# Anime用ONNXへの自動変換: download_models.py はONNX直配布が無いため
# RealESRGAN_x4plus_anime_6B.pth (PyTorch重み) のみをダウンロードする。
# ここでは basicsr 非依存の export_anime_onnx.py を使い、専用venv内で
# torch/onnx をインストールした上でONNXへ変換する。
#
# 冪等設計: 既に realesrgan-x4plus-anime.onnx が存在すればスキップする。
# .pthが無ければ (ダウンロード失敗等) 何もしない (step7でanime系はスキップ
# されるだけで、photo系のスモークテストは通常通り実行される)。
step5b_anime_onnx_convert() {
    local anime_onnx="${MODELS_DIR}/realesrgan-x4plus-anime.onnx"
    local anime_pth="${MODELS_DIR}/RealESRGAN_x4plus_anime_6B.pth"

    if [ -f "${anime_onnx}" ]; then
        log_skip "realesrgan-x4plus-anime.onnx は既に ${MODELS_DIR} に存在します。"
        return 0
    fi

    if [ ! -f "${anime_pth}" ]; then
        log_warn "Anime用PyTorch重み (${anime_pth}) が見つからないため、ONNX変換をスキップします。"
        log_warn "download_models.py のダウンロードが失敗した可能性があります。ログを確認してください。"
        return 0
    fi

    log_info "Anime用ONNXへの自動変換を行います (${anime_pth} -> ${anime_onnx})。"

    local torch_venv="${BUILD_DIR}/torch-venv"
    if [ -x "${torch_venv}/bin/python3" ]; then
        log_skip "torch-venv は既に ${torch_venv} に存在します。"
    else
        log_info "変換用の専用venvを作成します: ${torch_venv}"
        python3 -m venv "${torch_venv}"
        log_info "torch / onnx / onnxruntime をインストールします (初回のみ数分かかります。torchは"
        log_info "数百MB〜数GB程度のダウンロードになります)..."
        "${torch_venv}/bin/pip" install --upgrade pip >/dev/null
        "${torch_venv}/bin/pip" install torch onnx onnxruntime
    fi

    log_info "python3 plugin/scripts/export_anime_onnx.py を実行します..."
    "${torch_venv}/bin/python3" "${PLUGIN_DIR}/scripts/export_anime_onnx.py" \
        "${anime_pth}" "${anime_onnx}"

    if [ -f "${anime_onnx}" ]; then
        log_ok "Anime用ONNXへの変換が完了しました: ${anime_onnx}"
    else
        log_warn "Anime用ONNXへの変換に失敗しました。plugin/scripts/export_anime_onnx.py を手動実行して"
        log_warn "エラー内容を確認してください。Photo用モデルのみでの利用は引き続き可能です。"
    fi
}

# ---------------------------------------------------------------------------
# [6/7] インストール (Premiere Pro MediaCoreフォルダへ配置)
# ---------------------------------------------------------------------------
step6_install() {
    log_step 6 "インストール (${MEDIACORE_DIR})"

    local dest_bundle="${MEDIACORE_DIR}/${PLUGIN_BUNDLE_NAME}"
    local dest_models="${MEDIACORE_DIR}/models"

    log_info "${MEDIACORE_DIR} への書き込みには管理者権限が必要です (システム共通のプラグインフォルダのため)。"
    log_info "sudo でコピーを行います。パスワードの入力を求められる場合があります。"

    sudo mkdir -p "${MEDIACORE_DIR}"

    if [ -e "${dest_bundle}" ]; then
        local backup="${dest_bundle}.bak.${TIMESTAMP}"
        log_warn "既存のインストールを検出しました。上書き前に退避します: ${backup}"
        sudo mv "${dest_bundle}" "${backup}"
    fi
    sudo cp -R "${plugin_bundle_path_result}" "${dest_bundle}"
    log_ok "プラグイン本体をインストールしました: ${dest_bundle}"

    if [ -e "${dest_models}" ]; then
        local backup_models="${dest_models}.bak.${TIMESTAMP}"
        log_warn "既存のmodels/を検出しました。上書き前に退避します: ${backup_models}"
        sudo mv "${dest_models}" "${backup_models}"
    fi
    sudo cp -R "${MODELS_DIR}" "${dest_models}"
    log_ok "モデルをインストールしました: ${dest_models}"

    # プラグイン一式は管理者が配置するが、後続の読み込みはPremiere/AE
    # (通常ユーザー権限で起動) が行うため、読み取り権限を全ユーザーに開く。
    sudo chmod -R a+rX "${dest_bundle}" "${dest_models}"
}

# ---------------------------------------------------------------------------
# --uninstall
# ---------------------------------------------------------------------------
do_uninstall() {
    log_info "MediaCoreフォルダからAIUpscaleプラグインを削除します: ${MEDIACORE_DIR}"
    local dest_bundle="${MEDIACORE_DIR}/${PLUGIN_BUNDLE_NAME}"
    local dest_models="${MEDIACORE_DIR}/models"

    local removed=0
    if [ -e "${dest_bundle}" ]; then
        sudo rm -rf "${dest_bundle}"
        log_ok "削除しました: ${dest_bundle}"
        removed=1
    fi
    if [ -e "${dest_models}" ]; then
        sudo rm -rf "${dest_models}"
        log_ok "削除しました: ${dest_models}"
        removed=1
    fi

    if [ "${removed}" -eq 0 ]; then
        log_info "インストール済みのAIUpscaleプラグインは見つかりませんでした (既にアンインストール済みか、未インストールです)。"
    else
        log_info "アンインストールが完了しました。Premiere Pro / After Effectsを再起動してください。"
    fi
}

# ---------------------------------------------------------------------------
# [7/7] スモークテスト
# ---------------------------------------------------------------------------
step7_smoketest() {
    log_step 7 "スモークテスト (テスト画像で4倍化を実行)"

    local test_png="/tmp/ai_upscale_setup_test_in.png"
    local test_out="/tmp/ai_upscale_setup_test_out.png"

    # Photo用モデルを優先し、無ければAnime用にフォールバックする。片方でも
    # 存在すれば推論パイプライン全体 (upscale_cli経由) の疎通確認としては
    # 十分なため、どちらのモデルが取得できたかに関わらずスモークテストを
    # 実行できるようにする。
    local test_model=""
    if [ -f "${MODELS_DIR}/realesrgan-x4plus.onnx" ]; then
        test_model="${MODELS_DIR}/realesrgan-x4plus.onnx"
    elif [ -f "${MODELS_DIR}/realesrgan-x4plus-anime.onnx" ]; then
        test_model="${MODELS_DIR}/realesrgan-x4plus-anime.onnx"
    fi

    if [ -z "${test_model}" ]; then
        log_warn "モデル (realesrgan-x4plus.onnx / realesrgan-x4plus-anime.onnx) が" \
                 "${MODELS_DIR} に見つからないため、スモークテストをスキップします。"
        log_warn "plugin/scripts/download_models.py を手動で実行し、モデル取得後に再実行してください。"
        return 0
    fi
    log_info "使用するモデル: ${test_model}"

    log_info "テスト画像を生成します..."
    if command -v python3 >/dev/null 2>&1 && [ -f "${PLUGIN_DIR}/tests/make_test_image.py" ]; then
        python3 "${PLUGIN_DIR}/tests/make_test_image.py" "${test_png}" 64 64
    elif command -v ffmpeg >/dev/null 2>&1; then
        ffmpeg -y -loglevel error -f lavfi -i color=c=blue:s=64x64 -frames:v 1 "${test_png}"
    else
        log_warn "テスト画像生成手段 (python3 make_test_image.py / ffmpeg) が見つからないため、スモークテストをスキップします。"
        return 0
    fi

    log_info "upscale_cli で4倍化を実行します..."
    "${BUILD_DIR}/upscale_cli" "${test_model}" "${test_png}" "${test_out}"

    if [ -f "${PLUGIN_DIR}/tests/check_png_size.py" ]; then
        python3 "${PLUGIN_DIR}/tests/check_png_size.py" "${test_out}" --expect 256 256
    fi
    log_ok "スモークテストに成功しました (${test_png} 64x64 -> ${test_out} 256x256)。"
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
main() {
    # ログファイルへの tee 設定。--check-only 時もログは残す。
    exec > >(tee -a "${LOG_FILE}") 2>&1

    log_info "AI Upscale plugin セットアップスクリプト開始 ($(date))"
    log_info "ログファイル: ${LOG_FILE}"

    if [ "${UNINSTALL}" -eq 1 ]; then
        step1_precheck
        do_uninstall
        exit 0
    fi

    step1_precheck

    if [ "${CHECK_ONLY}" -eq 1 ]; then
        log_info "--check-only モードのため、依存インストール以降は実行せず終了します。"
        exit 0
    fi

    step2_dependencies
    step3_ae_sdk
    step4_build
    step5_models
    step6_install
    step7_smoketest

    log_info ""
    log_info "セットアップ完了。Premiereを再起動し、エフェクト > AI Enhance > AI Upscale を確認してください。"
}

main "$@"
