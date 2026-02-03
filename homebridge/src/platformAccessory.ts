import {
  CharacteristicEventTypes,
  CharacteristicGetCallback,
  Logging,
  Service,
  type HAP,
  type PlatformAccessory,
} from "homebridge";
import type { ExampleHomebridgePlatform } from "./platform.js";
import { IntercomStreamingDelegate } from "./streamingDelegate.js";
import net from "net";

// These should be kept in sync with the C++ code
enum Command {
  OPEN_DOOR = "D",
  LISTEN_ON = "L",
  LISTEN_STOP = "S",
}
enum IntercomEventType {
  BUZZER = "B",
  CREDIT_CARD = "C",
}
const CREDIT_CARD_DATA_LEN = 8;

export class ExamplePlatformAccessory {
  hap: HAP;
  private log: Logging;
  private accessory: PlatformAccessory;
  private streamingDelegate: IntercomStreamingDelegate;
  private socket: net.Socket | null = null;
  private doorbellService: Service | null = null;

  sendCommand(cmd: Command) {
    if (this.socket === null) {
      this.log.error("Cannot send command because no TCP client connected");
      return;
    }
    this.socket.write(cmd);
  }

  onIntercomData(data: Buffer<ArrayBuffer>) {
    const eventType = String.fromCharCode(data[0]);
    if (eventType === IntercomEventType.BUZZER) {
      if (this.doorbellService) {
        this.doorbellService
          .getCharacteristic(this.hap.Characteristic.ProgrammableSwitchEvent)
          .setValue(
            this.hap.Characteristic.ProgrammableSwitchEvent.SINGLE_PRESS,
          );
      } else {
        this.log.error("Unable ring doorbell because doorbellService is null");
      }
      return data.subarray(1);
    } else if (eventType === IntercomEventType.CREDIT_CARD) {
      const end = CREDIT_CARD_DATA_LEN + 1;
      if (data.length < end) {
        return null;
      }
      const creditCardData = data.subarray(1, CREDIT_CARD_DATA_LEN);
      console.log("Got credit card data", creditCardData);
      return data.subarray(end);
    } else {
      console.log("Invalid eventType", eventType, data);
      return null;
    }
  }

  startServer() {
    const server = net.createServer((socket) => {
      this.socket = socket;
      this.log.info(
        "Client connected:",
        socket.remoteAddress,
        socket.remotePort,
      );

      let buffer = Buffer.from([]);
      socket.on("data", (data) => {
        buffer = Buffer.concat([buffer, data]);
        while (true) {
          const newBuffer = this.onIntercomData(buffer);
          if (newBuffer === null) {
            break;
          }
          buffer = newBuffer;
        }
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
    this.doorbellService =
      this.accessory.getService(this.hap.Service.Doorbell) ?? null;
    let switchService = this.accessory.getService(this.hap.Service.Switch);

    if (this.doorbellService) {
      this.accessory.removeService(this.doorbellService);
    }
    if (switchService) {
      this.accessory.removeService(switchService);
    }

    this.doorbellService = new this.hap.Service.Doorbell(
      this.accessory.displayName,
    );
    this.doorbellService.setPrimaryService(true);
    this.streamingDelegate = new IntercomStreamingDelegate(this);
    this.accessory.configureController(this.streamingDelegate.controller);

    this.accessory
      .addService(this.doorbellService)
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
  }
}
