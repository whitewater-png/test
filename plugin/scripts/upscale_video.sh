#!/usr/bin/env bash
#
# upscale_video.sh - 動画素材をPremiere編集"前"にAIで事前アップスケールする
#                     一括処理ツール。
#
# 背景: Premiere上で4K素材をトランスフォームでパンチイン(拡大)すると画質が
# 荒れる。AIUpscaleプラグインをリアルタイム適用すると重い上に、実機検証の
# 結果このプラグインは常に「入力と同解像度」で動作する設計になっている
# (plugin/README.md の「使い方ガイド」参照)。そのため、素材そのものを
# 編集前に事前に高解像度化しておき、その高解像度素材をタイムラインに配置して
# トランスフォームで拡大する方式のほうが、リアルタイム処理より高品質かつ
# 軽量・確実である。本スクリプトはその「事前アップスケール」を、既にビルド
# 済みの upscale_cli (plugin/src/cli/upscale_cli.cpp, ONNX Runtime / CoreML
# 実行プロバイダ) を使ってフレーム単位で行い、元動画の音声・fps・
# タイムコードを保持して再結合する。
#
# ルートの upscale.py (realesrgan-ncnn-vulkan / ffmpegフィルタ) や
# plugin本体のリアルタイムプラグインとは別の、独立したCLIツール。
#
# 使い方:
#   bash plugin/scripts/upscale_video.sh <input.mov> [options]
#
# オプション:
#   -o, --output <path>     出力パス (既定: <入力名>_upscaled.mov)
#   --engine {ai,detail}    処理エンジン (既定 ai)
#   --scale {2,4}           倍率 (既定 4)
#   --mode {photo,anime}    使用モデル (既定 photo、--engine ai のみ)
#   --detail N              Detail Preserveエンジンの強度 0-100 (既定 50、
#                           --engine detail のみ)
#   --codec {prores,h264}   出力コーデック (既定 prores)
#   --jobs N                upscale_cli に渡す並列度 (既定: upscale_cliのauto)
#   --model-dir <dir>       モデルディレクトリ (既定: 自動検出、--engine ai のみ)
#   --upscale-cli <path>    upscale_cliバイナリのパス (既定: 自動検出)
#   -h, --help              このヘルプを表示
#
# macOS (Apple Silicon) を主要ターゲットとしているが、Linux上でも
# (upscale_cliがビルドされていれば) 同様に動作するように書かれている。
#
set -euo pipefail

# ---------------------------------------------------------------------------
# 基本設定
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

TIMESTAMP="$(date +%Y%m%d_%H%M%S)"

# 既定値
OUTPUT=""
ENGINE="ai"
SCALE=4
MODE="photo"
DETAIL=""
CODEC="prores"
JOBS=""
MODEL_DIR=""
UPSCALE_CLI=""
INPUT=""

TMP_DIR=""
LOG_FILE=""

# ---------------------------------------------------------------------------
# ログ用関数 (setup_mac.sh と同じスタイル)
# ---------------------------------------------------------------------------
log_info() {
    printf '[INFO] %s\n' "$*"
}

log_warn() {
    printf '[WARN] %s\n' "$*" >&2
}

log_error() {
    printf '[ERROR] %s\n' "$*" >&2
}

