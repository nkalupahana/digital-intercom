import {
  Characteristic,
  CharacteristicEventTypes,
  CharacteristicGetCallback,
  CharacteristicValue,
  Logging,
  type HAP,
  type PlatformAccessory,
} from "homebridge";
import type { ExampleHomebridgePlatform } from "./platform.js";
import { IntercomStreamingDelegate } from "./streamingDelegate.js";
import { acquireService } from "homebridge-plugin-utils";
import net from "net";

enum Command {
  OPEN_DOOR = "D",
  LISTEN_ON = "L",
  LISTEN_STOP = "S",
}

export class ExamplePlatformAccessory {
  hap: HAP;
  private log: Logging;
  private accessory: PlatformAccessory;
  private streamingDelegate: IntercomStreamingDelegate;
  private socket: net.Socket | null = null;

  sendCommand(cmd: Command) {
    if (this.socket === null) {
      this.log.error("Cannot send command because no TCP client connected");
      return;
    }
    this.socket.write(cmd);
  }

  startServer() {
    const server = net.createServer((socket) => {
      this.socket = socket;
      this.log.info(
        "Client connected:",
        socket.remoteAddress,
        socket.remotePort,
      );

      socket.on("data", (data) => {
        this.log.info("Received:", data.toString());
      });

      socket.on("close", () => {
        this.log.error("Client disconnected");
        this.socket = null;
      });

      socket.on("error", (err) => {
        this.log.error("Socket error:", err.message);
        this.socket = null;
      });
    });

    server.maxConnections = 1;
    server.listen(9998, "0.0.0.0", () => {
      this.log.info(`TCP server listening on 0.0.0.0:9998`);
    });
  }

  constructor(
    private readonly platform: ExampleHomebridgePlatform,
    private readonly paccessory: PlatformAccessory,
  ) {
    this.log = this.platform.log;
    this.hap = this.platform.api.hap;
    this.accessory = paccessory;

    /// Configure doorbell
    // Clear out any previous doorbell service.
    let doorbellService = this.accessory.getService(this.hap.Service.Doorbell);
    let switchService = this.accessory.getService(this.hap.Service.Switch);

    if (doorbellService) {
      this.accessory.removeService(doorbellService);
    }
    if (switchService) {
      this.accessory.removeService(switchService);
    }

    doorbellService = new this.hap.Service.Doorbell(this.accessory.displayName);
    doorbellService.setPrimaryService(true);
    this.streamingDelegate = new IntercomStreamingDelegate(this);
    this.accessory.configureController(this.streamingDelegate.controller);

    this.accessory
      .addService(doorbellService)
      .getCharacteristic(this.hap.Characteristic.ProgrammableSwitchEvent)
      .on(
        CharacteristicEventTypes.GET,
        (callback: CharacteristicGetCallback) => {
          // HomeKit wants this to always be null.
          callback(null, null);
        },
      );

    switchService = new this.hap.Service.Switch("Door");
    this.accessory
      .addService(switchService)
      .getCharacteristic(this.hap.Characteristic.On)
      .onGet(() => 0)
      .onSet((value) => {
        this.log.info("Setting door", value);
        if (value) {
          this.sendCommand(Command.OPEN_DOOR);
        }
      });

    this.startServer();

    // Test doorbell
    // setTimeout(() => {
    //   this.log.info("DING DONG");
    //   doorbellService.getCharacteristic(this.hap.Characteristic.ProgrammableSwitchEvent).setValue(this.hap.Characteristic.ProgrammableSwitchEvent.SINGLE_PRESS);
    // }, 5000);
  }
}
