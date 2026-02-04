import {
  AudioStreamingCodecType,
  AudioStreamingSamplerate,
  PrepareStreamResponse,
  SRTPCryptoSuites,
  StartStreamRequest,
  StopStreamRequest,
  StreamRequestTypes,
  type CameraController,
  type CameraControllerOptions,
  type CameraStreamingDelegate,
  type HAP,
  type PrepareStreamCallback,
  type PrepareStreamRequest,
  type SnapshotRequest,
  type SnapshotRequestCallback,
  type StreamingRequest,
  type StreamRequestCallback,
} from "homebridge";
import { DigitalIntercomPlatformAccessory } from "./platformAccessory.js";
import getPort from "get-port";
import { ChildProcess, exec } from "node:child_process";
import {
  FfmpegCodecs,
  FfmpegOptions,
  RtpDemuxer,
  RtpPortAllocator,
} from "homebridge-plugin-utils";
import { Command } from "./constants.js";
import sharp from "sharp";

const videomtu = 188 * 5;
const audiomtu = 188 * 1;
const pathToFfmpeg = "/Users/nisala/.nix-profile/bin/ffmpeg";
const ONE_SECOND = 1000;

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

interface ActiveSession {
  sessionID: string;
  startTime: number;
  ffmpegProcess: ChildProcess;
  returnFfmpegProcess: ChildProcess;
  microphoneMuted: boolean | null;
  rtpDemuxer: RtpDemuxer;
}

export class IntercomStreamingDelegate implements CameraStreamingDelegate {
  private accessory: DigitalIntercomPlatformAccessory;
  private hap: HAP;
  private pendingSessions: Record<string, SessionInfo> = {};
  private activeSession: ActiveSession | null = null;
  private readonly rtpPorts = new RtpPortAllocator();
  private ffmpegOptions = new FfmpegOptions({
    codecSupport: new FfmpegCodecs({ log: console }),
    hardwareDecoding: true,
    hardwareTranscoding: true,
    log: console,
    name: () => "Intercom Streaming Delegate",
  });
  controller: CameraController;

