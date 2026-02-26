import {
  CharacteristicEventTypes,
  CharacteristicGetCallback,
  Service,
  type HAP,
  type PlatformAccessory,
} from "homebridge";
import type { DigitalIntercomPlatform } from "../platform.js";
import { IntercomStreamingDelegate } from "../streamingDelegate.js";

export const INTERCOM_ACCESSORY_NAME = "Intercom";

export class IntercomPlatformAccessory {
  hap: HAP;
  private accessory: PlatformAccessory;
  private streamingDelegate: IntercomStreamingDelegate;
  private doorbellService: Service;

  constructor(
    private readonly platform: DigitalIntercomPlatform,
    private readonly paccessory: PlatformAccessory,
  ) {
    this.hap = this.platform.api.hap;
    this.accessory = paccessory;

    // Doorbell
    this.doorbellService =
      this.accessory.getService(this.hap.Service.Doorbell) ||
      this.accessory.addService(this.hap.Service.Doorbell);

    this.doorbellService.setPrimaryService(true);

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

    // Streaming
    this.streamingDelegate = new IntercomStreamingDelegate(this, platform);
    this.accessory.configureController(this.streamingDelegate.controller);
  }

  public triggerDoorbell() {
    this.doorbellService
      .getCharacteristic(this.hap.Characteristic.ProgrammableSwitchEvent)
      .setValue(this.hap.Characteristic.ProgrammableSwitchEvent.SINGLE_PRESS);
  }
}