print_help() {
    cat <<'EOF'
使い方: bash plugin/scripts/upscale_video.sh <input.mov> [オプション]

Premiereでの編集前に、動画素材をフレーム単位でAIアップスケールし、
元の音声・fps・タイムコードを保持したまま再結合します。

オプション:
  -o, --output <path>     出力パス (既定: <入力名>_upscaled.mov)
  --engine {ai,detail}    処理エンジン (既定: ai)
                          ai     -> Real-ESRGAN (ONNX Runtime)。最高画質だが
                                    低速 (実写/アニメのMode選択可)。
                          detail -> 古典的エッジ保持アップスケール（AI Upscale
                                    プラグインの既定エンジンと同じ独自実装、
                                    detail_upscaler.h参照）。モデル不要で
                                    高速。--mode は無視される。
  --scale {2,4}           倍率 (既定: 4)。
                          --engine ai: モデルはネイティブ4x。--scale 2の場合は
                          upscale_cli の --scale 2 (ネイティブ4x処理後に2xへ
                          ダウンサンプル) を使う。
                          --engine detail: 指定倍率でLanczosベース拡大 +
                          ディテール復元を行う。
  --mode {photo,anime}    使用モデル (既定: photo、--engine ai のみ)。
                          photo -> models/realesrgan-x4plus.onnx
                          anime -> models/realesrgan-x4plus-anime.onnx
  --detail N              Detail Preserveエンジンの強度 0-100 (既定: 50、
                          --engine detail のみ。0=Lanczosのみ、100=最大強度)
  --codec {prores,h264}   出力コーデック (既定: prores = ProRes 422 HQ、
                          編集用途向け。h264も選択可)
  --jobs N                upscale_cli に渡す並列度 (既定: upscale_cliのauto。
                          --engine detail は常に単一スレッドのため無視される)
  --model-dir <dir>       モデルディレクトリ (既定: インストール先の
                          MediaCore/models があればそれ、無ければ
                          plugin/models。--engine ai のみ使用)
  --upscale-cli <path>    upscale_cli バイナリのパス
                          (既定: plugin/build/upscale_cli、無ければPATH探索)
  -h, --help              このヘルプを表示

例:
  bash plugin/scripts/upscale_video.sh cam3sideA.mov --scale 4 --codec prores
  bash plugin/scripts/upscale_video.sh cam3sideA.mov --engine detail --scale 4 --detail 75
EOF
}