  constructor(paccessory: DigitalIntercomPlatformAccessory) {
    this.accessory = paccessory;
    this.hap = paccessory.hap;

    const options: CameraControllerOptions = {
      cameraStreamCount: 2, // HomeKit requires at least 2 streams, and HomeKit Secure Video requires 1.
      delegate: this,
      streamingOptions: {
        supportedCryptoSuites: [
          this.hap.SRTPCryptoSuites.AES_CM_128_HMAC_SHA1_80,
        ],
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
            [320, 240, 15], // Apple Watch requires this configuration
            [320, 180, 30],
          ],
          codec: {
            profiles: [
              this.hap.H264Profile.BASELINE,
              this.hap.H264Profile.MAIN,
              this.hap.H264Profile.HIGH,
            ],
            levels: [
              this.hap.H264Level.LEVEL3_1,
              this.hap.H264Level.LEVEL3_2,
              this.hap.H264Level.LEVEL4_0,
            ],
          },
        },
        audio: {
          twoWayAudio: true,
          codecs: [
            // TODO: changing these values seems to do nothing
            {
              type: AudioStreamingCodecType.AAC_ELD,
              samplerate: AudioStreamingSamplerate.KHZ_16,
            },
          ],
        },
      },
    };

    this.controller = new this.hap.CameraController(options);

    this.controller.on("microphone-change", () => {
      if (this.activeSession === null) {
        console.error("No session currently active");
      } else if (this.activeSession.microphoneMuted === null) {
        this.activeSession.microphoneMuted = true;
      } else {
        this.activeSession.microphoneMuted =
          !this.activeSession.microphoneMuted;
      }

      console.log("Microphone muted", this.activeSession?.microphoneMuted);
      if (this.activeSession?.microphoneMuted) {
        this.accessory.sendCommand(Command.LISTEN_ON);
      } else {
        this.accessory.sendCommand(Command.TALK_ON);
      }

      this.controller.setMicrophoneMuted(true);
    });
  }

  handleSnapshotRequest(
    request: SnapshotRequest,
    callback: SnapshotRequestCallback,
  ): void {
    console.log("Received snapshot request", request);
    sharp("assets/snapshot.png")
      .resize(request.width, request.height, { fit: "cover" })
      .toBuffer()
      .then((buffer) => {
        callback(undefined, buffer);
      });
  }
  async prepareStream(
    request: PrepareStreamRequest,
    callback: PrepareStreamCallback,
  ): Promise<void> {
    console.log("Prepare stream request", request);
    const videoReturnPort = await getPort();
    const videoSSRC = this.hap.CameraController.generateSynchronisationSource();
    const audioSSRC = this.hap.CameraController.generateSynchronisationSource();

    let reservePortFailed = false;
    const rtpPortReservations: number[] = [];
    const reservePort = async (
      ipFamily: "ipv4" | "ipv6" = "ipv4",
      portCount: 1 | 2 = 1,
    ): Promise<number> => {
      // If we've already failed, don't keep trying to find more ports.
      if (reservePortFailed) {
        return -1;
      }

      // Retrieve the ports we're looking for.
      const assignedPort = await this.rtpPorts.reserve(ipFamily, portCount);

      // We didn't get the ports we requested.
      if (assignedPort === -1) {
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

    const audioIncomingRtcpPort = await reservePort(request.addressVersion);
    const audioIncomingPort = await reservePort(request.addressVersion);
    const audioIncomingRtpPort = await reservePort(request.addressVersion, 2);
    const rtpDemuxer = new RtpDemuxer(
      request.addressVersion,
      audioIncomingPort,
      audioIncomingRtcpPort,
      audioIncomingRtpPort,
      console,
    );

    const sessionInfo: SessionInfo = {
      address: request.targetAddress,
      addressVersion: request.addressVersion,

      videoPort: request.video.port,
      videoReturnPort: videoReturnPort,
      videoCryptoSuite: request.video.srtpCryptoSuite,
      videoSRTP: Buffer.concat([
        request.video.srtp_key,
        request.video.srtp_salt,
      ]),
      videoSSRC: videoSSRC,
      rtpDemuxer,

      audioPort: request.audio.port,
      audioCryptoSuite: request.audio.srtpCryptoSuite,
      audioSRTP: Buffer.concat([
        request.audio.srtp_key,
        request.audio.srtp_salt,
      ]),
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
    console.log("Prepared stream response", response);

    callback(undefined, response);
  }
  handleStreamRequest(
    request: StreamingRequest,
    callback: StreamRequestCallback,
  ): void {
    const sessionInfo = this.pendingSessions[request.sessionID];

    switch (request.type) {
      case StreamRequestTypes.START:
        this.startStream(request, sessionInfo, callback);
        break;
      case StreamRequestTypes.STOP:
        this.stopStream(request, callback);
        break;
      default:
        console.error("Unknown stream request type", request.type);
        callback(undefined);
    }
  }

  private async startStream(
    request: StartStreamRequest,
    sessionInfo: SessionInfo,
    callback: StreamRequestCallback,
  ): Promise<void> {
    if (this.activeSession !== null) {
      // TODO: it would be better to actually check the stream status,
      // e.g. if it is transmitting data, and if not, kill it.
      if (this.activeSession.startTime + ONE_SECOND * 120 < Date.now()) {
        console.log("Stream already active, killing old session");
        this.stopStream(
          {
            sessionID: this.activeSession.sessionID,
            type: StreamRequestTypes.STOP,
          },
          callback,
        );
      } else {
        const stopped = await new Promise((resolve) => {
          (async () => {
            for (let i = 0; i < 30; i++) {
              await new Promise((innerResolve) =>
                setTimeout(innerResolve, ONE_SECOND),
              );
              if (!this.activeSession) {
                resolve(true);
              }

              if (
                this.activeSession &&
                this.activeSession.startTime + ONE_SECOND * 120 < Date.now()
              ) {
                this.stopStream(
                  {
                    sessionID: this.activeSession.sessionID,
                    type: StreamRequestTypes.STOP,
                  },
                  callback,
                );
                resolve(true);
              }
            }
            resolve(false);
          })();
        });

        if (!stopped) {
          console.error("Stream already active, not starting new stream");
          callback(new Error("Stream already active"));
          return;
        }
      }
    }

    console.log("Starting stream", request);
    this.accessory.sendCommand(Command.LISTEN_ON);
    const shell = process.platform === "win32" ? "powershell" : undefined;
    // 1. INPUT GENERATORS
    const videoInput = `-re -loop 1 -i assets/snapshot.png -r ${request.video.fps}`;

    // Replace anullsrc with sine wave generator
    // f=1000 sets the pitch to 1kHz
    const audioInput =
      `-fflags nobuffer -flags low_delay -analyzeduration 0 -probesize 32 ` +
      // `-f lavfi -i "sine=frequency=1000:sample_rate=16000"`;
      `-ac 1 -f u16le -ar 32000 -i "udp://0.0.0.0:9999?pkt_size=1024&fifo_size=1000000&overrun_nonfatal=0"`;

    // 2. VIDEO ARGUMENTS
    const ffmpegVideoArgs = ` -map 0:0 -vcodec libx264 -pix_fmt yuvj420p -r ${request.video.fps} -f rawvideo -probesize 32 -analyzeduration 0 -fflags nobuffer -preset veryfast -refs 1 -x264-params intra-refresh=1:bframes=0 -b:v ${request.video.max_bit_rate}k -bufsize ${2 * request.video.max_bit_rate}k -maxrate ${request.video.max_bit_rate}k -payload_type ${request.video.pt}`;

    const ffmpegVideoStream = ` -ssrc ${sessionInfo.videoSSRC} -f rtp -srtp_out_suite AES_CM_128_HMAC_SHA1_80 -srtp_out_params ${sessionInfo.videoSRTP.toString("base64")} 'srtp://${sessionInfo.address}:${sessionInfo.videoPort}?rtcpport=${sessionInfo.videoPort}&localrtcpport=${sessionInfo.videoPort}&pkt_size=${videomtu}'`;

    // 3. AUDIO ARGUMENTS
    let ffmpegAudioFull = "";
    const ffmpegAudioArgs = ` -map 1:0 -acodec libfdk_aac -profile:a aac_eld -flags +global_header -f null -ar ${request.audio.sample_rate}k -b:a ${request.audio.max_bit_rate}k -bufsize ${2 * request.audio.max_bit_rate}k -ac ${request.audio.channel} -payload_type ${request.audio.pt}`;
    // const ffmpegAudioArgs = ` -map 1:0 -flags +global_header -f null -ar ${request.audio.sample_rate}k -b:a ${request.audio.max_bit_rate}k -bufsize ${2 * request.audio.max_bit_rate}k -ac 1 -payload_type ${request.audio.pt}`;

    const ffmpegAudioStream = ` -ssrc ${sessionInfo.audioSSRC} -f rtp -srtp_out_suite AES_CM_128_HMAC_SHA1_80 -srtp_out_params ${sessionInfo.audioSRTP.toString("base64")} 'srtp://${sessionInfo.address}:${sessionInfo.audioPort}?rtcpport=${sessionInfo.audioPort}&localrtcpport=${sessionInfo.audioPort}&pkt_size=${audiomtu}'`;
    // const ffmpegAudioStream = "";

    ffmpegAudioFull = `${ffmpegAudioArgs}${ffmpegAudioStream}`;

    // 4. FINAL ASSEMBLY
    // const debugFlag = this.platform.debugMode ? ' -loglevel debug' : '';
    const fcmd = `${videoInput} ${audioInput}${ffmpegVideoArgs}${ffmpegVideoStream}${ffmpegAudioFull}`;
    const ffmpegProcess = exec(
      `${pathToFfmpeg} ${fcmd}`,
      { shell },
      // (error, stdout, stderr) => {
      //   console.log("error", error);
      //   console.log("stdout", stdout);
      //   console.log("stderr", stderr);
      // },
    );

    const sdpIpVersion = sessionInfo.addressVersion === "ipv6" ? "IP6" : "IP4";

    console.log("Return sample rate", request.audio.sample_rate);
    const sdpReturnAudio = [
      "v=0",
      "o=- 0 0 IN " + sdpIpVersion + " 127.0.0.1",
      "s=" + "Akash Audio Talkback",
      "c=IN " + sdpIpVersion + " " + sessionInfo.address,
      "t=0 0",
      "m=audio " +
        sessionInfo.audioIncomingRtpPort.toString() +
        " RTP/AVP " +
        request.audio.pt.toString(),
      "b=AS:24",
      "a=rtpmap:110 MPEG4-GENERIC/" +
        (request.audio.sample_rate === AudioStreamingSamplerate.KHZ_16
          ? "16000"
          : "24000") +
        "/" +
        request.audio.channel.toString(),
      "a=fmtp:110 profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3; config=" +
        (request.audio.sample_rate === AudioStreamingSamplerate.KHZ_16
          ? "F8F0212C00BC00"
          : "F8EC212C00BC00"),
      "a=crypto:1 AES_CM_128_HMAC_SHA1_80 inline:" +
        sessionInfo.audioSRTP.toString("base64"),
    ].join("\n");

    const ffmpegReturnAudioCmd = [
      "-hide_banner",
      "-nostats",
      "-protocol_whitelist",
      "crypto,file,pipe,rtp,udp",
      "-f",
      "sdp",
      "-codec:a",
      this.ffmpegOptions.audioDecoder,
      "-i",
      "pipe:0",
      "-map",
      "0:a:0",
      ...this.ffmpegOptions.audioEncoder(),
      "-flags",
      "+global_header",
      "-ar",
      "16000", //this.protectCamera.ufp.talkbackSettings.samplingRate.toString(),
      // "-b:a",
      // request.audio.max_bit_rate.toString() + "k",
      "-ac",
      "1", //this.protectCamera.ufp.talkbackSettings.channels.toString(),
      "-f",
      "s16le",
      `udp://${this.accessory.getSocketAddress()}:9997?pkt_size=1024`,
    ];

    // TODO: handle no socket address

    const returnFfmpegProcess = exec(
      `${pathToFfmpeg} ${ffmpegReturnAudioCmd.join(" ")}`,
      { shell },
      // (error, stdout, stderr) => {
      //   console.log("error", error);
      //   console.log("stdout", stdout);
      //   console.log("stderr", stderr);
      // },
    );
    returnFfmpegProcess.stdin?.end(sdpReturnAudio + "\n");

    this.activeSession = {
      sessionID: request.sessionID,
      startTime: Date.now(),
      ffmpegProcess,
      returnFfmpegProcess,
      microphoneMuted: null,
      rtpDemuxer: sessionInfo.rtpDemuxer,
    };

    this.controller.setMicrophoneMuted(true);
    callback(undefined);
  }

  private stopStream(
    request: StopStreamRequest,
    callback: StreamRequestCallback,
  ): void {
    console.log("Stopping stream", request);
    this.accessory.sendCommand(Command.LISTEN_STOP);
    if (this.activeSession === null) {
      console.error("No session currently active");
      callback();
      return;
    }

    if (this.activeSession.sessionID !== request.sessionID) {
      console.warn(
        "Session ID mismatch",
        this.activeSession.sessionID,
        request.sessionID,
      );
    }

    this.activeSession.ffmpegProcess.kill();
    this.activeSession.returnFfmpegProcess.kill();
    this.activeSession.rtpDemuxer.close();
    this.activeSession = null;

    // TODO: set intercom back to idle
    callback(undefined);
  }
}

// TODO: setInterval to check if the stream is still active, and if not, stop it and clean up
