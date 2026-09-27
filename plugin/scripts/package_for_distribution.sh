#!/usr/bin/env bash
#
# package_for_distribution.sh - ビルド済み AIUpscale.plugin を、知人へ個人配布
#                                するための1本のzipファイルにまとめる。
#
# 背景: 本プラグインは自己完結型 (モデル・onnxruntime不要、AIUpscale.plugin
# バンドル1つで動く) になったため、開発者以外の知人に渡す場合でも
# plugin/setup_mac.sh のフルセットアップを踏ませる必要はない。本スクリプトは
# 既にビルド済みのバンドルを、受け取った側がダブルクリックだけで
# インストールできる形にパッケージングする。
#
# コード署名・Apple公証は行っていない (個人間で数人に配布する用途のため)。
# そのため、受け取り側は初回起動時にmacOS Gatekeeperの「開発元を確認できない」
# 警告に遭遇する。同梱の「インストールする.command」がGatekeeperの隔離属性
# (com.apple.quarantine) 解除まで含めて自動化するため、実害はない
# (詳細は「はじめにお読みください.txt」参照)。
#
# 使い方:
#   bash plugin/scripts/package_for_distribution.sh
#
# 事前にビルドが完了している必要がある (plugin/build/AIUpscale.plugin、
# または既にインストール済みの MediaCore 配下の AIUpscale.plugin)。
# 完了すると ~/Downloads/AIUpscale_v<バージョン>_<YYYYMMDD>.zip が生成される。
#
set -euo pipefail

# ---------------------------------------------------------------------------
# 基本設定
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PLUGIN_DIR}/build"

PLUGIN_BUNDLE_NAME="AIUpscale.plugin"
MEDIACORE_DIR="/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore"

DOWNLOADS_DIR="${HOME}/Downloads"

# ---------------------------------------------------------------------------
# ログ用関数 (setup_mac.sh / upscale_video.sh と同じスタイル)
# ---------------------------------------------------------------------------
log_info() {
    printf '[INFO] %s\n' "$*"
}

log_ok() {
    printf '  -> OK: %s\n' "$*"
}

log_warn() {
    printf '[WARN] %s\n' "$*" >&2
}

log_error() {
    printf '[ERROR] %s\n' "$*" >&2
}

# ---------------------------------------------------------------------------
# ステージングディレクトリの後始末
# ---------------------------------------------------------------------------
STAGING_DIR=""

