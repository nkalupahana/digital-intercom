import { Characteristic, CharacteristicEventTypes, CharacteristicGetCallback, type HAP, type PlatformAccessory } from 'homebridge';
import type { ExampleHomebridgePlatform } from './platform.js';
import { IntercomStreamingDelegate } from './streamingDelegate.js';
import { acquireService } from 'homebridge-plugin-utils';
export class ExamplePlatformAccessory {
  hap: HAP;
  private accessory: PlatformAccessory;
  private streamingDelegate: IntercomStreamingDelegate;

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

    this.accessory.addService(doorbellService)
      .getCharacteristic(this.hap.Characteristic.ProgrammableSwitchEvent)
      .on(CharacteristicEventTypes.GET, (callback: CharacteristicGetCallback) => {
        // HomeKit wants this to always be null.
        callback(null, null);
      });

    // Test doorbell
    // setTimeout(() => {
    //   console.log("DING DONG");
    //   doorbellService.getCharacteristic(this.hap.Characteristic.ProgrammableSwitchEvent).setValue(this.hap.Characteristic.ProgrammableSwitchEvent.SINGLE_PRESS);
    // }, 5000);
  }
}
