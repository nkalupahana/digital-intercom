import { AudioStreamingCodecType, AudioStreamingSamplerate, PrepareStreamResponse, SRTPCryptoSuites, StartStreamRequest, StreamRequestTypes, type CameraController, type CameraControllerOptions, type CameraStreamingDelegate, type HAP, type PrepareStreamCallback, type PrepareStreamRequest, type SnapshotRequest, type SnapshotRequestCallback, type StreamingRequest, type StreamRequestCallback } from 'homebridge';
import { ExamplePlatformAccessory } from './platformAccessory.js';
import getPort from 'get-port';

const videomtu = 188 * 5;
const audiomtu = 188 * 1;

type SessionInfo = {
  address: string; // Address of the HAP controller.

  videoPort: number;
  videoReturnPort: number;
  videoCryptoSuite: SRTPCryptoSuites; // This should be saved if multiple suites are supported.
  videoSRTP: Buffer; // Key and salt concatenated.
  videoSSRC: number; // RTP synchronisation source.

  audioPort: number;
  audioReturnPort: number;
  audioCryptoSuite: SRTPCryptoSuites;
  audioSRTP: Buffer;
  audioSSRC: number;
};

export class IntercomStreamingDelegate implements CameraStreamingDelegate {
  private accessory: ExamplePlatformAccessory;
  private hap: HAP;
  private pendingSessions: Record<string, SessionInfo> = {};
  controller: CameraController;

  constructor(paccessory: ExamplePlatformAccessory) {
    this.accessory = paccessory;
    this.hap = paccessory.hap;

    const options: CameraControllerOptions = {
      cameraStreamCount: 2, // HomeKit requires at least 2 streams, and HomeKit Secure Video requires 1.
      delegate: this,
      streamingOptions: {
        supportedCryptoSuites: [this.hap.SRTPCryptoSuites.AES_CM_128_HMAC_SHA1_80],
        video: {
          resolutions: [
            // Width, height, framerate.
            [1920, 1080, 30],
            [1280, 960, 30],
            [1280, 720, 30],
            [1024, 768, 30],
            [640, 480, 30],
            [640, 360, 30],
            [480, 360, 30],
            [480, 270, 30],
            [320, 240, 30],
            [320, 240, 15],   // Apple Watch requires this configuration
            [320, 180, 30],
          ],
          codec: {
            profiles: [this.hap.H264Profile.BASELINE, this.hap.H264Profile.MAIN, this.hap.H264Profile.HIGH],
            levels: [this.hap.H264Level.LEVEL3_1, this.hap.H264Level.LEVEL3_2, this.hap.H264Level.LEVEL4_0],
          },
        },
        audio: {
          twoWayAudio: true,
          codecs: [
            {
              type: AudioStreamingCodecType.AAC_ELD,
              samplerate: AudioStreamingSamplerate.KHZ_16,
            },
          ],
        },
      },
    };
    
    this.controller = new this.hap.CameraController(options);
  }
  
  handleSnapshotRequest(request: SnapshotRequest, callback: SnapshotRequestCallback): void {
    console.log(request);
    throw new Error('Method not implemented.');
  }
  async prepareStream(request: PrepareStreamRequest, callback: PrepareStreamCallback): Promise<void> {
    console.log(request)
    const videoReturnPort = await getPort();
    const videoSSRC = this.hap.CameraController.generateSynchronisationSource();
    const audioReturnPort = await getPort();
    const audioSSRC = this.hap.CameraController.generateSynchronisationSource();

    const sessionInfo: SessionInfo = {
      address: request.targetAddress,

      videoPort: request.video.port,
      videoReturnPort: videoReturnPort,
      videoCryptoSuite: request.video.srtpCryptoSuite,
      videoSRTP: Buffer.concat([request.video.srtp_key, request.video.srtp_salt]),
      videoSSRC: videoSSRC,

      audioPort: request.audio.port,
      audioReturnPort: audioReturnPort,
      audioCryptoSuite: request.audio.srtpCryptoSuite,
      audioSRTP: Buffer.concat([request.audio.srtp_key, request.audio.srtp_salt]),
      audioSSRC: audioSSRC,
    };
    this.pendingSessions[request.sessionID] = sessionInfo;

    const response: PrepareStreamResponse = {
      video: {
        port: videoReturnPort,
        ssrc: videoSSRC,

        srtp_key: request.video.srtp_key,
        srtp_salt: request.video.srtp_salt,
      },

      audio: {
        port: audioReturnPort,
        ssrc: audioSSRC,

        srtp_key: request.audio.srtp_key,
        srtp_salt: request.audio.srtp_salt,
      },
    };
    
    callback(undefined, response);
  }
  handleStreamRequest(request: StreamingRequest, callback: StreamRequestCallback): void {
    console.log(request);
    const sessionInfo = this.pendingSessions[request.sessionID];
    console.log('Found sessionInfo', sessionInfo);

    switch (request.type) {
    case StreamRequestTypes.START:
      this.startStream(request, sessionInfo, callback);
      break;
    case StreamRequestTypes.STOP:
      callback(undefined);
      break;
    default:
      callback();
    }
  }