cleanup() {
    if [ -n "${STAGING_DIR}" ] && [ -d "${STAGING_DIR}" ]; then
        rm -rf "${STAGING_DIR}"
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# [1] ビルド済みバンドルの探索
# ---------------------------------------------------------------------------
find_plugin_bundle() {
    if [ -d "${BUILD_DIR}/${PLUGIN_BUNDLE_NAME}" ]; then
        echo "${BUILD_DIR}/${PLUGIN_BUNDLE_NAME}"
        return 0
    fi
    if [ -d "${BUILD_DIR}/Release/${PLUGIN_BUNDLE_NAME}" ]; then
        echo "${BUILD_DIR}/Release/${PLUGIN_BUNDLE_NAME}"
        return 0
    fi
    if [ -d "${MEDIACORE_DIR}/${PLUGIN_BUNDLE_NAME}" ]; then
        echo "${MEDIACORE_DIR}/${PLUGIN_BUNDLE_NAME}"
        return 0
    fi
    return 1
}

# ---------------------------------------------------------------------------
# [2] バージョン番号の自動抽出 (AIUpscale.h の AI_UPSCALE_MAJOR/MINOR_VERSION)
# ---------------------------------------------------------------------------
extract_version() {
    local header="${PLUGIN_DIR}/src/plugin/AIUpscale.h"
    if [ ! -f "${header}" ]; then
        log_error "バージョン定義ファイルが見つかりません: ${header}"
        return 1
    fi

    local major minor
    major="$(grep -E '^\s*#define\s+AI_UPSCALE_MAJOR_VERSION' "${header}" | awk '{print $3}')"
    minor="$(grep -E '^\s*#define\s+AI_UPSCALE_MINOR_VERSION' "${header}" | awk '{print $3}')"

    if [ -z "${major}" ] || [ -z "${minor}" ]; then
        log_error "AI_UPSCALE_MAJOR_VERSION / AI_UPSCALE_MINOR_VERSION を ${header} から抽出できませんでした。"
        return 1
    fi

    echo "${major}.${minor}"
}

# ---------------------------------------------------------------------------
# [3] インストーラ (インストールする.command) の生成
# ---------------------------------------------------------------------------
write_installer_command() {
    local dest="$1"

    cat > "${dest}" <<'INSTALLER_EOF'
#!/usr/bin/env bash
#
# インストールする.command
#
# ダブルクリック (または右クリック→開く) で AIUpscale.plugin を
# Premiere Pro / After Effects 用の共通プラグインフォルダ (MediaCore) に
# インストールします。
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN_SRC="${SCRIPT_DIR}/AIUpscale.plugin"
MEDIACORE_DIR="/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore"
PLUGIN_DEST="${MEDIACORE_DIR}/AIUpscale.plugin"

echo "=== AI Upscale プラグイン インストーラー ==="
echo ""

if [ ! -d "${PLUGIN_SRC}" ]; then
    echo "エラー: AIUpscale.plugin がこのスクリプトと同じフォルダに見つかりません。" >&2
    echo "zipを展開したフォルダの中でこのスクリプトを実行してください。" >&2
    read -r -p "Enterキーを押すと終了します..." _
    exit 1
fi

echo "[1/3] ダウンロードしたファイルに付くセキュリティ属性 (隔離属性) を解除します。"
echo "      これはmacOSが「インターネットからダウンロードしたファイル」に自動的に"
echo "      付ける印で、コード署名のないアプリ/プラグインを実行できるようにするため"
echo "      に必要な操作です。"
xattr -dr com.apple.quarantine "${PLUGIN_SRC}" || true

echo ""
echo "[2/3] Premiere Pro / After Effects の共通プラグインフォルダにコピーします:"
echo "      ${MEDIACORE_DIR}"
echo "      このフォルダはシステム共通の場所のため、管理者権限 (パスワード入力)"
echo "      が必要です。"

sudo mkdir -p "${MEDIACORE_DIR}"

if [ -e "${PLUGIN_DEST}" ]; then
    backup="${PLUGIN_DEST}.bak.$(date +%Y%m%d_%H%M%S)"
    echo "      既存のインストールを検出したため、上書き前に退避します: ${backup}"
    sudo mv "${PLUGIN_DEST}" "${backup}"
fi

sudo cp -R "${PLUGIN_SRC}" "${PLUGIN_DEST}"
sudo chmod -R a+rX "${PLUGIN_DEST}"

echo "      インストール完了: ${PLUGIN_DEST}"
echo ""
echo "[3/3] 完了しました。"
echo ""
echo "Premiereを再起動してください。再起動後、エフェクトパネルで"
echo "「AI Upscale」を検索すると使用できます。"
echo ""
read -r -p "Enterキーを押すとこのウィンドウを閉じます..." _
INSTALLER_EOF

    chmod +x "${dest}"
}

# ---------------------------------------------------------------------------
# [4] 手順書 (はじめにお読みください.txt) の生成
# ---------------------------------------------------------------------------
write_readme_txt() {
    local dest="$1"
    local version="$2"

    cat > "${dest}" <<README_EOF
AI Upscale プラグイン (v${version}) - はじめにお読みください
================================================================

■ これは何か
------------------------------------------------------------------
Premiere Pro 用のエフェクトプラグイン「AI Upscale」です。
映像のディテール（輪郭・質感）を保持したままシャープさを強調する
エフェクトで、特にモーション/トランスフォームで拡大 (パンチイン) した
映像の見た目の画質を改善したい場合に有効です。

■ 動作環境
------------------------------------------------------------------
- Apple Silicon Mac (M1 以降)
- macOS 13 以降
- Adobe Premiere Pro (動作確認: 2025)

■ インストール手順
------------------------------------------------------------------

【簡単な方法（おすすめ）】

  1. お渡ししたzipファイルを展開してください（ダブルクリックで自動的に
     展開されます）。
  2. 展開してできたフォルダの中の「インストールする.command」を
     右クリック → 「開く」を選んでください。
       ※ ダブルクリックすると「開発元を確認できないため開けません」と
         表示されることがあります。これは正常です。その場合は必ず
         右クリック（または Control キーを押しながらクリック）から
         「開く」を選んでください。この操作をした場合のみ、初回に
         限り実行が許可されます。
  3. ターミナルのウィンドウが開き、案内に従ってパスワードの入力を
     求められます（Macにログインする際のパスワードです）。
  4. 「インストール完了」と表示されたらEnterキーを押して閉じます。
  5. Premiere Pro を再起動してください。

【手動の方法（上記でうまくいかない場合）】

  ターミナル.app を開き、以下の2行を1行ずつ貼り付けて実行してください
  （<展開先> の部分は、実際にzipを展開したフォルダのパスに置き換えて
  ください。Finderでフォルダをターミナルにドラッグ＆ドロップすると
  パスを自動入力できます）。

    xattr -dr com.apple.quarantine <展開先>/AIUpscale.plugin
    sudo cp -R <展開先>/AIUpscale.plugin "/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/"

  実行後、Premiere Pro を再起動してください。

■ 使い方
------------------------------------------------------------------
  1. Premiere Pro のエフェクトパネルで「AI Upscale」を検索します
     （「AI Enhance」カテゴリに入っています）。
  2. 効果をかけたいクリップに適用します。
  3. 「Detail」スライダー（0〜100、標準値50）で強さを調整します。
  4. モーション/トランスフォームエフェクトで拡大しているクリップに
     適用すると特に効果的です（AI Upscaleをトランスフォームより上に
     配置してください）。

■ アンインストール
------------------------------------------------------------------
以下のフォルダ内の AIUpscale.plugin を削除するだけです。

  /Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/

■ 注意事項
------------------------------------------------------------------
- このプラグインはコード署名を行っていない個人配布版のため、初回起動時に
  macOSのセキュリティ警告が表示されます。上記のインストール手順の通りに
  操作すれば問題なく使用できます。
- 動作に問題があった場合は、このzipを渡した本人（配布者）へご連絡ください。
- 本プラグインは無保証です。自己責任でご使用ください。

README_EOF
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
main() {
    log_info "配布用パッケージを作成します。"

    local bundle_path
    if ! bundle_path="$(find_plugin_bundle)"; then
        log_error "ビルド済みの ${PLUGIN_BUNDLE_NAME} が見つかりません。"
        log_error "検索先: ${BUILD_DIR}/${PLUGIN_BUNDLE_NAME}, ${BUILD_DIR}/Release/${PLUGIN_BUNDLE_NAME}, ${MEDIACORE_DIR}/${PLUGIN_BUNDLE_NAME}"
        log_error "先に 'bash plugin/setup_mac.sh' を実行してビルド・インストールを済ませてください。"
        exit 1
    fi
    log_ok "ビルド済みバンドルを検出しました: ${bundle_path}"

    local version
    if ! version="$(extract_version)"; then
        exit 1
    fi
    log_ok "バージョン: ${version}"

    local date_str
    date_str="$(date +%Y%m%d)"

    local zip_name="AIUpscale_v${version}_${date_str}.zip"
    local zip_path="${DOWNLOADS_DIR}/${zip_name}"

    mkdir -p "${DOWNLOADS_DIR}"

    STAGING_DIR="$(mktemp -d)"
    local package_dir="${STAGING_DIR}/AIUpscale_v${version}"
    mkdir -p "${package_dir}"

    log_info "ステージングディレクトリを準備しています: ${package_dir}"

    cp -R "${bundle_path}" "${package_dir}/${PLUGIN_BUNDLE_NAME}"
    log_ok "AIUpscale.plugin をコピーしました。"

    write_installer_command "${package_dir}/インストールする.command"
    log_ok "インストールする.command を作成しました。"

    write_readme_txt "${package_dir}/はじめにお読みください.txt" "${version}"
    log_ok "はじめにお読みください.txt を作成しました。"

    if [ -e "${zip_path}" ]; then
        rm -f "${zip_path}"
    fi

    # zip はエントリごとの実行権限 (chmod +x した .command を含む) を
    # 保持するため、ditto ではなく zip -r を使う。
    ( cd "${STAGING_DIR}" && zip -r -q "${zip_path}" "$(basename "${package_dir}")" )
    log_ok "zipを作成しました: ${zip_path}"

    log_info ""
    log_info "完了しました: ${zip_path}"
    log_info "AirDropやクラウドストレージ (iCloud Drive / Google Drive等) で共有先に"
    log_info "送るだけでOKです。"
}

main "$@"
