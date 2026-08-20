# macOS 空間オーディオ (CoreAudio レンダラー) 実装メモ

> 対象ブランチ: `feature/macos-spatial-audio`
> コミット: `8559e103 feat(audio): add macOS CoreAudio renderer with spatial audio`
> 移植元: 上流 PR [moonlight-stream/moonlight-qt#1399](https://github.com/moonlight-stream/moonlight-qt/pull/1399)

## TL;DR

macOS で AirPods 等に空間オーディオ出力するため、SDL に代わる **CoreAudio ネイティブレンダラー**を
上流 PR #1399 から移植した。サラウンドストリームは `AUSpatialMixer` を通り、ヘッドホン・内蔵スピーカー・
チャンネル数が足りない出力デバイスではバイノーラル/空間レンダリングされる。

移植は**オーディオ関連部分のみ**。PR ブランチは本フォークより古く、丸ごと適用すると DualSense ハプティクス、
ゲームパッド quit コンボ設定、オーバーレイのリッチテキスト描画が巻き戻る。

上流コードには**実際に音を壊す不具合が3件**あり、移植時に修正した。7.1.4 (12ch) はチャンネル順の
不一致があり、リマップを追加した。

---

## 1. 背景

本フォークは Foundation-Sunshine との組み合わせで 7.1.4ch (12ch) のストリーミング要求ができる。
しかし移植前の moonlight-qt には**それを再生する手段が存在しなかった**（詳細は §6）。

macOS 側の目標は2つ:

1. AirPods / MacBook 内蔵スピーカーでサラウンドを空間オーディオとして再生する
2. 12ch ストリームを実際に受け取れるレンダラーを用意する

## 2. 全体構成

```
moonlight-common-c            moonlight-qt
─────────────────             ────────────────────────────────────────────────
RTSP DESCRIBE
  surround-params 解析
  → OPUS_MULTISTREAM_CONFIGURATION
                              Session::arInit()
                                └ initializeAudioRenderer()
                                    ├ createAudioRenderer()   ← レンダラー選択
                                    └ opus_multistream_decoder_create()
Opus パケット
  → arDecodeAndPlaySample()   getAudioBuffer() → リングバッファへ直接デコード
                              submitAudio()    → 書き込みポインタを進める
                                                 ↓
                              CoreAudioRenderer
                                ├ m_RingBuffer (TPCircularBuffer, インターリーブ float)
                                ├ renderCallbackDirect   … パススルー
                                └ renderCallbackSpatial  … AUSpatialRenderer 経由
                                     └ inputCallback: de-interleave + チャンネルリマップ
                                     └ AUSpatialMixer → ステレオ出力
```

レンダラー選択順 (`app/streaming/audio/audio.cpp`):

1. 環境変数 `ML_AUDIO` による明示指定 (`sdl` / `slaudio` / `coreaudio`)
2. `HAVE_SLAUDIO` (Steam Link)
3. `HAVE_COREAUDIO` (macOS) ← 今回追加
4. SDL (フォールバック)

## 3. 上流 PR #1399 からの移植範囲

### 取り込んだもの

| ファイル | 内容 |
|---|---|
| `audio/renderers/coreaudio/*` | CoreAudio レンダラー本体、`AUSpatialRenderer`、`TPCircularBuffer` |
| `audio/renderers/renderer.{h,cpp}` | `AUDIO_STATS` 統計フレームワーク、`getCapabilities()`、`getRendererName()` |
| `audio/renderers/sdl.{h,cpp}`, `slaud.{h,cpp}` | 統計フックと `getCapabilities()` の実装 |
| `audio/audio.cpp` | CoreAudio レンダラー登録、統計収集、`getAudioRendererCapabilities()` の実レンダラー問い合わせ化 |
| `video/overlaymanager.{h,cpp}` | `OverlayDebugAudio` オーバーレイ種別 |
| `app.pro` | `HAVE_COREAUDIO`、Accelerate/AudioToolbox フレームワーク |

### 取り込まなかったもの

PR ブランチは本フォークより古いため、以下は**適用すると機能が巻き戻る**:

- `input/gamepad.cpp` — DualSense デュアルタッチパッド、設定可能な quit コンボが消える
- `video/overlaymanager.cpp` — リッチテキスト（`**bold**`, `{18}` 等）描画とアライメント指定が消える
- `video/decoder.h`, `video/ffmpeg.cpp`, `pacer/` — 映像統計の刷新。本フォークは独自の統計系を持つ
- `Info.plist` — Game Mode、マイク、Bonjour の記述が消える
- `scripts/generate-dmg.sh` — アーキテクチャ判定、LTO、clipboard helper、File Provider 拡張が消える

これらのファイルからは**必要な数行だけ**を手で取り込んだ。

### 統計オーバーレイの配置

`OverlayDebugAudio` は新規のオーバーレイ種別なので、既存の全レンダラーに配置処理を追加した
（上流 PR は触ったレンダラーにしか入れていない）。

- 映像統計は従来どおり上中央、オーディオ統計は**右上**
- `eglvid.cpp` は未知のオーバーレイ種別で `SDL_assert(false)` に落ちる作りだったため、これも修正

## 4. 実装の要点

### 4.1 空間 / パススルーの判定

`CoreAudioRenderer::prepareForPlayback()`:

```cpp
if (opusConfig->channelCount > 2) {
    if (outputType != kSpatialMixerOutputType_ExternalSpeakers ||
            m_OutputASBD.mChannelsPerFrame < (uint32_t)opusConfig->channelCount) {
        m_Spatial = true;
    }
}
if (prefs->spatialAudioConfig == StreamingPreferences::SAC_DISABLED) {
    m_Spatial = false;
}
```

後半の条件（デバイスのチャンネル数 < ストリームのチャンネル数）は**本フォークで追加**。
2ch の USB DAC は `ExternalSpeakers` を名乗るため、上流の条件だけでは 6/8/12ch の
パススルーに落ちてフォーマット設定に失敗する。

### 4.2 リングバッファ

```
packetsToBuffer = max(2, ceil(0.030 / packetDuration))   // 5ms パケットなら 6
size = sizeof(float) * channelCount * samplesPerFrame * packetsToBuffer
```

`TPCircularBuffer` は仮想メモリのミラーマッピングを使うため、実サイズはページ境界に切り上がる。
Opus デコーダは `getAudioBuffer()` が返すリングバッファの書き込み位置に**直接デコード**する
（中間バッファなし）。

### 4.3 2つのレンダーコールバック

- **`renderCallbackDirect`** — リングバッファから出力バッファへ `memcpy`。インターリーブのまま
- **`renderCallbackSpatial`** — `AUSpatialMixer` を `AudioUnitRender()` で駆動し、
  そのステレオ出力を出力バッファへコピー。ミキサーへの入力は `inputCallback` が
  リングバッファから de-interleave して供給する

### 4.4 AUSpatialMixer の設定

```
kAudioUnitProperty_SpatializationAlgorithm   = kSpatializationAlgorithm_UseOutputType
kAudioUnitProperty_SpatialMixerSourceMode    = kSpatialMixerSourceMode_AmbienceBed
kAudioUnitProperty_SpatialMixerOutputType    = Headphones / BuiltInSpeakers / ExternalSpeakers
kAudioUnitProperty_MaximumFramesPerSlice     = 4096
kAudioUnitProperty_SpatialMixerPersonalizedHRTFMode = Auto   (macOS 13+)
kAudioUnitProperty_SpatialMixerEnableHeadTracking   = 設定次第 (macOS 13+)
```

`AmbienceBed` は入力チャンネルを遠方音源として listener の周囲に配置するモード。

出力タイプは `kAudioDevicePropertyTransportType` と `kAudioDevicePropertyDataSource` から判定する。
判定に失敗した場合は `ExternalSpeakers` にフォールバックするが、§4.1 の追加条件があるため
2ch デバイスなら空間オーディオが選ばれる。

## 5. 7.1.4 (12ch) 対応

### 5.1 チャンネル順の不一致

Sunshine が送るインターリーブ PCM の順序は `platf::speaker::map_surround714` で定義される:

```
FL FR FC LFE BL BR SL SR TFL TFR TBL TBR
```

CoreAudio のレイアウトタグを SDK に問い合わせた実測結果:

| タグ | チャンネル順 | Sunshine と一致? |
|---|---|---|
| `WAVE_5_1_B` | L R C LFE RearSurroundL RearSurroundR | ○ |
| `WAVE_7_1` | L R C LFE RearSurroundL/R LeftSurround/RightSurround | ○ |
| `Atmos_7_1_4` | L R C LFE **LeftSurround RightSurround RearSurroundL RearSurroundR** Vhl Vhr Ltr Rtr | **×** |

`Atmos_7_1_4` だけ**サイドとバックが逆**。5.1 / 7.1 は一致するので問題ない。

### 5.2 リマップ

`AUSpatialRenderer::buildChannelMap()` を追加し、`inputCallback` の de-interleave 時に
読み出し元インデックスをずらす。追加コストは `vDSP_vsadd` の開始オフセットが変わるだけ。

```cpp
if (inChannelCount == 12) {
    // host BL BR SL SR -> mixer Ls Rs Rls Rrs
    m_ChannelMap[4] = 6;
    m_ChannelMap[5] = 7;
    m_ChannelMap[6] = 4;
    m_ChannelMap[7] = 5;
}
```

### 5.3 macOS 側の受け入れ確認

単体テストプログラムで `AUSpatialMixer` が各レイアウトを受理し `AudioUnitInitialize()` まで
通ることを確認済み:

```
== WAVE_5_1_B  (6 ch)  ==  OK, latency=0.10 ms
== WAVE_7_1    (8 ch)  ==  OK, latency=0.10 ms
== Atmos_7_1_4 (12 ch) ==  OK, latency=0.10 ms
```

## 6. 移植前は 7.1.4 を要求できていなかった

設定に 7.1.4 の項目はあったが、**launch リクエスト送出前にステレオへ降格**していた。

バンドルされている SDL2 に対し `sdlaud.cpp` と同じ呼び出し（`allowed_changes = 0`）を実測:

```
SDL 2.32.70, driver=coreaudio
   2 ch -> OK        8 ch -> OK
   4 ch -> OK       10 ch -> FAILED: Unsupported number of audio channels.
   6 ch -> OK       12 ch -> FAILED: Unsupported number of audio channels.
```

`session.cpp:1610` のフォールバック:

```cpp
bool audioTestPassed = testAudio(m_StreamConfig.audioConfiguration);
if (!audioTestPassed && CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(...) > 2) {
    audioTestPassed = testAudio(AUDIO_CONFIGURATION_STEREO);
    if (audioTestPassed) {
        m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
        emitLaunchWarning(tr("Your selected surround sound setting is not supported by the current audio device."));
    }
}
```

つまり **12ch を実際に要求できるようになったのは CoreAudio レンダラーが初めて**。
移植後のログでは `surroundAudioInfo=-163643380` = `(0xF63F << 16) | 12` が送出されている。

## 7. 上流コードで見つけた不具合

いずれも上流 PR #1399 にそのまま存在する。**上流にも報告すべき**（→ Issue/PR 案ドキュメント参照）。

### 7.1 空間オーディオ時に出力フォーマットのチャンネル数が誤り

`coreaudio.cpp` `prepareForPlayback()`。空間経路のレンダーコールバックは**ステレオ**を出力するのに、
AudioUnit へ申告するストリームフォーマットは `mChannelsPerFrame = opusConfig->channelCount` のままだった。
AUHAL はダウンミックスしない（コード中のコメントにも "but not downmixing, hmm" とある）ため、
余剰チャンネルは単に届かない。

```diff
 streamDesc.mFormatFlags     |= kAudioFormatFlagIsNonInterleaved;
+streamDesc.mChannelsPerFrame = 2;
 streamDesc.mBytesPerPacket   = 4;
 streamDesc.mBytesPerFrame    = 4;
```

### 7.2 / 7.3 vDSP の長さ指定が要素数とバイト数の取り違え（2箇所）

`vDSP_vclr()` / `vDSP_mmov()` の長さ引数は **float 何個** であってバイト数ではない。
どちらも `inNumberFrames * 4`（バイト数のつもり）を渡しており、**4倍の範囲**をクリア/コピーしていた。
出力バッファへの書き込みなので、チャンネル構成によっては隣接バッファやヒープを踏む。

```diff
-vDSP_vclr((float *)ioData->mBuffers[i].mData, 1, inNumberFrames * 4);
+vDSP_vclr((float *)ioData->mBuffers[i].mData, 1, inNumberFrames);

-vDSP_mmov(src, dst, 1, inNumberFrames * 4, 1, 1);
+vDSP_mmov(src, dst, 1, inNumberFrames, 1, 1);
```

### 7.4 Atmos_7_1_4 のチャンネル順（§5.1）

上流には `case 12: layout = kAudioChannelLayoutTag_Atmos_7_1_4;` が既にあるが、リマップがない。
上流のまま 12ch を流すとサイド/バックが入れ替わる。

## 8. 本フォーク独自の変更

- §4.1 の空間オーディオ判定条件の追加
- §5.2 のチャンネルリマップ
- 診断ログを Info レベルへ（`DEBUG_TRACE` は Release ビルドで消えるため、判断過程が追えなかった）
  ```
  CoreAudioRenderer is using spatial audio output (8-channel stream, output type 1, device has 2 channels)
  CoreAudioRenderer spatial mixer input: 8 channels, layout tag 0xbd0008
  ```
- 統計表示に 7.1.4 ラベルを追加
- 全レンダラーへの `OverlayDebugAudio` 配置（§3）
- 新規コードのコンパイル警告を解消（`CA_DEBUG_HELPER`、`Q_UNUSED`）

## 9. entitlements

`app/deploy/macos/spatial-audio.entitlements`

- `com.apple.developer.spatial-audio.profile-access` — 個人化 HRTF
- `com.apple.developer.coremotion.head-pose` — ヘッドトラッキング

どちらも**制限付き entitlement** で、署名チームのプロビジョニングプロファイルによる許可が必要。
許可されていない entitlement を付けると起動時拒否や公証失敗を招くため、
`generate-dmg.sh` では `SPATIAL_AUDIO_ENTITLEMENTS=1` を指定したときだけ付与する**オプトイン**にした。

**空間オーディオ自体はこれらなしで動作する。** 実際、ad-hoc 署名・entitlement なしのビルドでも
個人化 HRTF とヘッドトラッキングが有効になることを実機で確認している。

上流のファイルには `com.apple.security.app-sandbox` が含まれていたが**削除した**。
Moonlight は非サンドボックスアプリであり、ネットワーク/ファイルの対応 entitlement なしに
サンドボックス化するとホスト検出とストリーミングが壊れる。上流はこのファイルを追加するだけで
署名には使っていないため、この問題は顕在化していない。

## 10. 検証状況

### 検証済み (7.1 / 8ch, 実ストリーム)

```
Audio stream: 7.1-channel Opus low-delay @ 48 kHz (CoreAudio)
Output device: My AirPods Pro 3 @ 48.0 kHz, 2-channel
Render mode: personalized spatial audio with head-tracking for Bluetooth
Latency: 170.0 ms (network 1 ms, buffers 9.0 ms, hardware: 160.0 ms)
```

§7.1〜7.3 の修正はいずれも 8ch 経路を通るため、これで検証されている。

### 未検証 (7.1.4 / 12ch)

12ch のネゴシエーション・デコーダ生成・空間ミキサー構成までは到達を確認:

```
CoreAudioRenderer is using spatial audio output (12-channel stream, output type 1, device has 2 channels)
CoreAudioRenderer spatial mixer input: 12 channels, layout tag 0xc0000c   (= Atmos_7_1_4)
Audio channel count: 12 / Audio channel mask: F63F
Audio stream has 12 channels
```

しかし**実際の音での確認ができていない**。テストホスト側に 8ch を超えるオーディオエンドポイントが
存在しないため（詳細は `surround-714-ecosystem-status.md`）。

§5.2 のリマップの正しさを確認するには、各チャンネルに識別可能な信号を入れた 12ch ソースを
Linux ホストの 12ch null sink で再生する必要がある。

## 11. ビルドと確認

```bash
# ビルド
cd build/build-Release
qmake ../../moonlight-qt.pro QMAKE_APPLE_DEVICE_ARCHS="arm64"
make -j$(sysctl -n hw.logicalcpu) release

# 配布可能な .app にする
macdeployqt build/build-Release/app/Moonlight.app \
  -executable=build/build-Release/app/Moonlight.app/Contents/MacOS/moonlight-clipboard-helper \
  -qmldir=app/gui -appstore-compliant
bash scripts/build-macos-fileprovider-extension.sh "$PWD" build/build-Release \
  build/build-Release/app/Moonlight.app arm64
codesign --force --deep --sign - build/build-Release/app/Moonlight.app

# DMG まで作る場合（entitlements を付けるなら SPATIAL_AUDIO_ENTITLEMENTS=1）
scripts/generate-dmg.sh Release
```

動作確認は `/tmp/Moonlight-*.log` の以下の行を見る:

- `CoreAudioRenderer is using spatial audio output (...)` / `... passthrough mode (...)`
- `CoreAudioRenderer spatial mixer input: N channels, layout tag 0x...`
- セッション終了時の `Current session audio stats`

ストリーム中は Ctrl+Alt+Shift+S（または設定の統計オーバーレイ）で右上にオーディオ統計が出る。

## 12. 残課題

- 12ch の実音検証（§10）
- `getAudioRendererCapabilities()` が SDP 生成のたびに実レンダラーを生成するため、
  ストリーム開始前にオーディオデバイスの開閉が3回発生する。上流由来の挙動で実害はないが、
  結果をキャッシュする余地がある
- 12ch ストリームを 8ch の HDMI レシーバーへ流す場合、パススルーに失敗して SDL レンダラーへ
  フォールバックする。空間ミキサーに回すとステレオになるため、どちらが望ましいかは要検討