  private startStream(request: StartStreamRequest, sessionInfo: SessionInfo, callback: StreamRequestCallback): void {
    // 1. INPUT GENERATORS
    const videoInput = `-re -f lavfi -i color=c=red:s=${request.video.width}x${request.video.height}:r=${request.video.fps}`;

    // Replace anullsrc with sine wave generator
    // f=1000 sets the pitch to 1kHz
    const audioInput = `-i /Users/nisala/Downloads/mybad.m4a`;

    // 2. VIDEO ARGUMENTS
    const ffmpegVideoArgs = ` -map 0:0 -vcodec libx264 -pix_fmt yuvj420p -r ${request.video.fps} -f rawvideo -probesize 32 -analyzeduration 0 -fflags nobuffer -preset veryfast -refs 1 -x264-params intra-refresh=1:bframes=0 -b:v ${request.video.max_bit_rate}k -bufsize ${2 * request.video.max_bit_rate}k -maxrate ${request.video.max_bit_rate}k -payload_type ${request.video.pt}`;

    const ffmpegVideoStream = ` -ssrc ${sessionInfo.videoSSRC} -f rtp -srtp_out_suite AES_CM_128_HMAC_SHA1_80 -srtp_out_params ${sessionInfo.videoSRTP.toString('base64')} 'srtp://${sessionInfo.address}:${sessionInfo.videoPort}?rtcpport=${sessionInfo.videoPort}&localrtcpport=${sessionInfo.videoPort}&pkt_size=${videomtu}'`;

    // 3. AUDIO ARGUMENTS
    let ffmpegAudioFull = '';
    const ffmpegAudioArgs = ` -map 1:0 -acodec libfdk_aac -profile:a aac_eld -flags +global_header -f null -ar ${request.audio.sample_rate}k -b:a ${request.audio.max_bit_rate}k -bufsize ${2 * request.audio.max_bit_rate}k -ac ${request.audio.channel} -payload_type ${request.audio.pt}`;
    // const ffmpegAudioArgs = ` -map 1:0 -flags +global_header -f null -ar ${request.audio.sample_rate}k -b:a ${request.audio.max_bit_rate}k -bufsize ${2 * request.audio.max_bit_rate}k -ac 1 -payload_type ${request.audio.pt}`;
    

    const ffmpegAudioStream = ` -ssrc ${sessionInfo.audioSSRC} -f rtp -srtp_out_suite AES_CM_128_HMAC_SHA1_80 -srtp_out_params ${sessionInfo.audioSRTP.toString('base64')} 'srtp://${sessionInfo.address}:${sessionInfo.audioPort}?rtcpport=${sessionInfo.audioPort}&localrtcpport=${sessionInfo.audioPort}&pkt_size=${audiomtu}'`;
    // const ffmpegAudioStream = "";

    ffmpegAudioFull = `${ffmpegAudioArgs}${ffmpegAudioStream}`;

    // 4. FINAL ASSEMBLY
    // const debugFlag = this.platform.debugMode ? ' -loglevel debug' : '';
    const fcmd = `${videoInput} ${audioInput}${ffmpegVideoArgs}${ffmpegVideoStream}${ffmpegAudioFull}`;
    console.log('ffmpeg', fcmd);

    callback(undefined);
  }
};