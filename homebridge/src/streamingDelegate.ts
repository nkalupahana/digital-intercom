import { AudioStreamingCodecType, AudioStreamingSamplerate, PrepareStreamResponse, SRTPCryptoSuites, StartStreamRequest, StreamRequestTypes, type CameraController, type CameraControllerOptions, type CameraStreamingDelegate, type HAP, type PrepareStreamCallback, type PrepareStreamRequest, type SnapshotRequest, type SnapshotRequestCallback, type StreamingRequest, type StreamRequestCallback } from 'homebridge';
import { ExamplePlatformAccessory } from './platformAccessory.js';
import getPort from 'get-port';
import { exec } from 'node:child_process';
import { FfmpegCodecs, FfmpegOptions, Nullable, RtpDemuxer, RtpPortAllocator } from 'homebridge-plugin-utils';
import { stderr } from 'node:process';

const videomtu = 188 * 5;
const audiomtu = 188 * 1;

type SessionInfo = {
  address: string; // Address of the HAP controller.
  addressVersion: "ipv4" | "ipv6";

  videoPort: number;
  videoReturnPort: number;
  videoCryptoSuite: SRTPCryptoSuites; // This should be saved if multiple suites are supported.
  videoSRTP: Buffer; // Key and salt concatenated.
  videoSSRC: number; // RTP synchronisation source.

  audioPort: number;
  audioCryptoSuite: SRTPCryptoSuites;
  audioSRTP: Buffer;
  audioSSRC: number;
  audioIncomingRtpPort: number;
  audioIncomingRtcpPort: number;
  audioIncomingPort: number;

  rtpDemuxer: RtpDemuxer;
};

export class IntercomStreamingDelegate implements CameraStreamingDelegate {
  private accessory: ExamplePlatformAccessory;
  private hap: HAP;
  private pendingSessions: Record<string, SessionInfo> = {};
  private readonly rtpPorts = new RtpPortAllocator();
  private ffmpegOptions = new FfmpegOptions({
    codecSupport: new FfmpegCodecs({ log: console }),
    hardwareDecoding: true,
    hardwareTranscoding: true,
    log: console,
    name: () => "Intercom Streaming Delegate",
  });
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
    console.log(request);
    const videoReturnPort = await getPort();
    const videoSSRC = this.hap.CameraController.generateSynchronisationSource();
    const audioSSRC = this.hap.CameraController.generateSynchronisationSource();

    let reservePortFailed = false;
    const rtpPortReservations: number[] = [];
    const reservePort = async (ipFamily: ("ipv4" | "ipv6") = "ipv4", portCount: (1 | 2) = 1): Promise<number> => {
      // If we've already failed, don't keep trying to find more ports.
      if (reservePortFailed) {
        return -1;
      }

      // Retrieve the ports we're looking for.
      const assignedPort = await this.rtpPorts.reserve(ipFamily, portCount);

      // We didn't get the ports we requested.
      if(assignedPort === -1) {
        reservePortFailed = true;
      } else {
        // Add this reservation the list of ports we've successfully requested.
        rtpPortReservations.push(assignedPort);
        if (portCount === 2) {
          rtpPortReservations.push(assignedPort + 1);
        }
      }

      // Return them.
      return assignedPort;
    };

    const audioIncomingRtcpPort = (await reservePort(request.addressVersion));
    const audioIncomingPort = await reservePort(request.addressVersion);
    const audioIncomingRtpPort = await reservePort(request.addressVersion, 2);
    const rtpDemuxer = new RtpDemuxer(request.addressVersion, audioIncomingPort, audioIncomingRtcpPort, audioIncomingRtpPort, console);

    const sessionInfo: SessionInfo = {
      address: request.targetAddress,
      addressVersion: request.addressVersion,

      videoPort: request.video.port,
      videoReturnPort: videoReturnPort,
      videoCryptoSuite: request.video.srtpCryptoSuite,
      videoSRTP: Buffer.concat([request.video.srtp_key, request.video.srtp_salt]),
      videoSSRC: videoSSRC,
      rtpDemuxer,

      audioPort: request.audio.port,
      audioCryptoSuite: request.audio.srtpCryptoSuite,
      audioSRTP: Buffer.concat([request.audio.srtp_key, request.audio.srtp_salt]),
      audioSSRC: audioSSRC,
      audioIncomingRtpPort: audioIncomingRtpPort,
      audioIncomingRtcpPort: audioIncomingRtcpPort,
      audioIncomingPort: audioIncomingPort,
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
        port: audioIncomingPort,
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
    const audioInput = `-fflags nobuffer -flags low_delay -analyzeduration 0 -probesize 32 -f u16le -ar 22050 -ac 1 -i "udp://0.0.0.0:9999?pkt_size=1024&fifo_size=1000000&overrun_nonfatal=1"`;

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
    const ret = exec(`/Users/nisala/.nix-profile/bin/ffmpeg ${fcmd}`);
    console.log('created process', ret.pid);

    const sdpIpVersion = sessionInfo.addressVersion === "ipv6" ? "IP6" : "IP4";

    const sdpReturnAudio = [
      "v=0",
      "o=- 0 0 IN " + sdpIpVersion + " 127.0.0.1",
      "s=" + "Akash Audio Talkback",
      "c=IN " + sdpIpVersion + " " + sessionInfo.address,
      "t=0 0",
      "m=audio " + sessionInfo.audioIncomingRtpPort.toString() + " RTP/AVP " + request.audio.pt.toString(),
      "b=AS:24",
      "a=rtpmap:110 MPEG4-GENERIC/" + ((request.audio.sample_rate === AudioStreamingSamplerate.KHZ_16) ? "16000" : "24000") + "/" + request.audio.channel.toString(),
      "a=fmtp:110 profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3; config=" +
        ((request.audio.sample_rate === AudioStreamingSamplerate.KHZ_16) ? "F8F0212C00BC00" : "F8EC212C00BC00"),
      "a=crypto:1 AES_CM_128_HMAC_SHA1_80 inline:" + sessionInfo.audioSRTP.toString("base64")
    ].join("\n");

    const ffmpegReturnAudioCmd = [
      "-hide_banner",
      "-nostats",
      "-protocol_whitelist", "crypto,file,pipe,rtp,udp",
      "-f", "sdp",
      "-codec:a", this.ffmpegOptions.audioDecoder,
      "-i", "pipe:0",
      "-map", "0:a:0",
      ...this.ffmpegOptions.audioEncoder(),
      "-flags", "+global_header",
      "-ar", "44100",//this.protectCamera.ufp.talkbackSettings.samplingRate.toString(),
      "-b:a", request.audio.max_bit_rate.toString() + "k",
      "-ac", "1", //this.protectCamera.ufp.talkbackSettings.channels.toString(),
      "-f", "mp3",
      "/tmp/output.mp3"
    ];

    const ret2 = exec(`/Users/nisala/.nix-profile/bin/ffmpeg ${ffmpegReturnAudioCmd.join(" ")}`, (error, stdout, stderr) => {
      console.log('error', error);
      console.log('stdout', stdout);
      console.log('stderr', stderr);
    });
    console.log('created return process', ret2.pid);
    ret2.stdin?.end(sdpReturnAudio + "\n");

    setTimeout(() => {
      console.log("process still running?", ret2.killed);
      console.log('killing return process');
      ret2.kill();
    }, 5000);

    callback(undefined);
  }
};