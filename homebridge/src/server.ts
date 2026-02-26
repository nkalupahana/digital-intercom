import net from "net";
import {
  Command,
  CREDIT_CARD_DATA_LEN,
  DigitalIntercomPlatformConfig,
  HEARTBEAT_INTERVAL,
  IntercomEventType,
} from "./constants.js";
import { Logging } from "homebridge";
import { DigitalIntercomPlatform } from "./platform.js";

export class Server {
  private socket: net.Socket | null = null;
  private log: Logging;
  private config: DigitalIntercomPlatformConfig;
  private platform: DigitalIntercomPlatform;

  constructor(platform: DigitalIntercomPlatform) {
    this.platform = platform;
    this.log = platform.log;
    this.config = platform.config;

    this.startServer();
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
      this.platform.triggerDoorbell();
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
      const allowedCard = this.config.allowedCards.find(
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
}