# ---------------------------------------------------------------------------
# 引数パース
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        -o|--output)
            [[ $# -ge 2 ]] || { log_error "$1 には値が必要です"; exit 1; }
            OUTPUT="$2"
            shift 2
            ;;
        --engine)
            [[ $# -ge 2 ]] || { log_error "$1 には値が必要です"; exit 1; }
            ENGINE="$2"
            shift 2
            ;;
        --scale)
            [[ $# -ge 2 ]] || { log_error "$1 には値が必要です"; exit 1; }
            SCALE="$2"
            shift 2
            ;;
        --mode)
            [[ $# -ge 2 ]] || { log_error "$1 には値が必要です"; exit 1; }
            MODE="$2"
            shift 2
            ;;
        --detail)
            [[ $# -ge 2 ]] || { log_error "$1 には値が必要です"; exit 1; }
            DETAIL="$2"
            shift 2
            ;;
        --codec)
            [[ $# -ge 2 ]] || { log_error "$1 には値が必要です"; exit 1; }
            CODEC="$2"
            shift 2
            ;;
        --jobs)
            [[ $# -ge 2 ]] || { log_error "$1 には値が必要です"; exit 1; }
            JOBS="$2"
            shift 2
            ;;
        --model-dir)
            [[ $# -ge 2 ]] || { log_error "$1 には値が必要です"; exit 1; }
            MODEL_DIR="$2"
            shift 2
            ;;
        --upscale-cli)
            [[ $# -ge 2 ]] || { log_error "$1 には値が必要です"; exit 1; }
            UPSCALE_CLI="$2"
            shift 2
            ;;
        -h|--help)
            print_help
            exit 0
            ;;
        -*)
            log_error "不明なオプション: $1"
            print_help
            exit 1
            ;;
        *)
            if [[ -z "${INPUT}" ]]; then
                INPUT="$1"
            else
                log_error "入力ファイルは1つだけ指定してください (既に '${INPUT}' を指定済み、'$1' は不要です)"
                exit 1
            fi
            shift
            ;;
    esac
done

if [[ -z "${INPUT}" ]]; then
    log_error "入力動画ファイルを指定してください。"
    print_help
    exit 1
fi

case "${ENGINE}" in
    ai|detail) ;;
    *) log_error "--engine は ai または detail を指定してください (指定値: ${ENGINE})"; exit 1 ;;
esac

case "${SCALE}" in
    2|4) ;;
    *) log_error "--scale は 2 または 4 を指定してください (指定値: ${SCALE})"; exit 1 ;;
esac

case "${MODE}" in
    photo|anime) ;;
    *) log_error "--mode は photo または anime を指定してください (指定値: ${MODE})"; exit 1 ;;
esac

if [[ -n "${DETAIL}" ]]; then
    if ! [[ "${DETAIL}" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
        log_error "--detail は 0 から 100 の数値を指定してください (指定値: ${DETAIL})"
        exit 1
    fi
    if (( $(awk -v d="${DETAIL}" 'BEGIN { print (d < 0 || d > 100) }') )); then
        log_error "--detail は 0 から 100 の範囲で指定してください (指定値: ${DETAIL})"
        exit 1
    fi
fi

case "${CODEC}" in
    prores|h264) ;;
    *) log_error "--codec は prores または h264 を指定してください (指定値: ${CODEC})"; exit 1 ;;
esac

if [[ -n "${JOBS}" ]]; then
    if ! [[ "${JOBS}" =~ ^[0-9]+$ ]]; then
        log_error "--jobs は非負整数を指定してください (指定値: ${JOBS})"
        exit 1
    fi
fi

if [[ ! -f "${INPUT}" ]]; then
    log_error "入力ファイルが見つかりません: ${INPUT}"
    exit 1
fi

if [[ -z "${OUTPUT}" ]]; then
    INPUT_DIR="$(cd "$(dirname "${INPUT}")" && pwd)"
    INPUT_BASE="$(basename "${INPUT}")"
    INPUT_STEM="${INPUT_BASE%.*}"
    OUTPUT="${INPUT_DIR}/${INPUT_STEM}_upscaled.mov"
fi

OUTPUT_DIR="$(cd "$(dirname "${OUTPUT}")" && pwd)"
OUTPUT="${OUTPUT_DIR}/$(basename "${OUTPUT}")"

LOG_FILE="${OUTPUT_DIR}/upscale_video_log_${TIMESTAMP}.log"

# 以降の全出力をログファイルにも記録する。
exec > >(tee -a "${LOG_FILE}") 2>&1

log_info "ログファイル: ${LOG_FILE}"

# ---------------------------------------------------------------------------
# 一時ディレクトリ・クリーンアップ
# ---------------------------------------------------------------------------
# shellcheck disable=SC2317  # trap経由でのみ呼ばれるため誤検知 (shellcheck wiki記載の既知の制約)
cleanup() {
    local exit_code=$?
    if [[ -n "${TMP_DIR}" && -d "${TMP_DIR}" ]]; then
        log_info "一時ディレクトリを削除しています: ${TMP_DIR}"
        rm -rf "${TMP_DIR}"
    fi
    if [[ ${exit_code} -ne 0 ]]; then
        log_error "処理が失敗しました (終了コード ${exit_code})。ログ: ${LOG_FILE}"
    fi
    exit "${exit_code}"
}
trap cleanup EXIT INT TERM

# shellcheck disable=SC2317  # trap経由でのみ呼ばれるため誤検知
on_error() {
    local line_no=$1
    log_error "予期しないエラーが発生しました (行 ${line_no})。"
}
trap 'on_error ${LINENO}' ERR

# ---------------------------------------------------------------------------
# 前提チェック
# ---------------------------------------------------------------------------
log_info "=== 前提チェック ==="

MISSING_TOOLS=()
for tool in ffmpeg ffprobe; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        MISSING_TOOLS+=("${tool}")
    fi
done
if [[ ${#MISSING_TOOLS[@]} -gt 0 ]]; then
    log_error "必要なコマンドが見つかりません: ${MISSING_TOOLS[*]}"
    log_error "macOS: brew install ffmpeg  /  Ubuntu: sudo apt-get install -y ffmpeg"
    exit 1
fi

# upscale_cli の解決
if [[ -z "${UPSCALE_CLI}" ]]; then
    if [[ -x "${PLUGIN_DIR}/build/upscale_cli" ]]; then
        UPSCALE_CLI="${PLUGIN_DIR}/build/upscale_cli"
    elif command -v upscale_cli >/dev/null 2>&1; then
        UPSCALE_CLI="$(command -v upscale_cli)"
    else
        log_error "upscale_cli バイナリが見つかりません。"
        log_error "先に 'bash plugin/setup_mac.sh' を実行してビルドしてください"
        log_error "(または --upscale-cli でパスを明示指定してください)。"
        exit 1
    fi
fi
if [[ ! -x "${UPSCALE_CLI}" ]]; then
    log_error "upscale_cli が実行可能ではありません: ${UPSCALE_CLI}"
    log_error "先に 'bash plugin/setup_mac.sh' を実行してビルドしてください。"
    exit 1
fi
log_info "upscale_cli: ${UPSCALE_CLI}"

# モデルディレクトリの解決 (--engine ai のみ必要。--engine detail はモデル
# 不要な古典的アルゴリズム (plugin/src/core/detail_upscaler.h) を使うため、
# モデルファイルの存在確認自体をスキップする)
if [[ "${ENGINE}" == "ai" ]]; then
    if [[ -z "${MODEL_DIR}" ]]; then
        INSTALLED_MODELS="/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/models"
        if [[ -d "${INSTALLED_MODELS}" ]]; then
            MODEL_DIR="${INSTALLED_MODELS}"
        else
            MODEL_DIR="${PLUGIN_DIR}/models"
        fi
    fi
    log_info "モデルディレクトリ: ${MODEL_DIR}"

    case "${MODE}" in
        photo) MODEL_FILE="${MODEL_DIR}/realesrgan-x4plus.onnx" ;;
        anime) MODEL_FILE="${MODEL_DIR}/realesrgan-x4plus-anime.onnx" ;;
    esac

    if [[ ! -f "${MODEL_FILE}" ]]; then
        log_error "モデルファイルが見つかりません: ${MODEL_FILE}"
        log_error "先に 'python3 plugin/scripts/download_models.py --out-dir ${MODEL_DIR}' を実行するか、"
        log_error "'bash plugin/setup_mac.sh' でセットアップを完了してください。"
        exit 1
    fi
    log_info "モデル: ${MODEL_FILE} (mode=${MODE})"
else
    # --engine detail: upscale_cli はこのモード時にモデル引数を開かないため、
    # プレースホルダを渡すだけでよい (plugin/src/cli/upscale_cli.cpp 参照)。
    MODEL_FILE="-"
    log_info "エンジン: detail (古典的エッジ保持アップスケール、モデル不要、detail=${DETAIL:-50})"
fi

log_info "入力ファイル: ${INPUT}"
log_info "出力ファイル: ${OUTPUT}"
log_info "engine=${ENGINE} scale=${SCALE} codec=${CODEC} jobs=${JOBS:-auto}"

# ---------------------------------------------------------------------------
# ffprobe で動画情報を取得
# ---------------------------------------------------------------------------
log_info "=== 入力動画の解析 ==="

VIDEO_INFO="$(ffprobe -v error -select_streams v:0 \
    -show_entries stream=r_frame_rate,width,height \
    -of csv=p=0 "${INPUT}" | head -n1)"

if [[ -z "${VIDEO_INFO}" ]]; then
    log_error "入力ファイルから映像ストリーム情報を取得できませんでした: ${INPUT}"
    exit 1
fi

IN_WIDTH="$(echo "${VIDEO_INFO}" | cut -d, -f1)"
IN_HEIGHT="$(echo "${VIDEO_INFO}" | cut -d, -f2)"
FPS_RAW="$(echo "${VIDEO_INFO}" | cut -d, -f3)"

# fps を小数表示用に計算 (ログ表示・目安計算専用。実際の -r には FPS_RAW
# (例: "24000/1001") をそのまま渡し、丸め誤差を避ける)
if [[ "${FPS_RAW}" == *"/"* ]]; then
    FPS_NUM="${FPS_RAW%%/*}"
    FPS_DEN="${FPS_RAW##*/}"
    FPS_DECIMAL="$(awk -v n="${FPS_NUM}" -v d="${FPS_DEN}" 'BEGIN { if (d == 0) { print n } else { printf "%.3f", n/d } }')"
else
    FPS_DECIMAL="${FPS_RAW}"
fi

HAS_AUDIO=0
AUDIO_CHECK="$(ffprobe -v error -select_streams a -show_entries stream=index -of csv=p=0 "${INPUT}" || true)"
if [[ -n "${AUDIO_CHECK}" ]]; then
    HAS_AUDIO=1
fi

# 総フレーム数の取得 (コンテナメタデータ優先、無ければ実カウント)
TOTAL_FRAMES="$(ffprobe -v error -select_streams v:0 -show_entries stream=nb_frames -of csv=p=0 "${INPUT}" 2>/dev/null || true)"
if [[ -z "${TOTAL_FRAMES}" || "${TOTAL_FRAMES}" == "N/A" ]]; then
    log_info "コンテナにフレーム数メタデータが無いため実カウントします (少し時間がかかります)..."
    TOTAL_FRAMES="$(ffprobe -v error -select_streams v:0 -count_frames \
        -show_entries stream=nb_read_frames -of csv=p=0 "${INPUT}" 2>/dev/null || true)"
fi
if [[ -z "${TOTAL_FRAMES}" || "${TOTAL_FRAMES}" == "N/A" ]]; then
    log_warn "総フレーム数を取得できませんでした。進捗表示は概数になります。"
    TOTAL_FRAMES=0
fi

OUT_WIDTH=$(( IN_WIDTH * SCALE ))
OUT_HEIGHT=$(( IN_HEIGHT * SCALE ))

log_info "入力解像度: ${IN_WIDTH}x${IN_HEIGHT}"
log_info "出力解像度(推定): ${OUT_WIDTH}x${OUT_HEIGHT} (${SCALE}x)"
log_info "fps: ${FPS_RAW} (約${FPS_DECIMAL})"
log_info "音声トラック: $( [[ ${HAS_AUDIO} -eq 1 ]] && echo あり || echo なし )"
log_info "総フレーム数: ${TOTAL_FRAMES}"

# ---------------------------------------------------------------------------
# 一時ディレクトリ準備・ディスク容量チェック
# ---------------------------------------------------------------------------
BASE_TMP="${TMPDIR:-/tmp}"
TMP_DIR="$(mktemp -d "${BASE_TMP%/}/upscale_video.XXXXXX")"
FRAMES_DIR="${TMP_DIR}/frames"
UPSCALED_DIR="${TMP_DIR}/upscaled"
mkdir -p "${FRAMES_DIR}" "${UPSCALED_DIR}"

log_warn "4K以上の素材をPNG連番で抽出するとディスク使用量が非常に大きくなります"
log_warn "(4x アップスケールの場合、フレーム1枚あたり数十MBになることがあります)。"
log_warn "一時ディレクトリ: ${TMP_DIR} (処理完了後に自動削除されます)"

if [[ "${TOTAL_FRAMES}" -gt 0 ]]; then
    # 大まかな見積もり: 元解像度PNG (非圧縮の約30%と仮定) + アップスケール後PNG
    # (解像度は scale^2 倍)。安全側に1.5倍のマージンを掛ける。
    SCALE_SQ=$(( SCALE * SCALE ))
    EST_IN_BYTES_PER_FRAME=$(( IN_WIDTH * IN_HEIGHT * 3 * 30 / 100 ))
    EST_OUT_BYTES_PER_FRAME=$(( EST_IN_BYTES_PER_FRAME * SCALE_SQ ))
    EST_TOTAL_BYTES=$(( (EST_IN_BYTES_PER_FRAME + EST_OUT_BYTES_PER_FRAME) * TOTAL_FRAMES * 3 / 2 ))
    EST_TOTAL_MB=$(( EST_TOTAL_BYTES / 1024 / 1024 ))

    AVAIL_KB="$(df -Pk "${TMP_DIR}" | awk 'NR==2 {print $4}')"
    AVAIL_MB=$(( AVAIL_KB / 1024 ))

    log_info "推定必要ディスク容量: 約${EST_TOTAL_MB}MB / 空き容量: 約${AVAIL_MB}MB (${TMP_DIR}が存在するファイルシステム)"

    if [[ "${AVAIL_MB}" -lt "${EST_TOTAL_MB}" ]]; then
        log_error "ディスクの空き容量が不足している可能性があります"
        log_error "(推定必要量 約${EST_TOTAL_MB}MB > 空き容量 約${AVAIL_MB}MB)。"
        log_error "空き容量を確保するか、TMPDIR環境変数で別のディスクを指定してから再実行してください。"
        exit 1
    fi
fi

# ---------------------------------------------------------------------------
# フレーム抽出
# ---------------------------------------------------------------------------
log_info "=== [1/3] フレームを抽出しています ==="
ffmpeg -y -v error -i "${INPUT}" -vsync 0 -qscale:v 1 "${FRAMES_DIR}/frame_%08d.png"

FRAME_LIST=("${FRAMES_DIR}"/frame_*.png)
NUM_EXTRACTED=${#FRAME_LIST[@]}
if [[ ${NUM_EXTRACTED} -eq 0 || ! -f "${FRAME_LIST[0]}" ]]; then
    log_error "フレームの抽出に失敗しました (フレームが1枚も生成されませんでした)。"
    exit 1
fi
log_info "抽出フレーム数: ${NUM_EXTRACTED}"
TOTAL_FRAMES=${NUM_EXTRACTED}

# ---------------------------------------------------------------------------
# フレーム単位アップスケール
# ---------------------------------------------------------------------------
if [[ "${ENGINE}" == "ai" ]]; then
    log_info "=== [2/3] AIアップスケールしています (engine=ai, mode=${MODE}, scale=${SCALE}) ==="
else
    log_info "=== [2/3] アップスケールしています (engine=detail, scale=${SCALE}, detail=${DETAIL:-50}) ==="
fi

CLI_ARGS=(--engine "${ENGINE}")
if [[ "${SCALE}" -ne 4 ]]; then
    CLI_ARGS+=(--scale "${SCALE}")
fi
if [[ -n "${JOBS}" ]]; then
    CLI_ARGS+=(--jobs "${JOBS}")
fi
if [[ "${ENGINE}" == "detail" && -n "${DETAIL}" ]]; then
    CLI_ARGS+=(--detail "${DETAIL}")
fi

FRAME_IDX=0
ELAPSED_FIRST=""
START_ALL="$(date +%s)"

for frame_path in "${FRAME_LIST[@]}"; do
    FRAME_IDX=$(( FRAME_IDX + 1 ))
    frame_name="$(basename "${frame_path}")"
    out_path="${UPSCALED_DIR}/${frame_name}"

    frame_t0="$(date +%s.%N 2>/dev/null || date +%s)"
    if ! "${UPSCALE_CLI}" "${MODEL_FILE}" "${frame_path}" "${out_path}" "${CLI_ARGS[@]}" \
            >>"${TMP_DIR}/upscale_cli.log" 2>&1; then
        log_error ""
        log_error "upscale_cli がフレーム ${frame_name} (${FRAME_IDX}/${TOTAL_FRAMES}) の処理に失敗しました。"
        log_error "詳細: ${TMP_DIR}/upscale_cli.log (このログはcleanup前の内容を以下に転記します)"
        tail -n 40 "${TMP_DIR}/upscale_cli.log" >&2 || true
        exit 1
    fi
    frame_t1="$(date +%s.%N 2>/dev/null || date +%s)"

    if [[ ${FRAME_IDX} -eq 1 ]]; then
        ELAPSED_FIRST="$(awk -v a="${frame_t0}" -v b="${frame_t1}" 'BEGIN { printf "%.3f", b-a }')"
        if [[ "${TOTAL_FRAMES}" -gt 1 ]]; then
            EST_REMAIN="$(awk -v e="${ELAPSED_FIRST}" -v n="${TOTAL_FRAMES}" 'BEGIN { printf "%.0f", e*(n-1) }')"
            log_info "1フレーム目の処理時間: ${ELAPSED_FIRST}s -> 残り${TOTAL_FRAMES}フレームの推定所要時間: 約${EST_REMAIN}秒"
        fi
    fi

    if [[ ${FRAME_IDX} -eq ${TOTAL_FRAMES} ]]; then
        printf '\r  進捗: %d/%d 完了\n' "${FRAME_IDX}" "${TOTAL_FRAMES}"
    elif (( FRAME_IDX % 10 == 0 )) || [[ ${FRAME_IDX} -eq 1 ]]; then
        printf '\r  進捗: %d/%d' "${FRAME_IDX}" "${TOTAL_FRAMES}"
    fi
done
echo ""

END_ALL="$(date +%s)"

# ---------------------------------------------------------------------------
# 動画再結合
# ---------------------------------------------------------------------------
log_info "=== [3/3] 動画に再結合しています (codec=${CODEC}) ==="

ENCODE_CMD=(ffmpeg -y -v error -framerate "${FPS_RAW}" -i "${UPSCALED_DIR}/frame_%08d.png")
if [[ ${HAS_AUDIO} -eq 1 ]]; then
    ENCODE_CMD+=(-i "${INPUT}")
fi
ENCODE_CMD+=(-map 0:v:0)
if [[ ${HAS_AUDIO} -eq 1 ]]; then
    ENCODE_CMD+=(-map 1:a:0)
fi

if [[ "${CODEC}" == "prores" ]]; then
    ENCODE_CMD+=(-c:v prores_ks -profile:v 3 -pix_fmt yuv422p10le)
else
    ENCODE_CMD+=(-c:v libx264 -crf 16 -pix_fmt yuv420p)
fi
ENCODE_CMD+=(-r "${FPS_RAW}")

if [[ ${HAS_AUDIO} -eq 1 ]]; then
    ENCODE_CMD+=(-c:a copy -shortest)
fi
ENCODE_CMD+=("${OUTPUT}")

if ! "${ENCODE_CMD[@]}"; then
    log_error "動画の再結合に失敗しました。"
    if [[ "${CODEC}" == "prores" ]]; then
        log_error "ffmpegがprores_ksをサポートしていない可能性があります。--codec h264 を試してください。"
    fi
    exit 1
fi

if [[ ! -f "${OUTPUT}" ]]; then
    log_error "出力ファイルが生成されませんでした: ${OUTPUT}"
    exit 1
fi

# ---------------------------------------------------------------------------
# 完了報告
# ---------------------------------------------------------------------------
TOTAL_ELAPSED=$(( END_ALL - START_ALL ))
FINAL_INFO="$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "${OUTPUT}" | head -n1)"
FINAL_WIDTH="$(echo "${FINAL_INFO}" | cut -d, -f1)"
FINAL_HEIGHT="$(echo "${FINAL_INFO}" | cut -d, -f2)"

log_info ""
log_info "=== 完了 ==="
log_info "出力ファイル: ${OUTPUT}"
log_info "最終解像度: ${FINAL_WIDTH}x${FINAL_HEIGHT}"
log_info "所要時間(フレーム処理+再結合、抽出は除く概算): 約${TOTAL_ELAPSED}秒"
log_info ""
log_info "Premiereでの使い方: この高解像度クリップをタイムラインに配置し、"
log_info "モーション/トランスフォームエフェクトの「スケール」を100%未満(縮小気味)"
log_info "で使うと、パンチイン(拡大)しても高精細な見た目になります。"

exit 0
