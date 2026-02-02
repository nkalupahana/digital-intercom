import {
  Characteristic,
  CharacteristicEventTypes,
  CharacteristicGetCallback,
  type HAP,
  type PlatformAccessory,
} from "homebridge";
import type { ExampleHomebridgePlatform } from "./platform.js";
import { IntercomStreamingDelegate } from "./streamingDelegate.js";
import { acquireService } from "homebridge-plugin-utils";
import net from "net";

export class ExamplePlatformAccessory {
  hap: HAP;
  private accessory: PlatformAccessory;
  private streamingDelegate: IntercomStreamingDelegate;
  private socket: net.Socket | null = null;

  startServer() {
    const server = net.createServer((socket) => {
      this.socket = socket;
      console.log("Client connected:", socket.remoteAddress, socket.remotePort);

      socket.on("data", (data) => {
        console.log("Received:", data.toString());
      });

      socket.on("close", () => {
        console.log("Client disconnected");
        this.socket = null;
      });

      socket.on("error", (err) => {
        console.error("Socket error:", err.message);
        this.socket = null;
      });
    });

    server.maxConnections = 1;
    server.listen(9998, "0.0.0.0", () => {
      console.log(`TCP server listening on 0.0.0.0:9998`);
    });
  }

  constructor(
    private readonly platform: ExampleHomebridgePlatform,
    private readonly paccessory: PlatformAccessory,
  ) {
    this.hap = this.platform.api.hap;
    this.accessory = paccessory;

    /// Configure doorbell
    // Clear out any previous doorbell service.
    let doorbellService = this.accessory.getService(this.hap.Service.Doorbell);

    if (doorbellService) {
      this.accessory.removeService(doorbellService);
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

    this.startServer();

    // Test doorbell
    // setTimeout(() => {
    //   console.log("DING DONG");
    //   doorbellService.getCharacteristic(this.hap.Characteristic.ProgrammableSwitchEvent).setValue(this.hap.Characteristic.ProgrammableSwitchEvent.SINGLE_PRESS);
    // }, 5000);
  }
}
