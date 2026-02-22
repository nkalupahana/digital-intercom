import {
  CharacteristicEventTypes,
  CharacteristicGetCallback,
  Logging,
  Service,
  type HAP,
  type PlatformAccessory,
} from "homebridge";
import type { DigitalIntercomPlatform } from "./platform.js";
import { IntercomStreamingDelegate } from "./streamingDelegate.js";
import net from "net";
import {
  Command,
  CREDIT_CARD_DATA_LEN,
  HEARTBEAT_INTERVAL,
  IntercomEventType,
} from "./constants.js";

export class DigitalIntercomPlatformAccessory {
  hap: HAP;
  private log: Logging;
  private accessory: PlatformAccessory;
  private streamingDelegate: IntercomStreamingDelegate;
  private socket: net.Socket | null = null;
  private doorbellService: Service | null = null;

  getSocketAddress() {
    if (this.socket === null) {
      return null;
    }
    return this.socket.remoteAddress ?? null;
  }

  sendCommand(cmd: Command) {
    if (this.socket === null) {
      this.log.error("Cannot send command because no TCP client connected");
      return;
    }
    this.socket.write(cmd);
  }

  onIntercomData(data: Buffer<ArrayBuffer>): void {
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
      return;
    } else if (eventType === IntercomEventType.CREDIT_CARD) {
      console.log("Got credit card event", data);
      const messageLength = CREDIT_CARD_DATA_LEN + 1;
      if (data.length !== messageLength) {
        console.log(
          "Invalid credit card data length",
          data.length,
          messageLength,
        );
        return;
      }
      const creditCardData = data.subarray(1, messageLength);
      const hash = creditCardData.toString("hex");
      console.log("Got credit card data", hash);
      const allowedCard = this.platform.config.allowedCards.find(
        (card) => card.hash === hash,
      );
      if (!allowedCard) {
        console.log("Card not allowed", hash);
        return;
      } else {
        console.log("Card allowed", hash, allowedCard.description);
        this.socket?.write(Command.OPEN_DOOR);
      }
      return;
    } else {
      console.log("Invalid eventType", eventType, data);
      return;
    }
  }

  startServer() {
    const server = net.createServer((socket) => {
      if (this.socket !== null) {
        this.log.warn("Client already connected, destroying old connection");
        this.socket.destroy();
      }

      this.socket = socket;
      this.log.info(
        "Client connected:",
        socket.remoteAddress,
        socket.remotePort,
      );

      socket.on("data", (data) => {
        this.onIntercomData(data);
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
    setInterval(() => {
      this.socket?.write(Command.HEARTBEAT);
    }, HEARTBEAT_INTERVAL);
  }

  constructor(
    private readonly platform: DigitalIntercomPlatform,
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
